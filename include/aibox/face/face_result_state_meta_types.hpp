/// @file face_result_state_meta_types.hpp
/// @brief Struct layout for GstFaceResultStateMeta used by the pipeline
///        overlay probe (Builder side).
///
/// Contains ONLY the struct definition + constants (no function declarations).
/// The GType is resolved at runtime via g_type_from_name(), avoiding any
/// link-time dependency on the gstqtifaceinfer plugin.
///
/// Source of truth: plugins/qtifaceinfer/gst_plugin/gstfaceresultstatemeta.h
///
/// The static_asserts below are MANDATORY (not optional): they pin field
/// order, packing and enum values against the plugin header, so any ABI
/// drift between the two headers becomes a compile error.

#pragma once

#include <gst/gst.h>
#include <cstddef>

#include "aibox/face/face_mask_policy.hpp"

G_BEGIN_DECLS

#define FACE_RESULT_STATE_META_ABI_VERSION 1u

typedef enum {
  FACE_RESULT_STATE_UNAVAILABLE = 0,
  FACE_RESULT_STATE_VALID_FACES = 1,
  FACE_RESULT_STATE_VALID_EMPTY = 2,
} GstFaceResultStateStatus;

typedef struct _GstFaceResultStateMeta {
  GstMeta meta;
  guint32 abi_version;
  guint32 struct_size;
  guint32 result_status;
  guint32 result_age_ms;
  guint64 detection_frame_id;
  guint64 applied_video_frame_id;
  guint64 caps_gen;
  guint64 stream_gen;
  guint32 frame_width;
  guint32 frame_height;
} GstFaceResultStateMeta;

G_END_DECLS

/* ---- MANDATORY ABI pins (must match gstfaceresultstatemeta.h) ---- */

static_assert (FACE_RESULT_STATE_META_ABI_VERSION == 1u,
    "ABI version drift");

/* field order + packing: each field directly follows the previous one */
static_assert (offsetof (GstFaceResultStateMeta, struct_size) ==
    offsetof (GstFaceResultStateMeta, abi_version) + sizeof (guint32),
    "struct_size layout drift");
static_assert (offsetof (GstFaceResultStateMeta, result_status) ==
    offsetof (GstFaceResultStateMeta, struct_size) + sizeof (guint32),
    "result_status layout drift");
static_assert (offsetof (GstFaceResultStateMeta, result_age_ms) ==
    offsetof (GstFaceResultStateMeta, result_status) + sizeof (guint32),
    "result_age_ms layout drift");
static_assert (offsetof (GstFaceResultStateMeta, detection_frame_id) ==
    offsetof (GstFaceResultStateMeta, result_age_ms) + sizeof (guint32),
    "detection_frame_id layout drift");
static_assert (offsetof (GstFaceResultStateMeta, applied_video_frame_id) ==
    offsetof (GstFaceResultStateMeta, detection_frame_id) + sizeof (guint64),
    "applied_video_frame_id layout drift");
static_assert (offsetof (GstFaceResultStateMeta, caps_gen) ==
    offsetof (GstFaceResultStateMeta, applied_video_frame_id) + sizeof (guint64),
    "caps_gen layout drift");
static_assert (offsetof (GstFaceResultStateMeta, stream_gen) ==
    offsetof (GstFaceResultStateMeta, caps_gen) + sizeof (guint64),
    "stream_gen layout drift");
static_assert (offsetof (GstFaceResultStateMeta, frame_width) ==
    offsetof (GstFaceResultStateMeta, stream_gen) + sizeof (guint64),
    "frame_width layout drift");
static_assert (offsetof (GstFaceResultStateMeta, frame_height) ==
    offsetof (GstFaceResultStateMeta, frame_width) + sizeof (guint32),
    "frame_height layout drift");

/* enum values must match the mask-policy constants (mask_policy.hpp) */
static_assert (FACE_RESULT_STATE_UNAVAILABLE ==
    static_cast<int> (aibox::face::kMaskStatusUnavailable),
    "UNAVAILABLE enum drift");
static_assert (FACE_RESULT_STATE_VALID_FACES ==
    static_cast<int> (aibox::face::kMaskStatusValidFaces),
    "VALID_FACES enum drift");
static_assert (FACE_RESULT_STATE_VALID_EMPTY ==
    static_cast<int> (aibox::face::kMaskStatusValidEmpty),
    "VALID_EMPTY enum drift");
