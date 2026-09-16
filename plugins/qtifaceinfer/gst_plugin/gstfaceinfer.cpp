/// @file gstfaceinfer.cpp
/// @brief GstFaceInfer plugin — registry / GObject plumbing.
///
/// GstBaseTransform in-place. transform_ip is a FAST path only (see
/// gstfaceinfer_fastpath.cpp); blocking vendor calls (clFinish / graphExecute
/// / SCRFD decode) run on a private GstTask worker (see
/// gstfaceinfer_lifecycle.cpp). The worker and the mailbox NEVER hold the
/// decoder GstBuffer — only face-owned, preallocated, hard-capped DMA slots.
///
/// Fail-closed: STALE/UNAVAILABLE results make the overlay probe paint a
/// full-frame white mask. Worker inference failures NEVER raise fatal errors
/// (WARNING + UNAVAILABLE only); only genuine main-chain errors (exceptions
/// inside transform_ip itself) go GST_ELEMENT_ERROR + GST_FLOW_ERROR.

#include "gstfaceinfer.h"
#include "gstfaceinfer_priv.h"

#include <cstring>
#include <memory>
#include <new>
#include <string>

GST_DEBUG_CATEGORY_STATIC (gst_face_infer_debug);
#define GST_CAT_DEFAULT gst_face_infer_debug

/* Properties */
enum {
    PROP_0,
    PROP_CONFIG_PATH,
    PROP_BACKEND_DEVICE_ID,
    PROP_STALE_TTL_MS,
    PROP_POOL_SIZE,
    PROP_STAGE_LOG,
#ifdef FACE_ASYNC_TEST_HOOKS
    PROP_DEBUG_INFER_DELAY_MS,
    PROP_DEBUG_INFER_GATE,
#endif
};

/* Pad templates: NV12 in, NV12 out (passthrough format) */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, format=(string)NV12"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, format=(string)NV12"));

#define gst_face_infer_parent_class parent_class
G_DEFINE_TYPE_WITH_PRIVATE (GstFaceInfer, gst_face_infer, GST_TYPE_BASE_TRANSFORM)

static void gst_face_infer_set_property (GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec);
static void gst_face_infer_get_property (GObject *object, guint prop_id,
    GValue *value, GParamSpec *pspec);
static void gst_face_infer_finalize (GObject *object);

/* Cross-TU accessor: G_DEFINE_TYPE's parent_class is static to this file. */
GstBaseTransformClass *
gst_face_infer_parent_class_peek (void)
{
    return GST_BASE_TRANSFORM_CLASS (gst_face_infer_parent_class);
}

