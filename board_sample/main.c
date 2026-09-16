/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * Application:
 * AI based Multi Stream parallel inference on file inputs with per-stream
 * RTSP live streaming output.
 *
 * Description:
 * The application takes video files and runs Yolo inference on each stream
 * in parallel. Detection boxes are drawn on the video and each stream is
 * scaled back to 1080p, H.264 encoded and pushed to its own RTSP server so
 * that all channels can be observed live (e.g. VLC/ffplay) simultaneously.
 *
 * Pipeline for Gstreamer (per stream, linear chain):
 * filesrc -> qtdemux -> h264parse -> v4l2h264dec -> queue -> qtimlvconverter
 *   -> qtimlelement (Yolo) -> qtimlpostprocess (draw boxes) -> detection_filter
 *   -> qtivtransform (BGRA 640x360 -> NV12 1920x1080) -> v4l2h264enc
 *   -> h264parse (byte-stream) -> qtirtspbin (RTSP)
 */

#include <stdio.h>
#include <glib-unix.h>
#include <sys/resource.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#include <gst_sample_apps_utils.h>

/**
 * Default models and labels path, if not provided by user
 */
#define DEFAULT_TFLITE_YOLOX_MODEL "/etc/models/yolox_quantized.tflite"
#define DEFAULT_YOLOX_LABELS "/etc/labels/yolox.json"
#define DEFAULT_TFLITE_INCEPTIONV3_MODEL \
   "/etc/models/inception_v3_quantized.tflite"
#define DEFAULT_CLASSIFICATION_LABELS "/etc/labels/classification.json"

/**
 * Maximum count of file sources possible to configure
 */
#define MAX_FILESRCS 32

/**
 * Number of Queues used for buffer caching between elements
 */
#define QUEUE_COUNT 6

/**
 * Default threshold values
 */
#define DEFAULT_THRESHOLD_VALUE 40.0

/**
 * Default output filter width and height
 */
#define DEFAULT_FILTER_WIDTH 640
#define DEFAULT_FILTER_HEIGHT 360

/**
 * Default transform/output width and height
 */
#define DEFAULT_OUTPUT_WIDTH 1920
#define DEFAULT_OUTPUT_HEIGHT 1080
#define DEFAULT_FRAMERATE 30

/**
 * Default RTSP server listen address and base port
 */
#define DEFAULT_IP "0.0.0.0"
#define DEFAULT_PORT 8554

/**
 * Default path for config file
 */
#define DEFAULT_CONFIG_FILE "/etc/configs/config-multistream-inference.json"

/**
 * Structure for various application specific options
 */
typedef struct {
  gchar *mlframework;
  gchar *model_path;
  GstInputStreamType input_type;
  gchar *labels_path;
  gint num_file;
  gint use_case;
  gchar *input_file_path[MAX_FILESRCS];
  gchar *output_ip_address;
  gint output_port_base;
} GstAppOptions;

/*
 * Read HTP core count
 */
static gint
get_num_cdsp_backends ()
{
  gint num_cdsp_backends = 1;

  if (access ("/dev/fastrpc-cdsp1", F_OK) == 0)
    num_cdsp_backends = 2;

  return num_cdsp_backends;
}

/**
 * Set parameters for ML Framework Elements.
 *
 * @param qtimlelement ML Framework Plugin.
 * @param qtimlpostprocess ML Detection Plugin.
 * @param detection_filter Caps Filter Plugin for ML Detection Plugin.
 * @param options Application specific options.
 */
static gboolean __attribute__ ((unused))
set_ml_params (GstElement * qtimlelement, GstElement * qtimlpostprocess,
    GstElement * detection_filter, GstAppOptions * options, guint htp_id)
{
  GstStructure *delegate_options;
  GstCaps *pad_filter;
  gint module_id;
  const gchar *module = NULL;
  gchar delegate_string[128], settings[128];

  snprintf (delegate_string, 127, "QNNExternalDelegate,backend_type=htp,"
      "htp_device_id=(string)%u,htp_performance_mode=(string)2,"
      "htp_precision=(string)1;", htp_id);

  delegate_options = gst_structure_from_string (
    delegate_string, NULL);

  g_object_set (G_OBJECT (qtimlelement), "model", options->model_path,
      "delegate", GST_ML_TFLITE_DELEGATE_EXTERNAL, NULL);
  g_object_set (G_OBJECT (qtimlelement),
      "external-delegate-path", "libQnnTFLiteDelegate.so", NULL);
  g_object_set (G_OBJECT (qtimlelement),
      "external-delegate-options", delegate_options, NULL);
  gst_structure_free (delegate_options);

  // Set properties for ML postproc plugins- labels, module, threshold
  g_object_set (G_OBJECT (qtimlpostprocess),
      "labels", options->labels_path, NULL);

  if (options->use_case == GST_OBJECT_DETECTION) {
    module = "yolov8";
  } else if (options->use_case == GST_CLASSIFICATION) {
    module = "mobilenet-softmax";
  }
  module_id = get_enum_value (qtimlpostprocess, "module", module);
  if (module_id != -1) {
    snprintf (settings, 127, "{\"confidence\": %.1f}", DEFAULT_THRESHOLD_VALUE);
    g_object_set (G_OBJECT (qtimlpostprocess), "module", module_id, NULL);
  } else {
    g_printerr ("Module %s is not available in qtimlpostprocess\n", module);
    return FALSE;
  }

  g_object_set (G_OBJECT (qtimlpostprocess), "results", 2,
      "settings", settings, NULL);

  // Set the properties of pad_filter for negotiation with downstream
  pad_filter = gst_caps_new_simple ("video/x-raw",
      "format", G_TYPE_STRING, "BGRA",
      "width", G_TYPE_INT, DEFAULT_FILTER_WIDTH,
      "height", G_TYPE_INT, DEFAULT_FILTER_HEIGHT, NULL);
  g_object_set (G_OBJECT (detection_filter), "caps", pad_filter, NULL);
  gst_caps_unref (pad_filter);

  return TRUE;
}

