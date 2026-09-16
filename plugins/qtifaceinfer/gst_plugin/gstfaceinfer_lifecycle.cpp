/// @file gstfaceinfer_lifecycle.cpp
/// @brief qtifaceinfer lifecycle: set_caps / pool / start / stop / sink_event.
///
/// Five-flag accepting state machine (authority rules, no cross-restore):
///   running / processor_ready / caps_compatible / flushing / eos
///   FLUSH_START sets flushing ONLY (+drains QUEUED slots via the worker core)
///   FLUSH_STOP  clears flushing ONLY
///   EOS         sets eos ONLY (never waits for in-flight inference)
///   STREAM_START clears eos ONLY (never clears unpaired flushing)
///   caps-compat is restored ONLY by a normal stop/re-init cycle.
///
/// Runtime-incompatible caps NEVER join the worker / rebuild the pool: they
/// only stop new inference tasks (caps_compatible=FALSE) and the pipeline
/// stays fully masked until a controlled restart. The pool is destroyed only
/// in stop(), after the worker has been joined.
///
/// KNOWN LIMITATION: if the worker is permanently stuck inside a vendor call
/// (clFinish/graphExecute), the stop join hangs and the SIGTERM grace path
/// applies — an in-process thread cannot be cancelled safely. No in-flight
/// slot is ever force-reclaimed.

#include "gstfaceinfer_priv.h"

#include "../config/FaceConfigLoader.hpp"
#include "../mem_management/opencl_loader.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>

/* ------------------------------------------------------------------ */
/* Pool lifecycle                                                     */
/* ------------------------------------------------------------------ */
guint64
gst_face_infer_slot_capacity_for_vinfo (const GstVideoInfo *vinfo)
{
    /* Compact target layout with alignment headroom: real decoder buffers
     * may pad the strides (up to 128) and the row counts (up to 32) beyond
     * the negotiated caps — the face-owned slot must absorb that. */
    const guint64 y_stride =
        GST_ROUND_UP_128 (GST_VIDEO_INFO_PLANE_STRIDE (vinfo, 0));
    const guint64 uv_stride =
        GST_ROUND_UP_128 (GST_VIDEO_INFO_PLANE_STRIDE (vinfo, 1));
    const guint64 height = GST_VIDEO_INFO_HEIGHT (vinfo);
    const guint64 y_rows = GST_ROUND_UP_32 (height);
    const guint64 uv_rows = GST_ROUND_UP_16 (height / 2);
    return y_stride * y_rows + uv_stride * uv_rows;
}

