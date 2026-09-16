/// @file gstfaceinfer_fastpath.cpp
/// @brief qtifaceinfer FAST path: transform_ip + worker fn + copy + attach + hb.
///
/// transform_ip NEVER blocks on inference:
///   read (camera_id, frame_id, PTS) -> acquire face-owned pool slot
///   -> bounded row-by-row DMA copy (decoder fd: CPU READ sync, slot fd:
///      CPU WRITE sync; BOTH guards surround the memcpy only, never vendor
///      calls) -> commit into the per-camera capacity-1 mailbox (re-checks
///      the five-flag accepting state + generations + slot state under the
///      state lock) -> attach the LATEST valid result (GstFaceResultStateMeta
///      + GstFaceDetectionMeta for VALID_FACES) -> return immediately.
///
/// Fail-closed: STALE/UNAVAILABLE results make the overlay probe paint a
/// full-frame white mask. Worker inference failures NEVER raise fatal errors
/// (WARNING + UNAVAILABLE only); only genuine main-chain errors (exceptions
/// inside transform_ip itself) go GST_ELEMENT_ERROR + GST_FLOW_ERROR.

#include "gstfaceinfer.h"
#include "gstfaceinfer_priv.h"
#include "gstfacedetectionmeta.h"
#include "gstfaceresultstatemeta.h"
#include "gstaiboxframemeta.h"

#include "../async/frame_layout.hpp"
#include "../mem_management/dma_sync_guard.hpp"

#include <gst/allocators/gstfdmemory.h>
#include <gst/video/video.h>

#include <linux/dma-buf.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define FACE_HB_INTERVAL_US (5 * G_TIME_SPAN_SECOND)

/* ------------------------------------------------------------------ */
/* Main-chain fail-closed (transform_ip internal errors ONLY)          */
/* ------------------------------------------------------------------ */
GstFlowReturn
gst_face_infer_fail_closed (GstFaceInfer *self, const char *why)
{
    GstFaceInferPrivate *priv = self->priv;
    if (priv) priv->n_err++;
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("%s", why),
        ("qtifaceinfer fail-closed"));
    fprintf (stderr, "[faceinfer] FAIL_CLOSED %s\n", why);
    return GST_FLOW_ERROR;
}

/* ------------------------------------------------------------------ */
/* Worker: the ONLY place blocking vendor calls run                    */
/* ------------------------------------------------------------------ */
static std::vector<face_async::FaceBox>
face_infer_fn (const face_async::TaskRecord &task, void *user_data)
{
    auto *priv = static_cast<GstFaceInferPrivate *> (user_data);
    std::vector<face_async::FaceBox> out;
    if (!priv || !priv->processor || !priv->processor_initialized)
        return out;
    if (task.slot_idx >= priv->slots.size ())
        return out;

    face_infer::DmaBuffer &slot = *priv->slots[task.slot_idx];
    const face_infer::FramePlaneInfo &plane = priv->slot_planes[task.slot_idx];
    const cl_mem nv12_cl = slot.cl_mem_handle ();
    if (!nv12_cl)
        return out;

    const gint64 t0 = g_get_monotonic_time ();
    /* clFinish / graphExecute / DmaSync+memcpy / SCRFD decode all run here */
    std::vector<face_infer::FaceResult> faces =
        priv->processor->ProcessFrame (nv12_cl, plane);
    {
        std::lock_guard<std::mutex> lk (priv->rings_mtx);
        priv->infer_ring.add (g_get_monotonic_time () - t0);
        priv->n_iframes++;
    }

    out.reserve (faces.size ());
    for (const auto &fr : faces) {
        face_async::FaceBox b;
        b.x1 = fr.det.x1;
        b.y1 = fr.det.y1;
        b.x2 = fr.det.x2;
        b.y2 = fr.det.y2;
        b.score = fr.det.score;
        for (int k = 0; k < face_infer::kFaceNumLandmarks; ++k) {
            b.landmarks[k * 2]     = fr.det.landmarks[k][0];
            b.landmarks[k * 2 + 1] = fr.det.landmarks[k][1];
        }
        if (priv->processor->HasArcFace ()) {
            b.embedding.assign (fr.embedding,
                fr.embedding + face_infer::kFaceEmbeddingDim);
        }
        out.push_back (std::move (b));
    }
    return out;
}