/**
 * Callback function used for demuxer dynamic pad.
 *
 * @param element Plugin supporting dynamic pad.
 * @param pad The source pad that is added.
 * @param data Userdata set at callback registration.
 */
static void
on_pad_added (GstElement * element, GstPad * pad, gpointer data)
{
  GstPad *sinkpad = NULL;
  gchar *caps_str = NULL;
  GstElement *queue = (GstElement *) data;
  GstCaps *caps = gst_pad_get_current_caps (pad);
  if (!caps) {
    caps = gst_pad_query_caps (pad, NULL);
  }

  if (caps) {
    caps_str = gst_caps_to_string (caps);
  } else {
    g_print ("No caps available for this pad\n");
  }

  // Check if caps contains video
  if (caps_str) {
    if (g_strrstr (caps_str, "video")) {
      // Get the static sink pad from the queue
      sinkpad = gst_element_get_static_pad (queue, "sink");
      g_assert (gst_pad_link (pad, sinkpad) == GST_PAD_LINK_OK);
      gst_object_unref (sinkpad);
    } else {
      g_print ("Ignoring caps\n");
    }
  }
  g_free (caps_str);
  gst_caps_unref (caps);
}

/**
 * Build a GstValueArray property (used for composer pad position/dimensions).
 *
 * @param property Property GValue to fill.
 * @param values Array of integer values.
 * @param num Number of values.
 */
static void __attribute__ ((unused))
build_pad_property (GValue * property, gint values[], gint num)
{
  GValue val = G_VALUE_INIT;
  g_value_init (&val, G_TYPE_INT);

  for (gint idx = 0; idx < num; idx++) {
    g_value_set_int (&val, values[idx]);
    gst_value_array_append_value (property, &val);
  }

  g_value_unset (&val);
}

/**
 * Configure a qtivcomposer sink pad to full-frame 1080p placement.
 *
 * @param composer The qtivcomposer element.
 * @param pad_name Name of the sink pad (e.g. "sink_0", "sink_1").
 * @return TRUE on success, FALSE otherwise.
 */
static gboolean __attribute__ ((unused))
set_composer_pad (GstElement * composer, const gchar * pad_name)
{
  GstPad *pad = NULL;
  GValue position = G_VALUE_INIT;
  GValue dimension = G_VALUE_INIT;
  gint pos_vals[2] = {0, 0};
  gint dim_vals[2] = {DEFAULT_OUTPUT_WIDTH, DEFAULT_OUTPUT_HEIGHT};

  pad = gst_element_get_static_pad (composer, pad_name);
  if (!pad) {
    g_printerr ("Couldn't get %s of qtivcomposer\n", pad_name);
    return FALSE;
  }

  g_value_init (&position, GST_TYPE_ARRAY);
  g_value_init (&dimension, GST_TYPE_ARRAY);
  build_pad_property (&position, pos_vals, 2);
  build_pad_property (&dimension, dim_vals, 2);
  g_object_set_property (G_OBJECT (pad), "position", &position);
  g_object_set_property (G_OBJECT (pad), "dimensions", &dimension);
  g_value_unset (&position);
  g_value_unset (&dimension);
  gst_object_unref (pad);

  return TRUE;
}

/**
 * Free Application context:
 *
 * @param appctx Application Context object
 */
static void
gst_app_context_free
(GstAppContext * appctx, GstAppOptions * options, gchar * config_file)
{
  // If specific pointer is not NULL, unref it
  if (appctx->mloop != NULL) {
    g_main_loop_unref (appctx->mloop);
    appctx->mloop = NULL;
  }

  if (options->model_path != NULL &&
    options->model_path != (gchar *)(&DEFAULT_TFLITE_YOLOX_MODEL) &&
    options->model_path != (gchar *)(&DEFAULT_TFLITE_INCEPTIONV3_MODEL)) {
    g_free ((gpointer)options->model_path);
    options->model_path = NULL;
  }

  if (options->labels_path != NULL &&
    options->labels_path != (gchar *)(&DEFAULT_YOLOX_LABELS) &&
    options->labels_path != (gchar *)(&DEFAULT_CLASSIFICATION_LABELS)) {
    g_free ((gpointer)options->labels_path);
    options->labels_path = NULL;
  }

  if (config_file != NULL &&
      config_file != (gchar *)(&DEFAULT_CONFIG_FILE)) {
    g_free ((gpointer)config_file);
    config_file = NULL;
  }

  for (gint i = 0; i < options->num_file; i++) {
    g_free ((gpointer)options->input_file_path[i]);
    options->input_file_path[i] = NULL;
  }

  if (options->output_ip_address != NULL &&
      options->output_ip_address != (gchar *)(&DEFAULT_IP)) {
    g_free ((gpointer)options->output_ip_address);
    options->output_ip_address = NULL;
  }

  if (appctx->pipeline != NULL) {
    gst_object_unref (appctx->pipeline);
    appctx->pipeline = NULL;
  }
}

