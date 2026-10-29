#pragma once
#include <json-glib/json-glib.h>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace aibox::siglip {
struct JsonParserDeleter { void operator()(JsonParser* p) const { if (p) g_object_unref(p); } };
using ParsedJson = std::unique_ptr<JsonParser, JsonParserDeleter>;
ParsedJson ParseJson(const std::string& text);
JsonObject* RootObject(JsonParser* parser);
JsonObject* Object(JsonObject* parent, const char* key);
JsonArray* Array(JsonObject* parent, const char* key);
std::string String(JsonObject* object, const char* key, bool optional = false);
std::int64_t Integer(JsonObject* object, const char* key);
bool Boolean(JsonObject* object, const char* key);
double Numeric(JsonNode* node);
std::string JsonString(JsonNode* node);
}  // namespace aibox::siglip
