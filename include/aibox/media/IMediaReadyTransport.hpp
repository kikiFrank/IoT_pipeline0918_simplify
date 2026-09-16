#pragma once

#include <string>

namespace aibox::media {

class IMediaReadyTransport {
 public:
  virtual ~IMediaReadyTransport() = default;
  virtual bool PublishReady(const std::string& json) = 0;
};

}  // namespace aibox::media