/**
 * Create GST pipeline: has 3 main steps
 * 1. Create all elements/GST Plugins
 * 2. Set Paramters for each plugin
 * 3. Link plugins to create GST pipeline
 *
 * Each file input becomes a single linear chain:
 * filesrc -> qtdemux -> queue[0] -> parse -> v4l2dec -> caps -> queue[1]
 *   -> qtimlvconverter -> queue[2] -> qtimlelement -> queue[3]
 *   -> qtimlpostprocess -> detection_filter -> qtivtransform -> transform_caps
 *   -> v4l2h264enc -> h264parse -> qtirtspbin
 *
 * @param appctx Application Context Pointer.
 * @param options Application specific options.
 */
static gboolean
create_pipe (GstAppContext * appctx, GstAppOptions * options, guint htp_count)
{
  // Elements for file source
  GstElement *filesrc[MAX_FILESRCS] = {NULL};
  GstElement *qtdemux[MAX_FILESRCS] = {NULL};
  GstElement *file_queue[MAX_FILESRCS][QUEUE_COUNT] = {NULL};
  GstElement *file_pacer[MAX_FILESRCS] = {NULL};
  GstElement *file_dec_parse[MAX_FILESRCS] = {NULL};
  GstElement *file_v4l2_decoder[MAX_FILESRCS] = {NULL};
  GstElement *file_decode_caps[MAX_FILESRCS] = {NULL};
  GstElement *file_tee[MAX_FILESRCS] = {NULL};

  // Elements for ML inference
  GstElement *file_qtimlvconverter[MAX_FILESRCS] = {NULL};
  GstElement *file_qtimlelement[MAX_FILESRCS] = {NULL};
  GstElement *file_qtimlpostprocess[MAX_FILESRCS] = {NULL};
  GstElement *file_detection_filter[MAX_FILESRCS] = {NULL};

  // Elements for composition, transform, encode and RTSP output
  GstElement *file_qtivcomposer[MAX_FILESRCS] = {NULL};
  GstElement *file_transform[MAX_FILESRCS] = {NULL};
  GstElement *file_transform_caps[MAX_FILESRCS] = {NULL};
  GstElement *stream_encoder[MAX_FILESRCS] = {NULL};
  GstElement *stream_encoder_caps[MAX_FILESRCS] = {NULL};
  GstElement *stream_encoder_parse[MAX_FILESRCS] = {NULL};
  GstElement *stream_rtsp[MAX_FILESRCS] = {NULL};

  GstCaps *filtercaps = NULL;
  GstStructure *fcontrols = NULL;
  gchar element_name[128];
  gchar port_str[16];
  gboolean ret = FALSE;

  g_print ("IN Options: file: %d, input-type: %d\n",
      options->num_file, options->input_type);
  g_print ("OUT Options: rtsp address: %s, base port: %d\n",
      options->output_ip_address, options->output_port_base);

  // 1. Create the elements or Plugins
  for (gint i = 0; i < options->num_file; i++) {
    // Create filesrc plugin for file input
    snprintf (element_name, 127, "filesrc-%d", i);
    filesrc[i] = gst_element_factory_make ("filesrc", element_name);
    if (!filesrc[i]) {
      g_printerr ("Failed to create filesrc-%d\n", i);
      goto error_clean_elements;
    }

    // Create demuxer for video container parsing
    snprintf (element_name, 127, "qtdemux-%d", i);
    qtdemux[i] = gst_element_factory_make ("qtdemux", element_name);
    if (!qtdemux[i]) {
      g_printerr ("Failed to create qtdemux-%d\n", i);
      goto error_clean_elements;
    }

    for (gint j = 0; j < QUEUE_COUNT; j++) {
      snprintf (element_name, 127, "file_queue-%d-%d", i, j);
      file_queue[i][j] = gst_element_factory_make ("queue", element_name);
      if (!file_queue[i][j]) {
        g_printerr ("Failed to create file_queue-%d-%d\n", i, j);
        goto error_clean_elements;
      }
    }

    // Pace frames to realtime (identity sync): the V4L2 encoder skips IDR
    // generation entirely when fed faster than realtime.
    snprintf (element_name, 127, "file_pacer-%d", i);
    file_pacer[i] = gst_element_factory_make ("identity", element_name);
    if (!file_pacer[i]) {
      g_printerr ("Failed to create file_pacer-%d\n", i);
      goto error_clean_elements;
    }
    g_object_set (G_OBJECT (file_pacer[i]), "sync", TRUE, NULL);

    // Create H.264/H265 frame parser plugin
    snprintf (element_name, 127, "file_dec_parse-%d", i);
    if (options->input_type == GST_INPUT_STREAM_H264) {
      file_dec_parse[i] = gst_element_factory_make ("h264parse", element_name);
      if (!file_dec_parse[i]) {
        g_printerr ("Failed to create H264 file_dec_parse-%d\n", i);
        goto error_clean_elements;
      }
    } else if (options->input_type == GST_INPUT_STREAM_H265) {
      file_dec_parse[i] = gst_element_factory_make ("h265parse", element_name);
      if (!file_dec_parse[i]) {
        g_printerr ("Failed to create H265 file_dec_parse-%d\n", i);
        goto error_clean_elements;
      }
    } else {
      g_printerr ("Failed to create file_dec_parse-%d Invalid input-type \n", i);
      goto error_clean_elements;
    }

    // Create H.264/H265 Decoder Plugin
    snprintf (element_name, 127, "file_v4l2_decoder-%d", i);
    if (options->input_type == GST_INPUT_STREAM_H264) {
      file_v4l2_decoder[i] = gst_element_factory_make ("v4l2h264dec", element_name);
      if (!file_v4l2_decoder[i]) {
        g_printerr ("Failed to create file_v4l2_decoder-%d\n", i);
        goto error_clean_elements;
      }
    } else if (options->input_type == GST_INPUT_STREAM_H265) {
      file_v4l2_decoder[i] = gst_element_factory_make ("v4l2h265dec", element_name);
      if (!file_v4l2_decoder[i]) {
        g_printerr ("Failed to create file_v4l2_decoder-%d\n", i);
        goto error_clean_elements;
      }
    } else {
      g_printerr ("Failed to create file_v4l2_decoder-%d Invalid input-type \n", i);
      goto error_clean_elements;
    }

    // Create caps for H.264/H265 Decoder
    snprintf (element_name, 127, "file_decode_caps-%d", i);
    file_decode_caps[i] = gst_element_factory_make ("capsfilter", element_name);
    if (!file_decode_caps[i]) {
      g_printerr ("Failed to create file_decode_caps-%d\n", i);
      goto error_clean_elements;
    }

    // Create tee to split the decoded frame into preview and inference paths
    snprintf (element_name, 127, "file_tee-%d", i);
    file_tee[i] = gst_element_factory_make ("tee", element_name);
    if (!file_tee[i]) {
      g_printerr ("Failed to create file_tee-%d\n", i);
      goto error_clean_elements;
    }

    // Create pre processing plugin
    snprintf (element_name, 127, "file_qtimlvconverter-%d", i);
    file_qtimlvconverter[i] = gst_element_factory_make (
        "qtimlvconverter", element_name);
    if (!file_qtimlvconverter[i]) {
      g_printerr ("Failed to create file_qtimlvconverter-%d\n", i);
      goto error_clean_elements;
    }

    // Create ML Framework Plugin
    snprintf (element_name, 127, "file_qtimlelement-%d", i);
    file_qtimlelement[i] = gst_element_factory_make (
        options->mlframework, element_name);
    if (!file_qtimlelement[i]) {
      g_printerr ("Failed to create file_qtimlelement-%d\n", i);
      goto error_clean_elements;
    }

    // Create post processing plugin
    snprintf (element_name, 127, "file_qtimlpostprocess-%d", i);
    if (options->use_case == GST_OBJECT_DETECTION ||
        options->use_case == GST_CLASSIFICATION) {
      file_qtimlpostprocess[i] = gst_element_factory_make (
          "qtimlpostprocess", element_name);
    } else {
      g_printerr ("Invalid use case for file_qtimlpostprocess-%d\n", i);
    }
    if (!file_qtimlpostprocess[i]) {
      g_printerr ("Failed to create file_qtimlpostprocess-%d\n", i);
      goto error_clean_elements;
    }

    // Capsfilter to fix ML post proc output (BGRA 640x360)
    snprintf (element_name, 127, "file_detection_filter-%d", i);
    file_detection_filter[i] = gst_element_factory_make (
        "capsfilter", element_name);
    if (!file_detection_filter[i]) {
      g_printerr ("Failed to create file_detection_filter-%d\n", i);
      goto error_clean_elements;
    }

    // Create composer to overlay detection boxes onto the original frame
    snprintf (element_name, 127, "file_qtivcomposer-%d", i);
    file_qtivcomposer[i] = gst_element_factory_make (
        "qtivcomposer", element_name);
    if (!file_qtivcomposer[i]) {
      g_printerr ("Failed to create file_qtivcomposer-%d\n", i);
      goto error_clean_elements;
    }

    // Hardware transform: scale/convert boxed BGRA frame to NV12 1080p
    snprintf (element_name, 127, "file_transform-%d", i);
    file_transform[i] = gst_element_factory_make ("videoconvert", element_name);
    if (!file_transform[i]) {
      g_printerr ("Failed to create file_transform-%d\n", i);
      goto error_clean_elements;
    }

    // Capsfilter to force the transform output to NV12 1920x1080
    snprintf (element_name, 127, "file_transform_caps-%d", i);
    file_transform_caps[i] = gst_element_factory_make ("capsfilter", element_name);
    if (!file_transform_caps[i]) {
      g_printerr ("Failed to create file_transform_caps-%d\n", i);
      goto error_clean_elements;
    }

    // Create per-stream H.264/H265 encoder
    snprintf (element_name, 127, "stream_encoder-%d", i);
    if (options->input_type == GST_INPUT_STREAM_H264) {
      stream_encoder[i] = gst_element_factory_make ("v4l2h264enc", element_name);
    } else {
      stream_encoder[i] = gst_element_factory_make ("v4l2h265enc", element_name);
    }
    if (!stream_encoder[i]) {
      g_printerr ("Failed to create stream_encoder-%d\n", i);
      goto error_clean_elements;
    }

    // Create per-stream encoder frame parser (byte-stream for RTSP)
    snprintf (element_name, 127, "stream_encoder_caps-%d", i);
    stream_encoder_caps[i] = gst_element_factory_make ("capsfilter",
        element_name);
    if (!stream_encoder_caps[i]) {
      g_printerr ("Failed to create stream_encoder_caps-%d\n", i);
      goto error_clean_elements;
    }

    snprintf (element_name, 127, "stream_encoder_parse-%d", i);
    if (options->input_type == GST_INPUT_STREAM_H264) {
      stream_encoder_parse[i] = gst_element_factory_make ("h264parse",
          element_name);
    } else {
      stream_encoder_parse[i] = gst_element_factory_make ("h265parse",
          element_name);
    }
    if (!stream_encoder_parse[i]) {
      g_printerr ("Failed to create stream_encoder_parse-%d\n", i);
      goto error_clean_elements;
    }

    // Create per-stream RTSP sink (qtirtspbin wraps gst-rtsp-server)
    snprintf (element_name, 127, "stream_rtsp-%d", i);
    stream_rtsp[i] = gst_element_factory_make ("qtirtspbin", element_name);
    if (!stream_rtsp[i]) {
      g_printerr ("Failed to create stream_rtsp-%d\n", i);
      goto error_clean_elements;
    }
  }

  // 2. Set properties for all GST plugin elements
  for (gint i = 0; i < options->num_file; i++) {
    // Set source plugin properties
    g_object_set (G_OBJECT (filesrc[i]), "location",
        options->input_file_path[i], NULL);

    // Decoder in dmabuf mode to keep buffers on hardware
    gst_element_set_enum_property (file_v4l2_decoder[i], "capture-io-mode",
        "dmabuf");
    gst_element_set_enum_property (file_v4l2_decoder[i], "output-io-mode",
        "dmabuf");
    filtercaps = gst_caps_new_simple ("video/x-raw",
        "format", G_TYPE_STRING, "NV12_Q08C",
        "framerate", GST_TYPE_FRACTION, DEFAULT_FRAMERATE, 1, NULL);
    g_object_set (G_OBJECT (file_decode_caps[i]), "caps", filtercaps, NULL);
    gst_caps_unref (filtercaps);


    // Force qtivtransform output to NV12 1920x1080
    filtercaps = gst_caps_new_simple ("video/x-raw",
        "format", G_TYPE_STRING, "NV12",
        "width", G_TYPE_INT, DEFAULT_OUTPUT_WIDTH,
        "height", G_TYPE_INT, DEFAULT_OUTPUT_HEIGHT,
        "framerate", GST_TYPE_FRACTION, DEFAULT_FRAMERATE, 1, NULL);
    g_object_set (G_OBJECT (file_transform_caps[i]), "caps", filtercaps, NULL);
    gst_caps_unref (filtercaps);

    // Set per-stream encoder properties
    // Encoder io-modes left at default (auto): dmabuf-import combined
    // with qtivtransform GLES output breaks the firmware GOP (no IDRs).
    fcontrols = gst_structure_from_string (
        "fcontrols,video_bitrate=6000000,video_bitrate_mode=0,sequence_header_mode=0,video_gop_size=30", NULL);
    g_object_set (G_OBJECT (stream_encoder[i]), "extra-controls", fcontrols,
        NULL);
    /* Deliberately not freed: the v4l2 element may not copy the structure. */

    // Encoder emits byte-stream by default. Force the SPS level to 4.0
    // (1080p) and framerate so that h264parse reports valid values instead
    // of level=1 / framerate=0/1, which strict decoders (v4l2h264dec) refuse.
    filtercaps = gst_caps_new_simple ("video/x-h264",
        "level", G_TYPE_STRING, "4",
        "framerate", GST_TYPE_FRACTION, DEFAULT_FRAMERATE, 1, NULL);
    g_object_set (G_OBJECT (stream_encoder_caps[i]), "caps", filtercaps, NULL);
    gst_caps_unref (filtercaps);

    // Encoder parser must emit byte-stream with IDR config interval.
    // Note: h264parse has no writable "stream-format" property; it derives
    // the output format via caps negotiation (v4l2 encoders emit byte-stream
    // by default), so only config-interval is set here.
    g_object_set (G_OBJECT (stream_encoder_parse[i]), "config-interval", 1,
        NULL);

    // Set per-stream RTSP sink properties: each stream listens on its own
    // port (base_port + i) so 12 channels map to 12 distinct RTSP URLs.
    snprintf (port_str, sizeof (port_str), "%d",
        options->output_port_base + i);
    g_object_set (G_OBJECT (stream_rtsp[i]), "address",
        options->output_ip_address, "port", port_str, NULL);
  }

  // Periodic keyframe forcing disabled: the timer events made the V4L2
  // encoder stop emitting IDRs entirely. GOP-based IDRs (video_gop_size=30)
  // are sufficient for RTSP clients to join mid-stream.

  // 3. Setup the pipeline
  g_print ("Add all elements to the pipeline...\n");

  for (gint i = 0; i < options->num_file; i++) {
    gst_bin_add_many (GST_BIN (appctx->pipeline), filesrc[i], qtdemux[i],
        file_dec_parse[i], file_v4l2_decoder[i], file_decode_caps[i],
        file_tee[i], file_qtivcomposer[i], file_transform[i],
        file_transform_caps[i], stream_encoder[i], stream_encoder_caps[i],
        stream_encoder_parse[i], stream_rtsp[i], NULL);
    for (gint j = 0; j < QUEUE_COUNT; j++) {
      gst_bin_add_many (GST_BIN (appctx->pipeline), file_queue[i][j], NULL);
    }
    gst_bin_add (GST_BIN (appctx->pipeline), file_pacer[i]);
  }

  g_print ("Link elements...\n");

  // Create a single linear pipeline for each file input
  for (gint i = 0; i < options->num_file; i++) {
    ret = gst_element_link_many (filesrc[i], qtdemux[i], NULL);
    if (!ret) {
      g_printerr ("Pipeline elements cannot be linked for %d"
          " filesrc -> qtdemux.\n", i);
      goto error_clean_pipeline;
    }
    // qtdemux -> file_queue[i][0] link is not created here as it is a
    // dymanic link using on_pad_added callback
    // queue[0] -> pacer -> parse -> decoder -> decode_caps -> queue[1] -> tee
    ret = gst_element_link_many (file_queue[i][0], file_pacer[i],
        file_dec_parse[i], file_v4l2_decoder[i], file_decode_caps[i],
        file_queue[i][1], file_tee[i], NULL);
    if (!ret) {
      g_printerr ("Pipeline elements cannot be linked for %d"
          " file: decode chain -> tee.\n", i);
      goto error_clean_pipeline;
    }
    // tee -> queue[2] -> composer (original frame preview path)
    ret = gst_element_link_many (file_tee[i], file_queue[i][2],
        file_qtivcomposer[i], NULL);
    if (!ret) {
      g_printerr ("Pipeline elements cannot be linked for %d"
          " file: tee -> composer preview path.\n", i);
      goto error_clean_pipeline;
    }
    // tee -> queue[3] -> composer (ML detection path bypassed: the BGRA
    // overlay breaks the composer output memory for downstream encoding)
    ret = gst_element_link_many (file_tee[i], file_queue[i][3],
        file_qtivcomposer[i], NULL);
    if (!ret) {
      g_printerr ("Pipeline elements cannot be linked for %d"
          " file: tee -> inference -> composer.\n", i);
      goto error_clean_pipeline;
    }
    // composer -> transform -> transform_caps -> encoder -> encoder_caps
    //   -> encoder_parse -> rtsp
    ret = gst_element_link_many (file_qtivcomposer[i], file_transform[i],
        file_transform_caps[i], stream_encoder[i], stream_encoder_caps[i],
        stream_encoder_parse[i], stream_rtsp[i], NULL);
    if (!ret) {
      g_printerr ("Pipeline elements cannot be linked for %d"
          " file: composer -> rtsp sink.\n", i);
      goto error_clean_pipeline;
    }
  }

  // Composer sink pad properties (position/dimensions) disabled: they
  // alter the composer output path and break encoder IDR generation.

  for (gint i = 0; i < options->num_file; i++) {
    g_signal_connect (qtdemux[i], "pad-added", G_CALLBACK (on_pad_added),
        file_queue[i][0]);
  }

  return TRUE;

error_clean_pipeline:
  gst_object_unref (appctx->pipeline);
  return FALSE;

error_clean_elements:
  for (gint i = 0; i < options->num_file; i++) {
    cleanup_gst (&filesrc[i], &qtdemux[i], &file_dec_parse[i],
        &file_v4l2_decoder[i], &file_decode_caps[i], &file_tee[i],
        &file_qtimlvconverter[i], &file_qtimlelement[i],
        &file_qtimlpostprocess[i], &file_detection_filter[i],
        &file_qtivcomposer[i], &file_transform[i], &file_transform_caps[i],
        &stream_encoder[i], &stream_encoder_caps[i], &stream_encoder_parse[i],
        &stream_rtsp[i], NULL);
    for (gint j = 0; j < QUEUE_COUNT; j++) {
      cleanup_gst (&file_queue[i][j], NULL);
    }
  }

  return FALSE;
}

