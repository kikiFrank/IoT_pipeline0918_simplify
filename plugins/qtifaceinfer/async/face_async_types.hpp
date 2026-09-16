/// @file face_async_types.hpp
/// @brief Pure-logic types for the qtifaceinfer async refactor.
///
/// No GStreamer / OpenCL / QNN / GLib dependency — std types only, so these
/// classes are unit-testable with a fake monotonic clock on any C++17 host.
/// Time unit: microseconds (std::int64_t). PTS unit: nanoseconds (std::uint64_t).

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace face_async {

/// Hard limits (must never grow past these).
constexpr std::size_t kMaxCameras = 16;
constexpr std::size_t kMaxCameraIdLen = 64;

constexpr std::uint32_t kFaceLandmarkCount = 5;

/// Detection result status.
enum class ResultStatus : std::uint32_t {
  UNAVAILABLE = 0,  ///< no result yet / worker error
  VALID_FACES = 1,  ///< valid detection, >=1 face
  VALID_EMPTY = 2,  ///< valid detection, no faces
};

/// One detected face (value semantics; embedding optional and may be empty).
struct FaceBox {
  float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  float score = 0;
  std::array<float, kFaceLandmarkCount * 2> landmarks{};
  std::vector<float> embedding;  ///< may be empty when ArcFace disabled
};

/// Inference task handed to the worker. Carries the sample timestamp —
/// the ONLY basis for TTL/age computation (never the completion time).
struct TaskRecord {
  std::uint32_t slot_idx = 0;
  std::string camera_id;          ///< <= kMaxCameraIdLen
  std::uint64_t det_frame_id = 0; ///< frame_id of the frame being inferred
  std::uint64_t pts_ns = 0;
  std::uint64_t caps_gen = 0;     ///< captured at acquire
  std::uint64_t stream_gen = 0;   ///< captured at acquire
  std::uint64_t seq = 0;          ///< assigned by producer at commit
  std::int64_t sample_mono_us = 0;///< frame receipt time (monotonic us)
  std::uint32_t width = 0;        ///< frame dims for full-frame mask
  std::uint32_t height = 0;
};

/// Immutable result snapshot published by the worker.
struct FaceResultEntry {
  ResultStatus status = ResultStatus::UNAVAILABLE;
  std::vector<FaceBox> faces;
  std::uint64_t det_frame_id = 0;
  std::uint64_t pts_ns = 0;
  std::uint64_t caps_gen = 0;
  std::uint64_t stream_gen = 0;
  std::uint64_t seq = 0;           ///< task's own seq (never fabricated)
  std::int64_t sample_mono_us = 0; ///< TTL basis: sample time, not completion
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

}  // namespace face_async