void
gst_face_infer_worker_loop (gpointer user_data)
{
    auto *self = GST_FACE_INFER (user_data);
    GstFaceInferPrivate *priv = self->priv;
    if (!priv->worker)
        return;
    priv->worker->worker_loop (face_infer_fn, priv);
}

/* ------------------------------------------------------------------ */
/* Result attach: latest per-camera snapshot (best-effort, TTL-gated)  */
/* ------------------------------------------------------------------ */
gboolean
gst_face_infer_attach_result_state (GstFaceInfer *self, GstBuffer *buffer,
    const std::string &cam, guint64 video_frame_id)
{
    /* Expert isolation ladder (2026-09-06 §5): AIBOX_FACE_META_MODE selects
     * which metas get attached while inference + DMA copy still run:
     *   0 = attach NOTHING (no state meta, no detection meta)
     *   1 = attach only the small GstFaceResultStateMeta
     *   2 = attach both (default, production behavior)
     * Read once per process; combine with AIBOX_FACE_MASKS_OFF=1 in the
     * Builder probe to also skip the overlay masks setter. */
    static int meta_mode = -1;
    if (meta_mode < 0) {
        const char *m = g_getenv ("AIBOX_FACE_META_MODE");
        meta_mode = (m && m[0] >= '0' && m[0] <= '2') ? (m[0] - '0') : 2;
    }
    if (meta_mode == 0)
        return TRUE;

    GstFaceInferPrivate *priv = self->priv;

    gint64 age_us = -1;
    auto entry = priv->worker
        ? priv->worker->latest_result (cam, &age_us)
        : std::shared_ptr<const face_async::FaceResultEntry> ();

    GstFaceResultStateMeta *sm =
        gst_buffer_add_face_result_state_meta (buffer);
    if (!sm) {
        GST_ERROR_OBJECT (self, "failed to attach GstFaceResultStateMeta");
        return FALSE;
    }
    sm->abi_version = FACE_RESULT_STATE_META_ABI_VERSION;
    sm->struct_size = sizeof (GstFaceResultStateMeta);
    sm->applied_video_frame_id = video_frame_id;
    sm->frame_width  = GST_VIDEO_INFO_WIDTH (&priv->vinfo);
    sm->frame_height = GST_VIDEO_INFO_HEIGHT (&priv->vinfo);
    sm->result_age_ms = (entry && age_us >= 0)
        ? static_cast<guint32> (age_us / 1000) : 0;

    if (!entry ||
        entry->status == face_async::ResultStatus::UNAVAILABLE) {
        sm->result_status = FACE_RESULT_STATE_UNAVAILABLE;
        return TRUE;
    }

    sm->detection_frame_id = entry->det_frame_id;
    sm->caps_gen   = entry->caps_gen;
    sm->stream_gen = entry->stream_gen;

    if (entry->status == face_async::ResultStatus::VALID_EMPTY ||
        entry->faces.empty ()) {
        sm->result_status = FACE_RESULT_STATE_VALID_EMPTY;
        return TRUE;
    }

    sm->result_status = FACE_RESULT_STATE_VALID_FACES;
    if (meta_mode == 1)
        return TRUE;   /* isolation ladder: state meta only */
    GstFaceDetectionMeta *dm = gst_buffer_add_face_detection_meta (buffer);
    if (!dm) {
        GST_ERROR_OBJECT (self, "failed to attach GstFaceDetectionMeta");
        return FALSE;
    }
    const guint n = static_cast<guint> (std::min<size_t> (
        entry->faces.size (), FACE_META_MAX_FACES));
    dm->n_faces = n;
    const gboolean has_arc =
        (priv->processor && priv->processor->HasArcFace ()) ? TRUE : FALSE;
    for (guint i = 0; i < n; ++i) {
        const face_async::FaceBox &f = entry->faces[i];
        GstFaceDetectionEntry *e = &dm->faces[i];
        e->x1 = f.x1;
        e->y1 = f.y1;
        e->x2 = f.x2;
        e->y2 = f.y2;
        e->score = f.score;
        std::memcpy (e->landmarks, f.landmarks.data (), sizeof (e->landmarks));
        if (has_arc && f.embedding.size () == FACE_META_EMBEDDING_DIM) {
            std::memcpy (e->embedding, f.embedding.data (),
                sizeof (e->embedding));
        } else {
            std::memset (e->embedding, 0, sizeof (e->embedding));
        }
        e->has_embedding = has_arc;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Bounded row-by-row copy into the face-owned slot (no locks held)    */
/* ------------------------------------------------------------------ */
static gboolean
copy_nv12_to_slot (GstFaceInfer *self, GstBuffer *buffer, guint32 slot_idx)
{
    GstFaceInferPrivate *priv = self->priv;
    if (slot_idx >= priv->slots.size ())
        return FALSE;

    /* rate-limited diagnostics for copy rejections (every ~2s) */
    auto reject = [&](const char *why) {
        const gint64 now = g_get_monotonic_time ();
        if (now - priv->copy_err_last_us > 2 * G_TIME_SPAN_SECOND) {
            priv->copy_err_last_us = now;
            fprintf (stderr, "[faceinfer] copy reject: %s\n", why);
        }
    };

    /* this round: single memory, FD/DMA-BUF backed, both planes inside it */
    if (gst_buffer_n_memory (buffer) != 1) {
        reject ("n_memory != 1");
        return FALSE;
    }
    GstMemory *mem = gst_buffer_peek_memory (buffer, 0);
    if (!mem || !gst_is_fd_memory (mem)) {
        reject ("memory not FD-backed");
        return FALSE;
    }

    GstVideoFrame vframe;
    if (!gst_video_frame_map (&vframe, &priv->vinfo, buffer, GST_MAP_READ)) {
        reject ("gst_video_frame_map failed");
        return FALSE;
    }

    face_async::FrameLayout lay {};
    lay.width  = GST_VIDEO_FRAME_WIDTH (&vframe);
    lay.height = GST_VIDEO_FRAME_HEIGHT (&vframe);
    lay.y_offset  = GST_VIDEO_FRAME_PLANE_OFFSET (&vframe, 0);
    lay.y_stride  = GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, 0);
    lay.uv_offset = GST_VIDEO_FRAME_PLANE_OFFSET (&vframe, 1);
    lay.uv_stride = GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, 1);
    lay.buffer_size = gst_buffer_get_size (buffer);

    std::string err;
    if (!face_async::validate_frame_layout (lay, priv->slot_capacity, &err)) {
        char diag[256];
        snprintf (diag, sizeof (diag),
            "%s (w=%u h=%u y_off=%llu y_stride=%u uv_off=%llu uv_stride=%u "
            "buf=%llu cap=%llu)",
            err.c_str (), lay.width, lay.height,
            (unsigned long long)lay.y_offset, lay.y_stride,
            (unsigned long long)lay.uv_offset, lay.uv_stride,
            (unsigned long long)lay.buffer_size,
            (unsigned long long)priv->slot_capacity);
        reject (diag);
        gst_video_frame_unmap (&vframe);
        return FALSE;
    }

    /* independent TARGET slot layout (never reuse source offsets verbatim) */
    face_infer::FramePlaneInfo dst_plane {};
    dst_plane.fd = priv->slots[slot_idx]->fd ();
    dst_plane.frame_len = static_cast<uint32_t> (priv->slot_capacity);
    dst_plane.y_offset = 0;
    dst_plane.y_stride = lay.y_stride;
    dst_plane.uv_offset = static_cast<uint32_t> (
        static_cast<guint64> (lay.y_stride) * lay.height);
    dst_plane.uv_stride = lay.uv_stride;
    dst_plane.width  = lay.width;
    dst_plane.height = lay.height;

    const guint8 *src_y =
        static_cast<const guint8 *> (GST_VIDEO_FRAME_PLANE_DATA (&vframe, 0));
    const guint8 *src_uv =
        static_cast<const guint8 *> (GST_VIDEO_FRAME_PLANE_DATA (&vframe, 1));
    guint8 *dst = static_cast<guint8 *> (priv->slots[slot_idx]->data ());

    if (priv->stage_reporter) {
        priv->stage_reporter->enter (face_async::FaceStage::COPY_INPUT);
        priv->stage_reporter->enter (face_async::FaceStage::DMA_SYNC);
    }
    const gint64 t0 = g_get_monotonic_time ();
    {
        /* BOTH guards START before the first memcpy and END after ALL rows.
         * decoder fd: CPU READ; slot fd: CPU WRITE. Never spans vendor calls. */
        face_infer::DmaSyncGuard dec_guard (gst_fd_memory_get_fd (mem),
            DMA_BUF_SYNC_READ);
        face_infer::DmaSyncGuard slot_guard (priv->slots[slot_idx]->fd (),
            DMA_BUF_SYNC_WRITE);
        for (guint r = 0; r < lay.height; ++r) {
            std::memcpy (dst + static_cast<gsize> (r) * lay.y_stride,
                src_y + static_cast<gsize> (r) * lay.y_stride, lay.y_stride);
        }
        const guint uv_rows = lay.height / 2;
        for (guint r = 0; r < uv_rows; ++r) {
            std::memcpy (dst + dst_plane.uv_offset +
                             static_cast<gsize> (r) * lay.uv_stride,
                src_uv + static_cast<gsize> (r) * lay.uv_stride, lay.uv_stride);
        }
    }
    {
        std::lock_guard<std::mutex> lk (priv->rings_mtx);
        priv->copy_ring.add (g_get_monotonic_time () - t0);
    }
    if (priv->stage_reporter) {
        priv->stage_reporter->leave (face_async::FaceStage::DMA_SYNC);
        priv->stage_reporter->leave (face_async::FaceStage::COPY_INPUT);
    }

    priv->slot_planes[slot_idx] = dst_plane;
    gst_video_frame_unmap (&vframe);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Heartbeat (diagnostic only — no watchdog, no auto-recovery)         */
/* ------------------------------------------------------------------ */
void
gst_face_infer_heartbeat (GstFaceInfer *self, gint64 now_us)
{
    GstFaceInferPrivate *priv = self->priv;
    priv->n_vframes++;
    if (priv->last_hb_us == 0)
        priv->last_hb_us = now_us;
    if (now_us - priv->last_hb_us < FACE_HB_INTERVAL_US)
        return;

    const double dt = (now_us - priv->last_hb_us) / 1000000.0;
    const guint64 dv = priv->n_vframes - priv->hb_vframes;
    const guint64 di = priv->n_iframes - priv->hb_iframes;

    const face_async::SlotCounters pc = priv->worker->pool_counters ();
    const face_async::MailboxCounters mc = priv->worker->mailbox_counters ();
    const face_async::WorkerCounters wc = priv->worker->counters ();

    gint64 copy_p50 = 0, copy_p95 = 0, copy_p99 = 0;
    gint64 tip_p50 = 0, tip_p95 = 0, tip_p99 = 0;
    gint64 inf_p50 = 0, inf_p95 = 0, inf_p99 = 0;
    {
        std::lock_guard<std::mutex> lk (priv->rings_mtx);
        copy_p50 = priv->copy_ring.pct (50);
        copy_p95 = priv->copy_ring.pct (95);
        copy_p99 = priv->copy_ring.pct (99);
        tip_p50 = priv->tip_ring.pct (50);
        tip_p95 = priv->tip_ring.pct (95);
        tip_p99 = priv->tip_ring.pct (99);
        inf_p50 = priv->infer_ring.pct (50);
        inf_p95 = priv->infer_ring.pct (95);
        inf_p99 = priv->infer_ring.pct (99);
    }

    const face_async::FaceStage stage = priv->stage_reporter
        ? priv->stage_reporter->current () : face_async::FaceStage::IDLE;
    gint64 stuck_ms = 0;
    if (priv->stage_reporter &&
        stage != face_async::FaceStage::IDLE) {
        const gint64 entered = priv->stage_reporter->current_enter_us ();
        if (entered > 0 && now_us > entered)
            stuck_ms = (now_us - entered) / 1000;
    }

    std::string results;
    for (const auto &cam : priv->worker->known_cameras ()) {
        gint64 age = -1;
        auto e = priv->worker->latest_result (cam, &age);
        if (!results.empty ()) results += " | ";
        results += cam + ":";
        if (!e)
            results += "UNAVAILABLE";
        else if (e->status == face_async::ResultStatus::UNAVAILABLE)
            results += "UNAVAILABLE";
        else if (e->status == face_async::ResultStatus::VALID_EMPTY)
            results += "VALID_EMPTY";
        else
            results += "VALID";
        if (age >= 0) results += "(age=" + std::to_string (age / 1000) + "ms)";
    }
    if (results.empty ()) results = "-";

    fprintf (stderr,
        "[faceinfer] hb vfps=%.1f ifps=%.1f pool=%u[f%llu,q%llu,i%llu] "
        "queue=%zu evict=%llu noslot=%llu err=%llu worker=%u stuck_ms=%lld "
        "results=%s copy_p50/95/99=%lld/%lld/%lldus "
        "tip_p50/95/99=%lld/%lld/%lldus infer_p50/95/99=%lld/%lld/%lldus\n",
        dt > 0.0 ? dv / dt : 0.0,
        dt > 0.0 ? di / dt : 0.0,
        priv->pool_size,
        (unsigned long long)pc.free_n, (unsigned long long)pc.queued,
        (unsigned long long)pc.inferencing,
        priv->worker->total_queue_depth (),
        (unsigned long long)mc.evicted, (unsigned long long)pc.noslot,
        (unsigned long long)priv->n_err + wc.errors,
        static_cast<unsigned>(stage), (long long)stuck_ms,
        results.c_str (),
        (long long)copy_p50, (long long)copy_p95, (long long)copy_p99,
        (long long)tip_p50, (long long)tip_p95, (long long)tip_p99,
        (long long)inf_p50, (long long)inf_p95, (long long)inf_p99);

    priv->last_hb_us = now_us;
    priv->hb_vframes = priv->n_vframes;
    priv->hb_iframes = priv->n_iframes;
}

/* ------------------------------------------------------------------ */
/* transform_ip FAST PATH: bounded copy + latest-result attach         */
/* ------------------------------------------------------------------ */
GstFlowReturn
gst_face_infer_transform_ip_impl (GstBaseTransform *base, GstBuffer *buffer)
{
    GstFaceInfer *self = GST_FACE_INFER (base);
    GstFaceInferPrivate *priv = self->priv;

    try {
        const gint64 tip_t0 = g_get_monotonic_time ();

        if (!priv->processor_initialized || !priv->processor ||
            !priv->worker) {
            /* not ready: fail-closed state meta (probe paints full white) */
            if (!gst_face_infer_attach_result_state (self, buffer,
                    std::string (), 0))
                return gst_face_infer_fail_closed (self,
                    "state meta attach failed (not initialized)");
            return GST_FLOW_OK;
        }

        const gint64 sample_us = g_get_monotonic_time ();

        std::string camera_id;
        guint64 frame_id = 0;
        GstAiboxFrameMeta *fm = gst_buffer_get_aibox_frame_meta (buffer);
        if (fm && fm->camera_id && fm->camera_id[0]) {
            camera_id = fm->camera_id;
            frame_id = fm->frame_id;
        } else {
            if (!priv->warned_no_cam) {
                priv->warned_no_cam = TRUE;
                GST_WARNING_OBJECT (self,
                    "buffer without camera_id: full-mask, no inference");
            }
            if (!gst_face_infer_attach_result_state (self, buffer,
                    camera_id, frame_id))
                return gst_face_infer_fail_closed (self,
                    "state meta attach failed (no camera)");
            return GST_FLOW_OK;
        }

        /* ① acquire slot: validates camera + five-flag accepting,
         *    FREE→COPYING, and captures the current EPOCH (generations live
         *    inside the worker's state mutex — commit compares against it).
         *    On refusal (no slot / not accepting / camera over cap) the video
         *    frame passes through with the latest (or UNAVAILABLE) result —
         *    never a wait. */
        guint32 slot_idx = 0;
        face_async::Epoch epoch;
        if (!priv->worker->try_acquire_slot (slot_idx, camera_id, &epoch)) {
            if (!gst_face_infer_attach_result_state (self, buffer,
                    camera_id, frame_id))
                return gst_face_infer_fail_closed (self,
                    "state meta attach failed (no slot)");
            {
                std::lock_guard<std::mutex> lk (priv->rings_mtx);
                priv->tip_ring.add (g_get_monotonic_time () - tip_t0);
            }
            gst_face_infer_heartbeat (self, sample_us);
            return GST_FLOW_OK;
        }

        /* ② bounded copy (no locks; both sync guards surround the memcpy) */
        if (!copy_nv12_to_slot (self, buffer, slot_idx)) {
            priv->worker->cancel_copy (slot_idx);
            priv->n_err++;
            if (!gst_face_infer_attach_result_state (self, buffer,
                    camera_id, frame_id))
                return gst_face_infer_fail_closed (self,
                    "state meta attach failed (copy error)");
            {
                std::lock_guard<std::mutex> lk (priv->rings_mtx);
                priv->tip_ring.add (g_get_monotonic_time () - tip_t0);
            }
            gst_face_infer_heartbeat (self, sample_us);
            return GST_FLOW_OK;
        }

        /* ③ commit: re-checks accepting + the task's CAPTURED epoch vs the
         *    lock-internal current epoch + slot state under the state lock.
         *    A FLUSH that completes fully inside the copy window bumps
         *    stream_gen => epoch mismatch => slot cancelled, task abandoned. */
        face_async::TaskRecord task;
        task.slot_idx = slot_idx;
        task.camera_id = camera_id;
        task.det_frame_id = frame_id;
        task.pts_ns = static_cast<guint64> (GST_BUFFER_PTS (buffer));
        task.caps_gen = epoch.caps_gen;
        task.stream_gen = epoch.stream_gen;
        task.sample_mono_us = sample_us;
        task.width  = GST_VIDEO_INFO_WIDTH (&priv->vinfo);
        task.height = GST_VIDEO_INFO_HEIGHT (&priv->vinfo);
        priv->worker->commit_task (task);

        /* ④ attach latest result (best-effort; STALE => UNAVAILABLE meta) */
        if (!gst_face_infer_attach_result_state (self, buffer,
                camera_id, frame_id))
            return gst_face_infer_fail_closed (self,
                "state meta attach failed");

        /* ⑤ diagnostics */
        {
            std::lock_guard<std::mutex> lk (priv->rings_mtx);
            priv->tip_ring.add (g_get_monotonic_time () - tip_t0);
        }
        gst_face_infer_heartbeat (self, sample_us);
        return GST_FLOW_OK;
    } catch (const std::bad_alloc &) {
        return gst_face_infer_fail_closed (self,
            "std::bad_alloc in transform_ip");
    } catch (const std::exception &e) {
        char why[256];
        snprintf (why, sizeof (why), "std::exception in transform_ip: %s",
            e.what ());
        return gst_face_infer_fail_closed (self, why);
    } catch (...) {
        return gst_face_infer_fail_closed (self,
            "unknown exception in transform_ip");
    }
}