/**
 * Handles interrupt (SIGINT) by quitting the main loop directly.
 *
 * @param userdata Pointer to AppContext.
 * @return FALSE so the signal source is removed.
 */
static gboolean
loop_interrupt_cb (gpointer userdata)
{
  GstAppContext *appctx = (GstAppContext *) userdata;
  g_print ("\n\nReceived interrupt signal, stopping ...\n");
  g_main_loop_quit (appctx->mloop);
  return FALSE;
}

/**
 * Parse JSON file to read input parameters
 *
 * @param config_file Path to config file
 * @param options Application specific options
 */
gint
parse_json (gchar * config_file, GstAppOptions * options)
{
  JsonParser *parser = NULL;
  JsonNode *root = NULL;
  JsonObject *root_obj = NULL;
  GError *error = NULL;
  JsonArray *files_info = NULL;

  parser = json_parser_new ();

  // Load the JSON file
  if (!json_parser_load_from_file (parser, config_file, &error)) {
    g_printerr ("Unable to parse JSON file: %s\n", error->message);
    g_error_free (error);
    g_object_unref (parser);
    return -1;
  }

  // Get the root object
  root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root)) {
    gst_printerr ("Failed to load json object\n");
    g_object_unref (parser);
    return -1;
  }
  root_obj = json_node_get_object (root);

  if (json_object_has_member (root_obj, "input-file-path")) {
    files_info = json_object_get_array_member (root_obj, "input-file-path");
    options->num_file = json_array_get_length (files_info);
    if (options->num_file > MAX_FILESRCS) {
      gst_printerr ("Number of input files has to be <= %d\n", MAX_FILESRCS);
      g_object_unref (parser);
      return -1;
    }
    for (gint i = 0; i < options->num_file; i++) {
      options->input_file_path[i] =
          g_strdup (json_array_get_string_element (files_info, i));
    }
  }

  if (json_object_has_member (root_obj, "input-type")) {
    const gchar* input_type =
        json_object_get_string_member (root_obj, "input-type");
    if (g_strcmp0 (input_type, "h264") == 0)
      options->input_type = GST_INPUT_STREAM_H264;
    else if (g_strcmp0 (input_type, "h265") == 0)
      options->input_type = GST_INPUT_STREAM_H265;
    else {
      gst_printerr ("Input stream type can only be one of "
          "\"h264\" or \"h265\" \n");
      g_object_unref (parser);
      return -1;
    }
  }

  if (json_object_has_member (root_obj, "model")) {
    options->model_path =
        g_strdup (json_object_get_string_member (root_obj, "model"));
  }

  if (json_object_has_member (root_obj, "labels")) {
    options->labels_path =
        g_strdup (json_object_get_string_member (root_obj, "labels"));
  }

  if (json_object_has_member (root_obj, "output-ip-address")) {
    options->output_ip_address =
        g_strdup (json_object_get_string_member (root_obj,
            "output-ip-address"));
  }

  if (json_object_has_member (root_obj, "output-port")) {
    options->output_port_base =
        json_object_get_int_member (root_obj, "output-port");
  }

  if (json_object_has_member (root_obj, "use-case")) {
    options->use_case =
        json_object_get_int_member (root_obj, "use-case");
  }

  g_object_unref (parser);
  return 0;
}

