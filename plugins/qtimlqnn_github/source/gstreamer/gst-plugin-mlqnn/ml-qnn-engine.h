/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __QTI_ML_QNN_ENGINE_H__
#define __QTI_ML_QNN_ENGINE_H__

#include <gst/gst.h>
#include <gst/ml/ml-info.h>
#include <gst/ml/ml-frame.h>

G_BEGIN_DECLS

/**
 * GST_ML_QNN_ENGINE_OPT_MODEL:
 *
 * #G_TYPE_STRING, neural network model file path and name
 * Default: NULL
 */
#define GST_ML_QNN_ENGINE_OPT_MODEL "GstMLQNNEngine.model"

/**
 * GST_ML_QNN_ENGINE_OPT_BACKEND:
 *
 * #G_TYPE_STRING, set the delegate
 * Default: /usr/lib/libQnnCpu.so
 */
#define GST_ML_QNN_ENGINE_OPT_BACKEND "GstMLQNNEngine.backend"

/**
 * GST_ML_QNN_ENGINE_OPT_SYSLIB:
 *
 * #G_TYPE_STRING, QNN system library file path and name
 * Default: /usr/lib/libQnnSystem.so
 */
#define GST_ML_QNN_ENGINE_OPT_SYSLIB "GstMLQNNEngine.sysLib"

/**
 * GST_ML_QNN_ENGINE_OPT_BACKEND_DEVICE_ID:
 *
 * #G_TYPE_UINT, QNN backend device id
 * Default: 0
 */
#define GST_ML_QNN_ENGINE_OPT_BACKEND_DEVICE_ID "GstMLQNNEngine.backend_device_id"

/**
 * GST_ML_QNN_ENGINE_OPT_OUTPUTS:
 *
 * #G_TYPE_POINTER, list of output configurations
 * Default: NULL
 */
#define GST_ML_QNN_ENGINE_OPT_OUTPUTS "GstMLQNNEngine.outputs"

typedef enum {
  GST_ML_QNN_HTP_PERFORMANCE_MODE_DEFAULT,
  GST_ML_QNN_HTP_PERFORMANCE_MODE_EXTREME_POWER_SAVER,
  GST_ML_QNN_HTP_PERFORMANCE_MODE_LOW_POWER_SAVER,
  GST_ML_QNN_HTP_PERFORMANCE_MODE_POWER_SAVER,
  GST_ML_QNN_HTP_PERFORMANCE_MODE_HIGH_POWER_SAVER,
  GST_ML_QNN_HTP_PERFORMANCE_MODE_LOW_BALANCED,
  GST_ML_QNN_HTP_PERFORMANCE_MODE_BALANCED,
  GST_ML_QNN_HTP_PERFORMANCE_MODE_HIGH_PERFORMANCE,
  GST_ML_QNN_HTP_PERFORMANCE_MODE_SUSTAINED_HIGH_PERFORMANCE,
  GST_ML_QNN_HTP_PERFORMANCE_MODE_BURST,
} GstMLQnnHtpPerformanceMode;

GST_API GType gst_ml_qnn_htp_performance_mode_get_type (void);
#define GST_TYPE_ML_QNN_HTP_PERFORMANCE_MODE \
    (gst_ml_qnn_htp_performance_mode_get_type())

#define GST_ML_QNN_ENGINE_OPT_HTP_PERFORMANCE_MODE \
    "GstMLQNNEngine.htp_performance_mode"

#define GET_OPT_MODEL(s) \
  gst_structure_get_string (s, GST_ML_QNN_ENGINE_OPT_MODEL)
#define GET_OPT_BACKEND(s) \
  gst_structure_get_string (s, GST_ML_QNN_ENGINE_OPT_BACKEND)
#define GET_OPT_SYSLIB(s) \
  gst_structure_get_string (s, GST_ML_QNN_ENGINE_OPT_SYSLIB)

typedef struct _GstMLQnnEngine GstMLQnnEngine;

GST_API GstMLQnnEngine *
gst_ml_qnn_engine_new                 (GstStructure * settings);

GST_API void
gst_ml_qnn_engine_free                (GstMLQnnEngine * engine);

GST_API const GstMLInfo *
gst_ml_qnn_engine_get_input_info      (GstMLQnnEngine * engine);

GST_API const GstMLInfo *
gst_ml_qnn_engine_get_output_info     (GstMLQnnEngine * engine);

GST_API gboolean
gst_ml_qnn_engine_execute             (GstMLQnnEngine * engine,
                                       GstMLFrame * inframe,
                                       GstMLFrame * outframe);

G_END_DECLS

#endif // __QTI_ML_QNN_ENGINE_H__