gboolean
gst_face_infer_allocate_pool (GstFaceInfer *self, guint64 slot_capacity)
{
    GstFaceInferPrivate *priv = self->priv;
    auto ocl = face_infer::OpenClLoader::Get ();
    const cl_context ctx = priv->processor ? priv->processor->GetContext ()
                                           : nullptr;

    priv->slots.clear ();
    priv->slot_planes.clear ();
    priv->slots.reserve (priv->pool_size);
    priv->slot_planes.resize (priv->pool_size);

    for (guint32 i = 0; i < priv->pool_size; ++i) {
        auto slot = std::make_unique<face_infer::DmaBuffer> ();
        if (priv->stage_reporter)
            priv->stage_reporter->enter (face_async::FaceStage::OPENCL_IMPORT);
        const bool ok = slot->Init (slot_capacity) &&
                        (ocl && ctx) && slot->BindOpenCl (ctx, ocl);
        if (priv->stage_reporter)
            priv->stage_reporter->leave (face_async::FaceStage::OPENCL_IMPORT);
        if (!ok) {
            GST_ERROR_OBJECT (self, "pool slot %u alloc/bind failed (cap=%llu)",
                i, (unsigned long long)slot_capacity);
            priv->slots.clear ();
            priv->slot_planes.clear ();
            return FALSE;
        }
        priv->slots.push_back (std::move (slot));
    }
    priv->slot_capacity = slot_capacity;
    GST_INFO_OBJECT (self, "face pool allocated slots=%u cap=%llu",
        priv->pool_size, (unsigned long long)slot_capacity);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* set_caps: init processor + allocate the face-owned pool (once)      */
/* ------------------------------------------------------------------ */
gboolean
gst_face_infer_set_caps_impl (GstBaseTransform *base,
    GstCaps *incaps, GstCaps *outcaps)
{
    (void) outcaps;
    GstFaceInfer *self = GST_FACE_INFER (base);
    GstFaceInferPrivate *priv = self->priv;

    GstVideoInfo new_info;
    if (!gst_video_info_from_caps (&new_info, incaps)) {
        GST_ERROR_OBJECT (self, "Failed to parse video info from caps");
        return FALSE;
    }

    GST_INFO_OBJECT (self, "set_caps: %dx%d NV12",
        GST_VIDEO_INFO_WIDTH (&new_info), GST_VIDEO_INFO_HEIGHT (&new_info));

    /* generations live inside the worker's state mutex */
    const auto gens = priv->worker->bump_caps_and_stream_gen ();
    priv->vinfo = new_info;

    if (!priv->processor_initialized) {
        if (priv->config_path.empty ()) {
            GST_ERROR_OBJECT (self, "config-path property not set");
            return FALSE;
        }

        face_infer::FaceProcessorConfig cfg;
        if (!face_infer::LoadFaceConfig (priv->config_path, cfg)) {
            GST_ERROR_OBJECT (self, "Failed to load face config: %s",
                priv->config_path.c_str ());
            return FALSE;
        }
        if (priv->backend_device_id >= 0)
            cfg.backend_device_id = priv->backend_device_id;
        fprintf (stderr, "[faceinfer] init config-path=%s backend-device-id=%d\n",
                 priv->config_path.c_str (), cfg.backend_device_id);

        priv->processor = std::make_unique<face_infer::FaceProcessor> ();
        if (!priv->processor->Init (cfg)) {
            GST_ERROR_OBJECT (self, "FaceProcessor::Init failed");
            priv->processor.reset ();
            return FALSE;
        }
        priv->processor->SetStageCallback (
            [sr = priv->stage_reporter] (face_async::FaceStage s, bool enter) {
                if (sr) {
                    if (enter) sr->enter (s);
                    else       sr->leave (s);
                }
            });

        const guint64 cap = gst_face_infer_slot_capacity_for_vinfo (&priv->vinfo);
        if (!gst_face_infer_allocate_pool (self, cap))
            return FALSE;

        priv->processor_initialized = TRUE;
        priv->worker->set_processor_ready (TRUE);
        priv->worker->set_caps_compatible (TRUE);
        GST_INFO_OBJECT (self, "FaceProcessor initialized (ArcFace=%s)",
            priv->processor->HasArcFace () ? "on" : "off");
    } else {
        /* renegotiation: NEVER join the worker, NEVER rebuild the pool here */
        const guint64 cap = gst_face_infer_slot_capacity_for_vinfo (&priv->vinfo);
        if (cap > priv->slot_capacity) {
            priv->worker->set_caps_compatible (FALSE);
            GST_WARNING_OBJECT (self,
                "caps need %llu bytes > pool slot %llu: inference stopped, "
                "full mask until controlled restart",
                (unsigned long long)cap,
                (unsigned long long)priv->slot_capacity);
        }
    }

    priv->result_store->reset_epoch (gens.first, gens.second);
    priv->result_store->clear ();
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* start / stop: private GstTask lifecycle (QTI batch/metamux idiom)   */
/* ------------------------------------------------------------------ */
gboolean
gst_face_infer_start_impl (GstBaseTransform *base)
{
    GstFaceInfer *self = GST_FACE_INFER (base);
    GstFaceInferPrivate *priv = self->priv;

#ifdef FACE_ASYNC_TEST_HOOKS
    /* TEST BUILD ONLY: env overrides activate the debug hooks for the C/D/E
     * board scenarios (the pipeline builder does not set these properties). */
    const gchar *env_delay = g_getenv ("AIBOX_FACE_DEBUG_DELAY_MS");
    if (env_delay)
        priv->debug_infer_delay_ms = atoi (env_delay);
    if (g_getenv ("AIBOX_FACE_DEBUG_GATE"))
        priv->debug_infer_gate = TRUE;
    const gchar *env_pool = g_getenv ("AIBOX_FACE_DEBUG_POOL_SIZE");
    if (env_pool) {
        const gint p = atoi (env_pool);
        if (p >= 2 && p <= 16)
            priv->pool_size = static_cast<guint32> (p);
    }
#endif

    /* Build the async core from the FINAL property values (NULL/READY-only
     * properties guarantee they cannot change afterwards). */
    priv->worker = std::make_unique<face_async::FaceWorker> (
        priv->pool_size, priv->result_store,
        []() -> std::int64_t { return g_get_monotonic_time (); },
        static_cast<std::int64_t> (priv->stale_ttl_ms) * 1000);
    priv->worker->set_stage_reporter (priv->stage_reporter);
#ifdef FACE_ASYNC_TEST_HOOKS
    priv->worker->set_test_delay_ms (priv->debug_infer_delay_ms);
    if (priv->debug_infer_gate)
        priv->worker->set_gate (TRUE);
    fprintf (stderr,
        "[faceinfer] TEST_HOOKS delay_ms=%d gate=%d (test build)\n",
        priv->debug_infer_delay_ms, priv->debug_infer_gate ? 1 : 0);
#endif

    /* startup invariant: worker SlotPool size == requested pool-size */
    if (priv->worker->pool_size () != priv->pool_size) {
        GST_ERROR_OBJECT (self,
            "worker pool-size %u != requested %u",
            priv->worker->pool_size (), priv->pool_size);
        priv->worker.reset ();
        return FALSE;
    }

    priv->worker->set_running (TRUE);
    if (!priv->task) {
        priv->task = gst_task_new (gst_face_infer_worker_loop, self, NULL);
        /* GstTask REQUIRES a lock before it can start ("task without a lock
         * can't be set to state started"). QTI batch/metamux use the same
         * GRecMutex idiom; the loop itself uses its own internal locks. */
        gst_task_set_lock (priv->task, &priv->task_lock);
    }

    /* task_lock is owned by GstTask while the loop runs; take it only here */
    g_rec_mutex_lock (&priv->task_lock);
    gst_task_start (priv->task);
    g_rec_mutex_unlock (&priv->task_lock);
    fprintf (stderr,
        "[faceinfer] async worker started pool=%u worker_slots=%u ttl=%ums\n",
        priv->pool_size, priv->worker->pool_size (), priv->stale_ttl_ms);
    return TRUE;
}

gboolean
gst_face_infer_stop_impl (GstBaseTransform *base)
{
    GstFaceInfer *self = GST_FACE_INFER (base);
    GstFaceInferPrivate *priv = self->priv;

    /* ① stop new tasks + release the debug gate + drain ALL QUEUED slots
     *    (evict) under the state lock — nothing queued survives the stop */
    if (priv->worker)
        priv->worker->stop ();

    /* ② stop the task. KNOWN LIMITATION: see file header. */
    if (priv->task) {
        gst_task_stop (priv->task);
        gst_task_join (priv->task);
    }

    /* ③ verify the pool is FULLY idle; NEVER force states to match counters */
    if (priv->worker && priv->worker->verify_idle_and_reset ()) {
        /* ④ release cl_mem → munmap → close(fd), outside every lock */
        priv->slots.clear ();
        priv->slot_planes.clear ();
        priv->slot_capacity = 0;
    } else if (priv->worker) {
        GST_WARNING_OBJECT (self,
            "pool not idle at stop — leaking slots to avoid use-after-free");
    }

    /* ⑤ destroy processor (ArcFace → SCRFD QNN → OpenCL last) */
    if (priv->processor) {
        try {
            priv->processor->Destroy ();
            priv->processor.reset ();
        } catch (const std::exception &e) {
            GST_WARNING_OBJECT (self,
                "Exception in FaceProcessor::Destroy: %s — leaking", e.what ());
            (void)priv->processor.release ();
        } catch (...) {
            GST_WARNING_OBJECT (self,
                "Unknown exception in FaceProcessor::Destroy — leaking");
            (void)priv->processor.release ();
        }
    }
    priv->processor_initialized = FALSE;

    /* ⑥ clear results; the worker is recreated fresh on the next start */
    priv->result_store->clear ();
    GST_INFO_OBJECT (self, "stopped");
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* sink_event: five-flag authority rules (no cross-restore)            */
/* ------------------------------------------------------------------ */
gboolean
gst_face_infer_sink_event_impl (GstBaseTransform *base, GstEvent *event)
{
    GstFaceInfer *self = GST_FACE_INFER (base);
    GstFaceInferPrivate *priv = self->priv;

    if (priv->worker) {
        switch (GST_EVENT_TYPE (event)) {
            case GST_EVENT_FLUSH_START:
                /* flushing + ++stream_gen + drain QUEUED under ONE state
                 * lock section; never touches COPYING/INFERENCING, never
                 * waits for the worker. A copy in flight CANNOT commit
                 * afterwards (epoch mismatch inside the lock). */
            {
                const auto gens = priv->worker->flush_start_bump ();
                priv->result_store->reset_epoch (gens.first, gens.second);
                priv->result_store->clear ();
                break;
            }
            case GST_EVENT_FLUSH_STOP:
                priv->worker->flush_stop ();
                break;
            case GST_EVENT_EOS:
                /* eos only: never waits for in-flight inference */
                priv->worker->set_eos ();
                break;
            case GST_EVENT_STREAM_START:
                /* clears eos only + ++stream_gen; never clears unpaired
                 * flushing */
            {
                const auto gens = priv->worker->stream_start_bump ();
                priv->result_store->reset_epoch (gens.first, gens.second);
                break;
            }
            default:
                break;
        }
    }
    return gst_face_infer_parent_class_peek ()->sink_event (base, event);
}
