#ifndef __GST_AIBOX_FLOW_TAP_H__
#define __GST_AIBOX_FLOW_TAP_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_AIBOX_FLOW_TAP (gst_aibox_flow_tap_get_type())
G_DECLARE_FINAL_TYPE (GstAiboxFlowTap, gst_aibox_flow_tap, GST,
    AIBOX_FLOW_TAP, GstElement)

G_END_DECLS

#endif /* __GST_AIBOX_FLOW_TAP_H__ */
