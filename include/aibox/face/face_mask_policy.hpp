/// @file face_mask_policy.hpp
/// @brief Smooth-first mask decision policy for the qtivoverlay sink probe.
///
/// POLICY (2026-09-04 expert revision — fail-open / smooth-first):
///   - VALID_FACES with at least one validated bbox => BOXES (white boxes
///     from the ONE validated list)
///   - EVERYTHING ELSE => CLEAR (original video passes through unchanged):
///       VALID_EMPTY, UNAVAILABLE, STALE (TTL expiry), missing/mismatched
///       state meta, missing detection meta, illegal/NaN/inverted bboxes,
///       no usable slot, worker errors.
///   FULL-frame masking is NOT part of this policy anymore. The continuous
///   full-frame overlay path was suspected of starving downstream WebRTC
///   encoders (group-level stall) — removed per expert direction.
///
/// Latest valid bboxes are reused ONLY within stale-ttl-ms (250ms default);
/// beyond TTL the result is CLEAR — stale boxes never linger.
///
/// The probe must additionally DEDUPLICATE g_object_set("masks"): update the
/// overlay property only when the decision or the box content changes.
///
/// No GStreamer dependency — the probe fills MaskPolicyInput from the
/// buffer's GstFaceResultStateMeta / GstFaceDetectionMeta and calls
/// DecideMask(). Status constants stay in sync with GstFaceResultStateStatus
/// (static_assert in the mirror header).

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace aibox {
namespace face {

constexpr std::uint32_t kMaskStatusUnavailable = 0;
constexpr std::uint32_t kMaskStatusValidFaces = 1;
constexpr std::uint32_t kMaskStatusValidEmpty = 2;

enum class MaskDecision : std::uint32_t { CLEAR, BOXES };

struct MaskPolicyInput {
  bool state_meta_present = false;
  bool abi_version_ok = false;
  bool meta_size_ok = false;
  std::uint32_t result_status = kMaskStatusUnavailable;
  bool det_meta_present = false;
  std::uint32_t n_faces = 0;          ///< raw value read from detection meta
  std::uint32_t n_faces_over_max = 0; ///< != 0 when n_faces exceeds the legal max
  bool any_valid_bbox = false;        ///< >=1 bbox with w>0 && h>0 after clamp
};

/// Fail-open / smooth-first: only a fully valid VALID_FACES result draws
/// boxes; every other state clears the masks and lets the video through.
inline MaskDecision DecideMask(const MaskPolicyInput& in) {
  const bool state_ok =
      in.state_meta_present && in.abi_version_ok && in.meta_size_ok;
  if (!state_ok) return MaskDecision::CLEAR;
  if (in.result_status != kMaskStatusValidFaces) return MaskDecision::CLEAR;
  if (!in.det_meta_present) return MaskDecision::CLEAR;
  if (in.n_faces == 0 || in.n_faces_over_max != 0) return MaskDecision::CLEAR;
  if (!in.any_valid_bbox) return MaskDecision::CLEAR;
  return MaskDecision::BOXES;
}

// ---------------------------------------------------------------------------
// Box validation: the ONE list used both for the policy decision and for mask
// generation. Rejects non-finite values, clamps into the frame, and drops
// boxes whose integerized w/h is not positive.
// ---------------------------------------------------------------------------

struct RawBox {
  float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

struct IntBox {
  int x = 0, y = 0, w = 0, h = 0;
};

inline std::vector<IntBox> ValidateBoxes(const RawBox* boxes, std::uint32_t n,
                                         std::uint32_t frame_w,
                                         std::uint32_t frame_h) {
  std::vector<IntBox> out;
  if (!boxes || frame_w == 0 || frame_h == 0) return out;
  const int max_x = static_cast<int>(frame_w) - 1;
  const int max_y = static_cast<int>(frame_h) - 1;
  for (std::uint32_t i = 0; i < n; ++i) {
    const RawBox& b = boxes[i];
    if (!std::isfinite(b.x1) || !std::isfinite(b.y1) ||
        !std::isfinite(b.x2) || !std::isfinite(b.y2))
      continue;  // non-finite => never drawn, never counted
    int x1 = static_cast<int>(b.x1);
    int y1 = static_cast<int>(b.y1);
    int x2 = static_cast<int>(b.x2);
    int y2 = static_cast<int>(b.y2);
    x1 = x1 < 0 ? 0 : (x1 > max_x ? max_x : x1);
    y1 = y1 < 0 ? 0 : (y1 > max_y ? max_y : y1);
    x2 = x2 < 0 ? 0 : (x2 > max_x ? max_x : x2);
    y2 = y2 < 0 ? 0 : (y2 > max_y ? max_y : y2);
    const int w = x2 - x1;
    const int h = y2 - y1;
    if (w <= 0 || h <= 0) continue;
    out.push_back(IntBox{x1, y1, w, h});
  }
  return out;
}

}  // namespace face
}  // namespace aibox
