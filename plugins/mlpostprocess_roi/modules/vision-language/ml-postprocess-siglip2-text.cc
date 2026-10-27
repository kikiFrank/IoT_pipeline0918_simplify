// SigLIP2 text-only encoder postprocess for the existing IModule ABI.
// Contract: 1 tensor [1,768] -> ImageEmbedding (FP32, L2 normalized).
// No tokenizer, no classification, no similarity, no bounding boxes.
#include "qti-ml-post-process.h"
#include <algorithm>
#include <any>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {
constexpr size_t kEmbeddingDim = 768;

float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1U;
  const uint32_t exponent = (h >> 10) & 31U;
  const uint32_t fraction = h & 1023U;
  float value;
  if (exponent == 0) {
    value = std::ldexp(static_cast<float>(fraction), -24);
  } else if (exponent == 31) {
    value = fraction ? std::numeric_limits<float>::quiet_NaN()
                     : std::numeric_limits<float>::infinity();
  } else {
    value = std::ldexp(static_cast<float>(1024U + fraction),
                       static_cast<int>(exponent) - 25);
  }
  return sign ? -value : value;
}

template <typename T>
T LoadElement(const void* base, size_t index) {
  T val;
  std::memcpy(&val, static_cast<const uint8_t*>(base) + index * sizeof(T),
              sizeof(T));
  return val;
}

bool DecodeScalar(const Tensor& tensor, size_t i, float* result) {
  if (tensor.data == nullptr || result == nullptr) return false;
  const float scale = tensor.qscale;
  const float offset = tensor.qoffset;
  switch (tensor.type) {
    case TensorType::kFloat32:
      *result = LoadElement<float>(tensor.data, i);
      return true;
    case TensorType::kFloat16:
      *result = HalfToFloat(LoadElement<uint16_t>(tensor.data, i));
      return true;
    case TensorType::kUint16:
      if (!std::isfinite(scale) || scale <= 0.0f || !std::isfinite(offset))
        return false;
      *result = (static_cast<float>(LoadElement<uint16_t>(tensor.data, i))
                + offset) * scale;
      return true;
    case TensorType::kInt16:
      if (!std::isfinite(scale) || scale <= 0.0f || !std::isfinite(offset))
        return false;
      *result = (static_cast<float>(LoadElement<int16_t>(tensor.data, i))
                + offset) * scale;
      return true;
    case TensorType::kUint8:
      if (!std::isfinite(scale) || scale <= 0.0f || !std::isfinite(offset))
        return false;
      *result = (static_cast<float>(LoadElement<uint8_t>(tensor.data, i))
                + offset) * scale;
      return true;
    case TensorType::kInt8:
      if (!std::isfinite(scale) || scale <= 0.0f || !std::isfinite(offset))
        return false;
      *result = (static_cast<float>(LoadElement<int8_t>(tensor.data, i))
                + offset) * scale;
      return true;
    default:
      return false;
  }
}
}  // namespace

class Siglip2TextModule final : public IModule {
 public:
  explicit Siglip2TextModule(LogCallback logger) : logger_(logger) {}

  std::string Caps() override {
    // The runtime MUST negotiate an actual type from this set.
    // The model output is one embedding; B=1 is mandatory in phase 1.
    return R"({"type":"text-embedding","tensors":[{"format":["FLOAT32","FLOAT16","UINT16","INT16","UINT8","INT8"],"dimensions":[[1,768]]}]})";
  }

  bool Configure(const std::string& labels_file,
                 const std::string& json_settings) override {
    (void)labels_file;
    // This text-only model needs no label file or threshold.
    if (!json_settings.empty() && json_settings != "{}") {
      LOG(logger_, kError,
          "SigLIP2 text-only postprocess accepts empty settings or {} only");
      return false;
    }
    return true;
  }

  bool Process(const Tensors& tensors, Dictionary& mlparams,
               std::any& output) override {
    (void)mlparams;
    if (tensors.size() != 1) {
      LOG(logger_, kError, "SigLIP2 text encoder needs exactly one tensor");
      return false;
    }
    const Tensor& t = tensors.front();
    if (t.name != "unknown" && t.name != "text_embeds") {
      LOG(logger_, kError, "Unexpected output tensor name: %s", t.name.c_str());
      return false;
    }
    if (t.dimensions.size() != 2 || t.dimensions[0] != 1 ||
        t.dimensions[1] != kEmbeddingDim || t.data == nullptr) {
      LOG(logger_, kError, "Unexpected shape; expected [1,768] and data != NULL");
      return false;
    }
    if (output.type() != typeid(ImageEmbedding)) {
      LOG(logger_, kError, "Output type must be ImageEmbedding");
      return false;
    }
    auto& embedding = std::any_cast<ImageEmbedding&>(output);
    embedding.values.resize(kEmbeddingDim);
    double squared_norm = 0.0;
    for (size_t i = 0; i < kEmbeddingDim; ++i) {
      float value;
      if (!DecodeScalar(t, i, &value) || !std::isfinite(value)) {
        LOG(logger_, kError, "Bad scalar or quantization parameters");
        return false;
      }
      embedding.values[i] = value;
      squared_norm += static_cast<double>(value) * value;
    }
    const double norm = std::sqrt(squared_norm);
    if (!std::isfinite(norm) || norm < 1e-12) {
      LOG(logger_, kError, "Degenerate SigLIP2 text embedding");
      return false;
    }
    embedding.raw_l2_norm = static_cast<float>(norm);
    const float inv_norm = static_cast<float>(1.0 / norm);
    for (float& value : embedding.values) value *= inv_norm;
    return true;
  }

 private:
  LogCallback logger_;
};

extern "C" IModule* NewModule(LogCallback logger) {
  return new Siglip2TextModule(logger);
}
