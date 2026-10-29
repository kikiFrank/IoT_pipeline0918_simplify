#include "aibox/siglip_worker/EmbeddingJson.hpp"
#include "aibox/siglip_worker/JsonUtil.hpp"
#include <cmath>
#include <stdexcept>

namespace aibox::siglip {
Embedding ParseEmbedding(const char* data, std::size_t size, std::uint64_t expected_pts) {
  if (size && data[size - 1] == '\0') --size;
  if (!size || std::string(data, size).find('\0') != std::string::npos)
    throw std::runtime_error("invalid embedding JSON buffer length");
  const auto parser = ParseJson(std::string(data, size));
  JsonObject* root = RootObject(parser.get());
  if (String(root, "type") != "image-embedding" ||
      String(root, "schema") != "siglip2.image_embedding.v1" ||
      Integer(root, "dim") != 768)
    throw std::runtime_error("embedding schema mismatch");
  const auto pts = Integer(root, "pts_ns");
  if (pts < 0 || static_cast<std::uint64_t>(pts) != expected_pts)
    throw std::runtime_error("embedding PTS mismatch");
  JsonNode* norm_node = json_object_get_member(root, "raw_l2_norm");
  if (!norm_node || !JSON_NODE_HOLDS_VALUE(norm_node)) throw std::runtime_error("missing raw_l2_norm");
  Embedding result;
  result.pts_ns = expected_pts;
  result.raw_l2_norm = Numeric(norm_node);
  if (!std::isfinite(result.raw_l2_norm) || result.raw_l2_norm <= 0)
    throw std::runtime_error("invalid raw_l2_norm");
  JsonArray* values = Array(root, "embedding");
  if (json_array_get_length(values) != 768) throw std::runtime_error("embedding dimension mismatch");
  double sum = 0;
  for (guint i = 0; i < 768; ++i) {
    JsonNode* value = json_array_get_element(values, i);
    if (!value || !JSON_NODE_HOLDS_VALUE(value)) throw std::runtime_error("non-numeric embedding");
    const double x = Numeric(value);
    if (!std::isfinite(x)) throw std::runtime_error("non-finite embedding");
    result.values.push_back(x);
    sum += x * x;
  }
  if (std::abs(std::sqrt(sum) - 1.0) > 0.01)
    throw std::runtime_error("embedding not L2-normalized");
  return result;
}
}  // namespace aibox::siglip
