#include "aibox/webrtc/WebrtcPayload.hpp"

#include <cstdlib>
#include <cstring>
#include <sstream>

namespace aibox::webrtc {

// =========================================================================
// IceServer
// =========================================================================

bool IceServer::IsStun() const {
  return url.compare(0, 5, "stun:") == 0 || url.compare(0, 6, "stuns:") == 0;
}

bool IceServer::IsTurn() const {
  return url.compare(0, 5, "turn:") == 0 || url.compare(0, 6, "turns:") == 0;
}

std::string IceServer::ToTurnUrl() const {
  // Build turn://user:pass@host:port format
  if (!IsTurn()) return "";
  // url is like "turn:host:port?transport=udp" or "turn:host:port"
  // strip "turn:" prefix
  std::string hostpart = url.substr(5);
  // Remove query params
  auto qpos = hostpart.find('?');
  if (qpos != std::string::npos) hostpart = hostpart.substr(0, qpos);
  if (!username.empty()) {
    return "turn://" + username + ":" + credential + "@" + hostpart;
  }
  return "turn://" + hostpart;
}

// =========================================================================
// JSON helpers — minimal, no external dependency
// =========================================================================

namespace {

// Find the start of a JSON string value for a given key.
// Returns pointer to the value (first char after opening quote for strings,
// or first digit/char for numbers/booleans), or nullptr.
const char* FindStringValue(const char* json, const char* key, size_t keylen) {
  // Start inside the outer braces. If json begins with '{', skip it.
  const char* p = json;
  if (*p == '{') ++p;

  while (*p && *p != '}') {
    // Skip whitespace and commas
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') ++p;
    if (*p == '}' || !*p) break;

    // Expect a quoted key
    if (*p != '"') { ++p; continue; }
    ++p;
    if (strncmp(p, key, keylen) == 0 && p[keylen] == '"') {
      p += keylen + 1;
      while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
      if (*p == ':') {
        ++p;
        while (*p == ' ' || *p == '\t') ++p;
        return p;
      }
    }

    // Skip this key-value pair
    // Skip the key string
    while (*p && *p != '"') { if (*p == '\\') ++p; if (*p) ++p; }
    if (*p == '"') ++p;
    // Skip ':'
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p == ':') ++p;
    while (*p == ' ' || *p == '\t') ++p;
    // Skip the value
    if (*p == '"') {
      ++p; while (*p && *p != '"') { if (*p == '\\') ++p; if (*p) ++p; } if (*p) ++p;
    } else if (*p == '{' || *p == '[') {
      char open = *p, close = (open == '{') ? '}' : ']';
      int depth = 1; ++p;
      while (*p && depth > 0) {
        if (*p == '"') { ++p; while (*p && *p != '"') { if (*p == '\\') ++p; if (*p) ++p; } if (*p) ++p; }
        else if (*p == open) { ++depth; ++p; }
        else if (*p == close) { --depth; ++p; }
        else ++p;
      }
    } else {
      while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') ++p;
    }
  }
  return nullptr;
}

std::string ExtractString(const char*& p) {
  if (*p != '"') return "";
  ++p;
  std::string out;
  while (*p && *p != '"') {
    if (*p == '\\') {
      ++p;
      if (*p == '"') out += '"';
      else if (*p == '\\') out += '\\';
      else if (*p == '/') out += '/';
      else if (*p == 'n') out += '\n';
      else if (*p == 'r') out += '\r';
      else if (*p == 't') out += '\t';
      else { out += '\\'; if (*p) out += *p; }
    } else {
      out += *p;
    }
    ++p;
  }
  if (*p == '"') ++p;
  return out;
}

int ExtractInt(const char*& p) {
  // Consume from p, which could be negative
  bool neg = false;
  if (*p == '-') { neg = true; ++p; }
  int val = 0;
  while (*p >= '0' && *p <= '9') {
    val = val * 10 + (*p - '0');
    ++p;
  }
  return neg ? -val : val;
}

// Find the start of an array value for a given key. Returns pointer past '[' or nullptr.
const char* FindArrayValue(const char* json, const char* key, size_t keylen) {
  const char* p = FindStringValue(json, key, keylen);
  if (!p) return nullptr;
  if (*p != '[') return nullptr;
  return p + 1;  // past '['
}

}  // namespace

