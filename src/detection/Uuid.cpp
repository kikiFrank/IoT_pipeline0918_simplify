#include "aibox/detection/Uuid.hpp"

#include <uuid/uuid.h>

namespace aibox::detection {

std::string UuidV4() {
  uuid_t uuid;
  uuid_generate_random(uuid);
  char text[37] = {};
  uuid_unparse_lower(uuid, text);
  return std::string(text);
}

}  // namespace aibox::detection