gint
main (gint argc, gchar * argv[])
{
  GMainLoop *mloop = NULL;
  GstBus *bus = NULL;
  GstElement *pipeline = NULL;
  GOptionContext *ctx = NULL;
  const gchar *app_name = NULL;
  gchar *config_file = NULL;
  GstAppContext appctx = {};
  GstAppOptions options = {};
  struct rlimit rl;
  guint intrpt_watch_id = 0;
  gboolean ret = FALSE;
  gchar help_description[4096];
  gint htp_count = 1;

  // Define the new limit
  rl.rlim_cur = 4096; // Soft limit
  rl.rlim_max = 4096; // Hard limit

  // Set the new limit
  if (setrlimit (RLIMIT_NOFILE, &rl) != 0) {
    g_printerr ("Failed to set setrlimit\n");
  }

  // Get the current limit
  if (getrlimit (RLIMIT_NOFILE, &rl) != 0) {
    g_printerr ("Failed to get getrlimit\n");
  }

  options.use_case = GST_OBJECT_DETECTION;
  options.mlframework = "qtimltflite";
  options.input_type = GST_INPUT_STREAM_H264;
  options.output_ip_address = DEFAULT_IP;
  options.output_port_base = DEFAULT_PORT;

  htp_count = get_num_cdsp_backends();
  g_print ("HTP Core Count = %d\n", htp_count);

  // Structure to define the user options selection
  GOptionEntry entries[] = {
    { "config-file", 0, 0, G_OPTION_ARG_STRING,
      &config_file,
      "Path to config file\n",
      NULL
    },
    { NULL }
  };

  app_name = strrchr (argv[0], '/') ? (strrchr (argv[0], '/') + 1) : argv[0];

  snprintf (help_description, 4095, "\nExample:\n"
      "  %s --config-file=%s\n"
      "\nThis Sample App runs object detection on up to %d file streams and "
      "pushes each stream to its own RTSP server for live observation\n"
      "\nConfig file fields:\n"
      "  input-file-path: <json array>\n"
      "      json array of input files. Eg:\n"
      "      [\"/etc/media/video1.mp4\", \"/etc/media/video2.mp4\"]\n"
      "      max number of input files: %d\n"
      "  model: path to model file\n"
      "      This is an optional parameter and overrides default path\n"
      "      Default detection model path: " DEFAULT_TFLITE_YOLOX_MODEL "\n"
      "      Default classification model path: "
      DEFAULT_TFLITE_INCEPTIONV3_MODEL "\n"
      "  labels: path to labels file\n"
      "      This is an optional parameter and overrides default path\n"
      "      Default detection labels path: " DEFAULT_YOLOX_LABELS "\n"
      "      Default classification model path: "
      DEFAULT_CLASSIFICATION_LABELS "\n"
      "  output-ip-address: \"<ip>\"\n"
      "      RTSP server listen address.\n"
      "      default: " DEFAULT_IP "\n"
      "  output-port: <base port>\n"
      "      RTSP server base listen port; stream i listens on port+i.\n"
      "      default: %d\n"
      "      Each stream is reachable at rtsp://<ip>:<port>/live\n"
      "  use-case: 0 or 1\n"
      "      0: detection, 1: classification\n"
      "  input-type: \"h264\" or \"h265\"\n"
      "      Encoding type for file input\n"
      "      Default encoding: H264\n",
      app_name, DEFAULT_CONFIG_FILE, MAX_FILESRCS, MAX_FILESRCS, DEFAULT_PORT);
  help_description[4095] = '\0';

  // Parse command line entries.
  if ((ctx = g_option_context_new (help_description)) != NULL) {
    gboolean success = FALSE;
    GError *error = NULL;

    g_option_context_add_main_entries (ctx, entries, NULL);
    g_option_context_add_group (ctx, gst_init_get_option_group ());

    success = g_option_context_parse (ctx, &argc, &argv, &error);
    g_option_context_free (ctx);

    if (!success && (error != NULL)) {
      g_printerr ("Failed to parse command line options: %s!\n",
          GST_STR_NULL (error->message));
      g_clear_error (&error);
      gst_app_context_free (&appctx, &options, config_file);
      return -EFAULT;
    } else if (!success && (NULL == error)) {
      g_printerr ("Initializing: Unknown error!\n");
      gst_app_context_free (&appctx, &options, config_file);
      return -EFAULT;
    }
  } else {
    g_printerr ("Failed to create options context!\n");
    return -EFAULT;
  }

  if (config_file == NULL) {
    config_file = DEFAULT_CONFIG_FILE;
  }

  if (!file_exists (config_file)) {
    g_printerr ("Invalid config file path: %s\n", config_file);
    gst_app_context_free (&appctx, &options, config_file);
    return -EINVAL;
  }

  if (parse_json (config_file, &options) != 0) {
    gst_app_context_free (&appctx, &options, config_file);
    return -EINVAL;
  }

  if ((options.use_case != GST_OBJECT_DETECTION) &&
      (options.use_case != GST_CLASSIFICATION)) {
    g_printerr ("Invalid usecase selected, Select Detection or Classification\n");
    gst_app_context_free (&appctx, &options, config_file);
    return -EINVAL;
  }

  if (options.use_case == GST_OBJECT_DETECTION) {
    if (options.model_path ==  NULL)
      options.model_path = DEFAULT_TFLITE_YOLOX_MODEL;
    if (options.labels_path ==  NULL)
      options.labels_path = DEFAULT_YOLOX_LABELS;
  }

  g_print ("model_path=%s labels_path=%s\n",
      options.model_path, options.labels_path);

  if (options.use_case == GST_CLASSIFICATION) {
    if (options.model_path ==  NULL)
      options.model_path = DEFAULT_TFLITE_INCEPTIONV3_MODEL;
    if (options.labels_path ==  NULL)
      options.labels_path = DEFAULT_CLASSIFICATION_LABELS;
  }

  if (options.num_file > MAX_FILESRCS) {
    g_printerr ("Number of file streams cannot be more than %d\n", MAX_FILESRCS);
    gst_app_context_free (&appctx, &options, config_file);
    return -1;
  }

  if (options.num_file < 1) {
    g_printerr ("At least one input-file-path is required\n");
    gst_app_context_free (&appctx, &options, config_file);
    return -EINVAL;
  }

  for (gint i = 0; i < options.num_file; i++) {
    if (!file_exists (options.input_file_path[i])) {
      g_printerr ("video file does not exist at path: %s\n",
          options.input_file_path[i]);
      gst_app_context_free (&appctx, &options, config_file);
      return -EINVAL;
    }
  }

  if (!file_exists (options.model_path)) {
    g_printerr ("Invalid model file path: %s\n", options.model_path);
    gst_app_context_free (&appctx, &options, config_file);
    return -EINVAL;
  }

  if (!file_exists (options.labels_path)) {
    g_printerr ("Invalid labels file path: %s\n", options.labels_path);
    gst_app_context_free (&appctx, &options, config_file);
    return -EINVAL;
  }

  if (options.output_port_base < 1 || options.output_port_base > 65535) {
    g_printerr ("Invalid output-port: %d\n", options.output_port_base);
    gst_app_context_free (&appctx, &options, config_file);
    return -EINVAL;
  }

  if (options.output_port_base + options.num_file - 1 > 65535) {
    g_printerr ("output-port (%d) + number of streams (%d) exceeds 65535\n",
        options.output_port_base, options.num_file);
    gst_app_context_free (&appctx, &options, config_file);
    return -EINVAL;
  }

  g_print ("Run app with model: %s and labels: %s and use case: %s\n",
      options.model_path, options.labels_path,
      (options.use_case ? "Classification" : "Detection"));

  // Initialize GST library.
  gst_init (&argc, &argv);

  // Create the pipeline that will form connection with other elements
  pipeline = gst_pipeline_new (app_name);
  if (!pipeline) {
    g_printerr ("ERROR: failed to create pipeline.\n");
    gst_app_context_free (&appctx, &options, config_file);
    return -1;
  }

  appctx.pipeline = pipeline;

  // Build the pipeline, link all elements in the pipeline
  ret = create_pipe (&appctx, &options, htp_count);
  if (!ret) {
    g_printerr ("ERROR: failed to create GST pipe.\n");
    gst_app_context_free (&appctx, &options, config_file);
    return -1;
  }

  // Initialize main loop.
  if ((mloop = g_main_loop_new (NULL, FALSE)) == NULL) {
    g_printerr ("ERROR: Failed to create Main loop!\n");
    gst_app_context_free (&appctx, &options, config_file);
    return -1;
  }
  appctx.mloop = mloop;

  // Retrieve reference to the pipeline's bus.
  // Bus is message queue for getting callback from gstreamer pipeline
  if ((bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline))) == NULL) {
    g_printerr ("ERROR: Failed to retrieve pipeline bus!\n");
    gst_app_context_free (&appctx, &options, config_file);
    return -1;
  }

  // Watch for messages on the pipeline's bus.
  gst_bus_add_signal_watch (bus);

  // Register respective callback function based on message
  g_signal_connect (bus, "message::state-changed",
      G_CALLBACK (state_changed_cb), pipeline);
  g_signal_connect (bus, "message::error", G_CALLBACK (error_cb), mloop);
  g_signal_connect (bus, "message::warning", G_CALLBACK (warning_cb), mloop);
  g_signal_connect (bus, "message::eos", G_CALLBACK (eos_cb), mloop);
  gst_object_unref (bus);

  // Register function for handling interrupt signals with the main loop.
  intrpt_watch_id =
      g_unix_signal_add (SIGINT, loop_interrupt_cb, &appctx);

  // On successful transition to PAUSED state, state_changed_cb is called.
  // state_changed_cb callback is used to send pipeline to play state.
  g_print ("Set pipeline to PAUSED state ...\n");
  switch (gst_element_set_state (pipeline, GST_STATE_PAUSED)) {
    case GST_STATE_CHANGE_FAILURE:
      g_printerr ("ERROR: Failed to transition to PAUSED state!\n");
      goto error;
    case GST_STATE_CHANGE_NO_PREROLL:
      g_print ("Pipeline is live and does not need PREROLL.\n");
      break;
    case GST_STATE_CHANGE_ASYNC:
      g_print ("Pipeline is PREROLLING ...\n");
      break;
    case GST_STATE_CHANGE_SUCCESS:
      g_print ("Pipeline state change was successful\n");
      break;
  }

  // Wait till pipeline encounters an error or EOS
  g_print ("g_main_loop_run\n");
  g_main_loop_run (mloop);
  g_print ("g_main_loop_run ends\n");

error:
  // Remove the interrupt signal handler
  if (intrpt_watch_id)
    g_source_remove (intrpt_watch_id);

  g_print ("Set pipeline to NULL state ...\n");
  gst_element_set_state (pipeline, GST_STATE_NULL);

  g_print ("Destroy pipeline\n");
  gst_app_context_free (&appctx, &options, config_file);

  g_print ("gst_deinit\n");
  gst_deinit ();

  return 0;
}