std::string JsonGetString(const std::string& json, const std::string& key) {
  const char* p = FindStringValue(json.c_str(), key.c_str(), key.size());
  if (!p) return "";
  if (*p == '"') return ExtractString(p);
  // For null or other, return empty
  if (*p == 'n') { p += 4; return ""; }
  return "";
}

int JsonGetInt(const std::string& json, const std::string& key, int def) {
  const char* p = FindStringValue(json.c_str(), key.c_str(), key.size());
  if (!p) return def;
  if (*p >= '0' && *p <= '9') return ExtractInt(p);
  return def;
}

std::string JsonGetArray(const std::string& json, const std::string& key) {
  const char* start = FindArrayValue(json.c_str(), key.c_str(), key.size());
  if (!start) return "";
  // Find matching ']'
  const char* p = start;
  int depth = 1;
  while (*p && depth > 0) {
    if (*p == '"') { ++p; while (*p && *p != '"') { if (*p == '\\') ++p; if (*p) ++p; } if (*p) ++p; }
    else if (*p == '[') { ++depth; ++p; }
    else if (*p == ']') { --depth; if (depth == 0) break; ++p; }
    else ++p;
  }
  return std::string(start, static_cast<size_t>(p - start));
}

IceServer ParseIceServer(const std::string& obj) {
  IceServer srv;
  srv.url        = JsonGetString(obj, "url");
  srv.username   = JsonGetString(obj, "username");
  srv.credential = JsonGetString(obj, "credential");
  return srv;
}

SessionNewPayload ParseSessionNew(const std::string& json) {
  SessionNewPayload p;
  p.camera_id  = JsonGetString(json, "camera_id");
  p.session_id = JsonGetString(json, "session_id");
  p.rtsp_url   = JsonGetString(json, "rtsp_url");

  std::string arr = JsonGetArray(json, "ice_servers");
  if (!arr.empty()) {
    // Parse individual objects in the array
    const char* s = arr.c_str();
    while (*s) {
      while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == ',') ++s;
      if (!*s || *s == ']') break;
      if (*s == '{') {
        ++s;
        const char* e = s;
        int depth = 1;
        while (*e && depth > 0) {
          if (*e == '"') { ++e; while (*e && *e != '"') { if (*e == '\\') ++e; if (*e) ++e; } if (*e) ++e; }
          else if (*e == '{') { ++depth; ++e; }
          else if (*e == '}') { --depth; if (depth == 0) break; ++e; }
          else ++e;
        }
        std::string objstr(s, static_cast<size_t>(e - s));
        p.ice_servers.push_back(ParseIceServer(objstr));
        s = (*e == '}') ? e + 1 : e;
      } else {
        ++s;
      }
    }
  }
  return p;
}

SdpPayload ParseSdpPayload(const std::string& json) {
  SdpPayload p;
  p.camera_id  = JsonGetString(json, "camera_id");
  p.session_id = JsonGetString(json, "session_id");
  p.sdp        = JsonGetString(json, "sdp");
  return p;
}

IceCandidatePayload ParseIceCandidate(const std::string& json) {
  IceCandidatePayload p;
  p.camera_id     = JsonGetString(json, "camera_id");
  p.session_id    = JsonGetString(json, "session_id");
  p.sdpMid        = JsonGetString(json, "sdpMid");
  p.sdpMLineIndex = JsonGetInt(json, "sdpMLineIndex", 0);
  p.candidate     = JsonGetString(json, "candidate");
  return p;
}

SessionClosePayload ParseSessionClose(const std::string& json) {
  SessionClosePayload p;
  p.camera_id  = JsonGetString(json, "camera_id");
  p.session_id = JsonGetString(json, "session_id");
  p.reason     = JsonGetString(json, "reason");
  return p;
}

