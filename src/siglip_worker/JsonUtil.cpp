#include "aibox/siglip_worker/JsonUtil.hpp"
#include <cmath>

namespace aibox::siglip {
double Numeric(JsonNode* node) {
  if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
      (json_node_get_value_type(node) != G_TYPE_DOUBLE &&
       json_node_get_value_type(node) != G_TYPE_INT64))
    throw std::runtime_error("JSON value must be a number");
  const double value = json_node_get_double(node);
  if (!std::isfinite(value)) throw std::runtime_error("JSON value must be finite");
  return value;
}
ParsedJson ParseJson(const std::string& text) {
  ParsedJson parser(json_parser_new());
  GError* error = nullptr;
  if (!json_parser_load_from_data(parser.get(), text.data(), text.size(), &error)) {
    const std::string message = error ? error->message : "JSON parse failed";
    if (error) g_error_free(error);
    throw std::runtime_error(message);
  }
  return parser;
}
JsonObject* RootObject(JsonParser* parser) {
  JsonNode* root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) throw std::runtime_error("JSON root must be object");
  return json_node_get_object(root);
}
JsonObject* Object(JsonObject* parent, const char* key) {
  JsonNode* node = json_object_get_member(parent, key);
  if (!node || !JSON_NODE_HOLDS_OBJECT(node))
    throw std::runtime_error(std::string("JSON object missing: ") + key);
  return json_node_get_object(node);
}
JsonArray* Array(JsonObject* parent, const char* key) {
  JsonNode* node = json_object_get_member(parent, key);
  if (!node || !JSON_NODE_HOLDS_ARRAY(node))
    throw std::runtime_error(std::string("JSON array missing: ") + key);
  return json_node_get_array(node);
}
std::string String(JsonObject* object, const char* key, bool optional) {
  JsonNode* node = json_object_get_member(object, key);
  if ((!node || JSON_NODE_HOLDS_NULL(node)) && optional) return {};
  if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
    throw std::runtime_error(std::string("JSON string missing: ") + key);
  const char* value = json_node_get_string(node);
  return value ? value : "";
}
std::int64_t Integer(JsonObject* object, const char* key) {
  JsonNode* node = json_object_get_member(object, key);
  if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
    throw std::runtime_error(std::string("JSON integer missing: ") + key);
  return json_node_get_int(node);
}
bool Boolean(JsonObject* object, const char* key) {
  JsonNode* node = json_object_get_member(object, key);
  if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN)
    throw std::runtime_error(std::string("JSON boolean missing: ") + key);
  return json_node_get_boolean(node);
}
std::string JsonString(JsonNode* node) {
  char* data = json_to_string(node, FALSE);
  if (!data) throw std::runtime_error("JSON serialization failed");
  std::string text(data);
  g_free(data);
  return text;
}
}  // namespace aibox::siglip
