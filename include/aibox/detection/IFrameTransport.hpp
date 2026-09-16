#pragma once

#include <cstdint>
#include <vector>

namespace aibox::detection {

class IFrameTransport {
 public:
  virtual ~IFrameTransport() = default;
  virtual bool PublishFrame(const std::vector<uint8_t>& payload) = 0;
  // Default true so unit FakeTransport needs no change.
  virtual bool IsConnected() const { return true; }
};

}  // namespace aibox::detection
