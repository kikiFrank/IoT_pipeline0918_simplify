#pragma once

#include "aibox/common/Types.hpp"

namespace aibox::pipeline {

struct CodecProfile {
  const char* name;
  const char* depay;
  const char* parse;
  const char* hw_decoder;
  const char* hw_encoder;
  const char* rtp_pay;
  const char* caps_name;
};

inline const CodecProfile* GetCodecProfile(CodecType codec) {
  static constexpr CodecProfile h264{"h264", "rtph264depay", "h264parse",
      "v4l2h264dec", "v4l2h264enc", "rtph264pay", "video/x-h264"};
  static constexpr CodecProfile h265{"h265", "rtph265depay", "h265parse",
      "v4l2h265dec", "v4l2h265enc", "rtph265pay", "video/x-h265"};
  switch (codec) {
    case CodecType::kH264: return &h264;
    case CodecType::kH265: return &h265;
    default: return nullptr;
  }
}

inline const char* CodecName(CodecType codec) {
  const auto* profile = GetCodecProfile(codec);
  return profile ? profile->name : "unknown";
}

}  // namespace aibox::pipeline
