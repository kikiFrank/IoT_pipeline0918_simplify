/// @file gstfaceinfer_priv.h
/// @brief Shared private state + cross-file declarations for qtifaceinfer.
///
/// gstfaceinfer.cpp is split into three translation units to keep each file
/// focused: registry/plumbing, lifecycle (pool/task/flush), and the fast
/// path (transform_ip + copy + attach + heartbeat).

#ifndef GST_FACE_INFER_PRIV_H
#define GST_FACE_INFER_PRIV_H

#include "gstfaceinfer.h"

#include "../FaceProcessor.hpp"
#include "../async/face_worker.hpp"
#include "../async/result_store.hpp"
#include "../async/stage_reporter.hpp"
#include "../mem_management/dma_buffer.hpp"
#include "../mem_management/mem_types.hpp"

#include <gst/video/video.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/* ------------------------------------------------------------------ */
/* Latency ring: fixed 256-sample reservoir for p50/p95/p99 (diagnostics) */
/* ------------------------------------------------------------------ */
struct LatencyRing {
    static constexpr size_t kN = 256;
    gint64 samples[kN] = {};
    size_t count = 0;
    size_t pos = 0;

    void add (gint64 us) {
        samples[pos] = us;
        pos = (pos + 1) % kN;
        if (count < kN) count++;
    }
    gint64 pct (double p) const {
        if (count == 0) return 0;
        gint64 sorted[kN];
        std::memcpy (sorted, samples, sizeof (gint64) * count);
        std::sort (sorted, sorted + count);
        const size_t idx = (size_t)(p / 100.0 * (count - 1));
        return sorted[idx];
    }
};

struct _GstFaceInferPrivate {
    std::string config_path;
    gint backend_device_id = -1;
    std::unique_ptr<face_infer::FaceProcessor> processor;
    GstVideoInfo vinfo {};
    gboolean processor_initialized = FALSE;
    /* NOTE: caps/stream generations live INSIDE FaceWorker's state mutex —
       the plugin must never read/write its own generation counters. */

    /* async core */
    std::shared_ptr<face_async::ResultStore> result_store;
    std::unique_ptr<face_async::FaceWorker> worker;
    std::shared_ptr<face_async::StageReporter> stage_reporter;

    /* face-owned DMA pool (preallocated, hard-capped) */
    std::vector<std::unique_ptr<face_infer::DmaBuffer>> slots;
    std::vector<face_infer::FramePlaneInfo> slot_planes;
    guint64 slot_capacity = 0;

    guint32 pool_size = 6;
    guint32 stale_ttl_ms = 250;
    gboolean stage_log = FALSE;
#ifdef FACE_ASYNC_TEST_HOOKS
    gint debug_infer_delay_ms = 0;
    gboolean debug_infer_gate = FALSE;
#endif

    GstTask *task = nullptr;
    GRecMutex task_lock;
    gint64 copy_err_last_us = 0;

    /* counters / heartbeat */
    guint64 n_vframes = 0;
    guint64 n_iframes = 0;
    guint64 n_err = 0;
    gint64 last_hb_us = 0;
    guint64 hb_vframes = 0;
    guint64 hb_iframes = 0;

    std::mutex rings_mtx;
    LatencyRing copy_ring;
    LatencyRing tip_ring;
    LatencyRing infer_ring;

    gboolean warned_no_cam = FALSE;
};

/* ---- cross-file declarations (all C++ internal, no C ABI) ---- */

/* gstfaceinfer.cpp (the G_DEFINE_TYPE parent-class static lives there) */
GstBaseTransformClass *gst_face_infer_parent_class_peek (void);

/* fastpath.cpp */
GstFlowReturn gst_face_infer_fail_closed (GstFaceInfer *self,
    const char *why);
gboolean gst_face_infer_attach_result_state (GstFaceInfer *self,
    GstBuffer *buffer, const std::string &cam, guint64 video_frame_id);
void gst_face_infer_heartbeat (GstFaceInfer *self, gint64 now_us);
void gst_face_infer_worker_loop (gpointer user_data);
GstFlowReturn gst_face_infer_transform_ip_impl (GstBaseTransform *base,
    GstBuffer *buffer);

/* lifecycle.cpp */
gboolean gst_face_infer_start_impl (GstBaseTransform *base);
gboolean gst_face_infer_stop_impl (GstBaseTransform *base);
gboolean gst_face_infer_sink_event_impl (GstBaseTransform *base,
    GstEvent *event);
gboolean gst_face_infer_set_caps_impl (GstBaseTransform *base,
    GstCaps *incaps, GstCaps *outcaps);
gboolean gst_face_infer_allocate_pool (GstFaceInfer *self,
    guint64 slot_capacity);
guint64 gst_face_infer_slot_capacity_for_vinfo (const GstVideoInfo *vinfo);

#endif /* GST_FACE_INFER_PRIV_H */
