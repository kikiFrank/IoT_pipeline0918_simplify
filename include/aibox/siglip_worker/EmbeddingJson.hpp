#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace aibox::siglip {
struct Embedding {
  std::vector<double> values;
  double raw_l2_norm = 0;
  std::uint64_t pts_ns = 0;
};
Embedding ParseEmbedding(const char* data, std::size_t size, std::uint64_t expected_pts);
}  // namespace aibox::siglip