static void
gst_face_infer_class_init (GstFaceInferClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
    GstBaseTransformClass *transform_class = GST_BASE_TRANSFORM_CLASS (klass);

    gobject_class->set_property = gst_face_infer_set_property;
    gobject_class->get_property = gst_face_infer_get_property;
    gobject_class->finalize = gst_face_infer_finalize;

    g_object_class_install_property (gobject_class, PROP_CONFIG_PATH,
        g_param_spec_string ("config-path", "Config Path",
            "Path to face_config.json",
            "", (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property (gobject_class, PROP_BACKEND_DEVICE_ID,
        g_param_spec_int ("backend-device-id", "Backend Device Id",
            "HTP core id for SCRFD (0 or 1). -1 = default 0",
            -1, 7, -1,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property (gobject_class, PROP_STALE_TTL_MS,
        g_param_spec_uint ("stale-ttl-ms", "Stale TTL ms",
            "Result age limit in ms; beyond it the probe paints a full-frame "
            "white mask (fail-closed). Best-effort within the window.",
            10, 10000, 250,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property (gobject_class, PROP_POOL_SIZE,
        g_param_spec_uint ("pool-size", "Pool Size",
            "Face-owned DMA slot count (hard cap). Builder sets it to "
            "cameras-in-group + 2.",
            2, 16, 6,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property (gobject_class, PROP_STAGE_LOG,
        g_param_spec_boolean ("stage-log", "Stage Log",
            "Diagnostic stage tracking (hb worker= / stuck_ms=). No watchdog.",
            FALSE, (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

#ifdef FACE_ASYNC_TEST_HOOKS
    /* Test-build only: injected worker delay / cancellable gate (scenarios
     * C/D/E). The production build does not contain these properties. */
    g_object_class_install_property (gobject_class, PROP_DEBUG_INFER_DELAY_MS,
        g_param_spec_int ("debug-infer-delay-ms", "Debug Infer Delay ms",
            "TEST HOOK: post-inference delay for the worker (0 = off)",
            0, 60000, 0,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property (gobject_class, PROP_DEBUG_INFER_GATE,
        g_param_spec_boolean ("debug-infer-gate", "Debug Infer Gate",
            "TEST HOOK: cancellable worker gate before inference (released by "
            "stop/flush)",
            FALSE, (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
#endif

    gst_element_class_set_static_metadata (element_class,
        "QTI Face Inference (async)",
        "Filter/Analyzer/Video",
        "Async SCRFD face detection + ArcFace recognition (OpenCL + QNN HTP); "
        "video main path never waits for inference",
        "multi_cam_app");

    gst_element_class_add_static_pad_template (element_class, &sink_template);
    gst_element_class_add_static_pad_template (element_class, &src_template);

    transform_class->start = GST_DEBUG_FUNCPTR (gst_face_infer_start_impl);
    transform_class->stop = GST_DEBUG_FUNCPTR (gst_face_infer_stop_impl);
    transform_class->sink_event =
        GST_DEBUG_FUNCPTR (gst_face_infer_sink_event_impl);
    transform_class->set_caps = GST_DEBUG_FUNCPTR (gst_face_infer_set_caps_impl);
    transform_class->transform_ip =
        GST_DEBUG_FUNCPTR (gst_face_infer_transform_ip_impl);

    /* In-place mode: framework makes the buffer writable so we can attach
     * metas. Video data is zero-copy (DMABuf refcount+1). */
    transform_class->passthrough_on_same_caps = FALSE;

    GST_DEBUG_CATEGORY_INIT (gst_face_infer_debug, "qtifaceinfer", 0,
        "QTI Face Inference");
}

static void
gst_face_infer_init (GstFaceInfer *self)
{
    self->priv = (GstFaceInferPrivate *)
        gst_face_infer_get_instance_private (self);
    new (self->priv) GstFaceInferPrivate ();
    GstFaceInferPrivate *priv = self->priv;

    priv->stage_reporter = std::make_shared<face_async::StageReporter> (
        []() -> std::int64_t { return g_get_monotonic_time (); });
    priv->result_store = std::make_shared<face_async::ResultStore> (
        []() -> std::int64_t { return g_get_monotonic_time (); });
    /* FaceWorker is created in start_impl AFTER all properties are final
       (pool-size / stale-ttl-ms are NULL/READY-only) so the SlotPool size
       always matches the final pool-size. */

    gst_base_transform_set_in_place (GST_BASE_TRANSFORM (self), TRUE);
}

static void
gst_face_infer_finalize (GObject *object)
{
    GstFaceInfer *self = GST_FACE_INFER (object);
    if (self->priv) {
        try {
            if (self->priv->task) {
                gst_object_unref (self->priv->task);
                self->priv->task = nullptr;
            }
            self->priv->~GstFaceInferPrivate ();
        } catch (...) {
            GST_WARNING_OBJECT (self,
                "Exception in ~GstFaceInferPrivate, leaking to avoid crash");
        }
    }
    G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_face_infer_set_property (GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec)
{
    GstFaceInfer *self = GST_FACE_INFER (object);
    GstFaceInferPrivate *priv = self->priv;

    /* Structural properties are NULL/READY-only: the worker/SlotPool are
     * built from them at start, so changing them afterwards would silently
     * desync the pool size / TTL. */
    const gboolean mutable_phase = (GST_STATE (self) == GST_STATE_NULL ||
                                    GST_STATE (self) == GST_STATE_READY);

    switch (prop_id) {
        case PROP_CONFIG_PATH:
            if (mutable_phase)
                priv->config_path = g_value_get_string (value);
            else
                GST_WARNING_OBJECT (self,
                    "config-path ignored: only settable in NULL/READY");
            break;
        case PROP_BACKEND_DEVICE_ID:
            if (mutable_phase)
                priv->backend_device_id = g_value_get_int (value);
            else
                GST_WARNING_OBJECT (self,
                    "backend-device-id ignored: only settable in NULL/READY");
            break;
        case PROP_STALE_TTL_MS:
            if (mutable_phase)
                priv->stale_ttl_ms = g_value_get_uint (value);
            else
                GST_WARNING_OBJECT (self,
                    "stale-ttl-ms ignored: only settable in NULL/READY");
            break;
        case PROP_POOL_SIZE:
            if (mutable_phase)
                priv->pool_size = g_value_get_uint (value);
            else
                GST_WARNING_OBJECT (self,
                    "pool-size ignored: only settable in NULL/READY");
            break;
        case PROP_STAGE_LOG:
            priv->stage_log = g_value_get_boolean (value);
            break;
#ifdef FACE_ASYNC_TEST_HOOKS
        case PROP_DEBUG_INFER_DELAY_MS:
            priv->debug_infer_delay_ms = g_value_get_int (value);
            break;
        case PROP_DEBUG_INFER_GATE:
            priv->debug_infer_gate = g_value_get_boolean (value);
            break;
#endif
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static void
gst_face_infer_get_property (GObject *object, guint prop_id,
    GValue *value, GParamSpec *pspec)
{
    GstFaceInfer *self = GST_FACE_INFER (object);
    GstFaceInferPrivate *priv = self->priv;
    switch (prop_id) {
        case PROP_CONFIG_PATH:
            g_value_set_string (value, priv->config_path.c_str ());
            break;
        case PROP_BACKEND_DEVICE_ID:
            g_value_set_int (value, priv->backend_device_id);
            break;
        case PROP_STALE_TTL_MS:
            g_value_set_uint (value, priv->stale_ttl_ms);
            break;
        case PROP_POOL_SIZE:
            g_value_set_uint (value, priv->pool_size);
            break;
        case PROP_STAGE_LOG:
            g_value_set_boolean (value, priv->stage_log);
            break;
#ifdef FACE_ASYNC_TEST_HOOKS
        case PROP_DEBUG_INFER_DELAY_MS:
            g_value_set_int (value, priv->debug_infer_delay_ms);
            break;
        case PROP_DEBUG_INFER_GATE:
            g_value_set_boolean (value, priv->debug_infer_gate);
            break;
#endif
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static gboolean
plugin_init (GstPlugin *plugin)
{
    return gst_element_register (plugin, "qtifaceinfer",
        GST_RANK_NONE, GST_TYPE_FACE_INFER);
}

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    qtifaceinfer,
    "QTI Face Detection + Recognition (async SCRFD + ArcFace)",
    plugin_init,
    "1.0",
    "Proprietary",
    "multi_cam_app",
    "https://www.qualcomm.com"
)
