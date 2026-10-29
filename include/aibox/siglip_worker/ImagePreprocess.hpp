#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace aibox::siglip {
struct PreparedImage {
  std::vector<std::uint8_t> png;
  std::string source_sha256;
  std::string inference_sha256;
  int source_width = 0, source_height = 0;
};
PreparedImage PrepareImage(const std::vector<std::uint8_t>& jpeg);
}  // namespace aibox::siglip