RenegotiatePayload ParseRenegotiate(const std::string& json) {
  RenegotiatePayload p;
  p.camera_id  = JsonGetString(json, "camera_id");
  p.session_id = JsonGetString(json, "session_id");
  // Nested resolution object
  auto pos = json.find("\"resolution\"");
  if (pos != std::string::npos) {
    auto brace = json.find('{', pos);
    if (brace != std::string::npos) {
      int depth = 0;
      size_t e = brace;
      for (; e < json.size(); ++e) {
        if (json[e] == '{') ++depth;
        else if (json[e] == '}') {
          --depth;
          if (depth == 0) { ++e; break; }
        }
      }
      std::string obj = json.substr(brace, e - brace);
      p.width    = JsonGetInt(obj, "width", 0);
      p.height   = JsonGetInt(obj, "height", 0);
      p.max_kbps = JsonGetInt(obj, "max_kbps", 0);
      return p;
    }
  }
  p.width    = JsonGetInt(json, "width", 0);
  p.height   = JsonGetInt(json, "height", 0);
  p.max_kbps = JsonGetInt(json, "max_kbps", 0);
  return p;
}

// =========================================================================
// JSON builders
// =========================================================================

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;
    }
  }
  return out;
}

std::string BuildSdpJson(const std::string& camera_id,
                         const std::string& session_id,
                         const std::string& sdp,
                         const std::string& type) {
  std::ostringstream os;
  os << "{"
     << "\"camera_id\":\"" << JsonEscape(camera_id) << "\","
     << "\"session_id\":\"" << JsonEscape(session_id) << "\","
     << "\"sdpType\":\"" << type << "\","
     << "\"sdp\":\"" << JsonEscape(sdp) << "\""
     << "}";
  return os.str();
}

std::string BuildIceJson(const std::string& camera_id,
                         const std::string& session_id,
                         const std::string& sdp_mid,
                         int sdp_mline_index,
                         const std::string& candidate) {
  std::ostringstream os;
  os << "{"
     << "\"camera_id\":\"" << JsonEscape(camera_id) << "\","
     << "\"session_id\":\"" << JsonEscape(session_id) << "\","
     << "\"sdpMid\":\"" << JsonEscape(sdp_mid) << "\","
     << "\"sdpMLineIndex\":" << sdp_mline_index << ","
     << "\"candidate\":\"" << JsonEscape(candidate) << "\""
     << "}";
  return os.str();
}

std::string BuildStateJson(const std::string& camera_id,
                           const std::string& session_id,
                           const std::string& ice_state,
                           const std::string& peer_state) {
  std::ostringstream os;
  os << "{"
     << "\"camera_id\":\"" << JsonEscape(camera_id) << "\","
     << "\"session_id\":\"" << JsonEscape(session_id) << "\","
     << "\"ice\":\"" << JsonEscape(ice_state) << "\","
     << "\"peer\":\"" << JsonEscape(peer_state) << "\""
     << "}";
  return os.str();
}

// =========================================================================
// NATS subject helpers
// =========================================================================

std::string MakeSessionSubject(const std::string& camera_id,
                               const std::string& session_id,
                               const std::string& suffix) {
  return "webrtc.cam." + camera_id + ".session." + session_id + "." + suffix;
}

bool ParseSessionSubject(const std::string& subject,
                         std::string& camera_id,
                         std::string& session_id,
                         std::string& suffix) {
  // Expected format: webrtc.cam.{camera_id}.session.{session_id}.{suffix}
  const char* prefix = "webrtc.cam.";
  if (subject.compare(0, 11, prefix) != 0) return false;

  const char* p = subject.c_str() + 11;

  // Extract camera_id (chars until ".session.")
  const char* cam_start = p;
  const char* sess_marker = strstr(p, ".session.");
  if (!sess_marker) return false;
  camera_id.assign(cam_start, static_cast<size_t>(sess_marker - cam_start));

  p = sess_marker + 9;  // past ".session."

  // Extract session_id (chars until next '.')
  const char* sid_start = p;
  const char* dot = strchr(p, '.');
  if (!dot) return false;
  session_id.assign(sid_start, static_cast<size_t>(dot - sid_start));

  suffix = std::string(dot + 1);
  return true;
}

}  // namespace aibox::webrtc
