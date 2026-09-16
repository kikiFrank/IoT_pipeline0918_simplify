#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aibox::webrtc {

// =========================================================================
// JSON message payloads — one struct per NATS message type.
// Field names match the JSON wire format (camelCase).
// =========================================================================

// --- ice_servers entry (from session.new) ---
struct IceServer {
  std::string url;         // "stun:..." or "turn:..."
  std::string username;    // TURN only
  std::string credential;  // TURN only

  bool IsStun() const;     // url starts with "stun:"
  bool IsTurn() const;     // url starts with "turn:"
  // turn://user:pass@host:port format for GStreamer turn-server property
  std::string ToTurnUrl() const;
};

// --- session.new (NATS → IMSDK) ---
struct SessionNewPayload {
  std::string camera_id;
  std::string session_id;
  std::string rtsp_url;
  std::vector<IceServer> ice_servers;
};

// --- session.offer / session.answer (SDP messages) ---
struct SdpPayload {
  std::string camera_id;
  std::string session_id;
  std::string sdp;
};

// --- session.ice.local / session.ice.remote ---
struct IceCandidatePayload {
  std::string camera_id;
  std::string session_id;
  std::string sdpMid;        // JSON: sdpMid  ↔ GStreamer: sdp_mid
  int         sdpMLineIndex = 0;  // JSON: sdpMLineIndex ↔ Gst: sdp_mline_index
  std::string candidate;
};

// --- session.state (IMSDK → NATS) ---
struct SessionStatePayload {
  std::string camera_id;
  std::string session_id;
  std::string ice;    // checking|connected|completed|failed|disconnected|closed
  std::string peer;   // new|connecting|connected|disconnected|failed|closed
};

// --- session.close (NATS → IMSDK) ---
struct SessionClosePayload {
  std::string camera_id;
  std::string session_id;
  std::string reason;  // client_disconnect|session_timeout|ice_failed|server_shutdown
};

// --- session.renegotiate (NATS → IMSDK) ---
struct RenegotiatePayload {
  std::string camera_id;
  std::string session_id;
  int width = 0;
  int height = 0;
  int max_kbps = 0;  // 0 = unlimited
};

// =========================================================================
// JSON builder/parser — minimal, no external library dependency.
// =========================================================================

// Parse helpers: extract a string field from a JSON object string.
// Returns empty string if field not found.
std::string JsonGetString(const std::string& json, const std::string& key);

// Extract int field. Returns def if not found.
int JsonGetInt(const std::string& json, const std::string& key, int def = 0);

// Extract array of objects (for ice_servers). Returns the raw substring
// between '[' and ']'. Caller parses individual entries.
std::string JsonGetArray(const std::string& json, const std::string& key);

// Parse a single ice_servers entry object → IceServer
IceServer ParseIceServer(const std::string& obj);

// Parse full session.new JSON → SessionNewPayload
SessionNewPayload ParseSessionNew(const std::string& json);

// Parse SDP payload (offer/answer) JSON
SdpPayload ParseSdpPayload(const std::string& json);

// Parse ICE candidate (local/remote) JSON
IceCandidatePayload ParseIceCandidate(const std::string& json);

// Parse session.close JSON
SessionClosePayload ParseSessionClose(const std::string& json);

// Parse session.renegotiate JSON
RenegotiatePayload ParseRenegotiate(const std::string& json);

// --- Build helpers ---

// Escape a string for JSON (quote, backslash, control chars)
std::string JsonEscape(const std::string& s);

// Build session.offer / session.answer JSON
std::string BuildSdpJson(const std::string& camera_id,
                         const std::string& session_id,
                         const std::string& sdp,
                         const std::string& type);  // "offer" or "answer"

// Build session.ice.local JSON
std::string BuildIceJson(const std::string& camera_id,
                         const std::string& session_id,
                         const std::string& sdp_mid,
                         int sdp_mline_index,
                         const std::string& candidate);

// Build session.state JSON
std::string BuildStateJson(const std::string& camera_id,
                           const std::string& session_id,
                           const std::string& ice_state,
                           const std::string& peer_state);

// =========================================================================
// NATS subject helpers
// =========================================================================

// Build a publish subject: webrtc.cam.{camera_id}.session.{session_id}.{suffix}
std::string MakeSessionSubject(const std::string& camera_id,
                               const std::string& session_id,
                               const std::string& suffix);

// Parse camera_id and session_id from a NATS subject string like
// "webrtc.cam.{camera_id}.session.{session_id}.{suffix}"
// Returns true if parsing succeeded.
bool ParseSessionSubject(const std::string& subject,
                         std::string& camera_id,
                         std::string& session_id,
                         std::string& suffix);

}  // namespace aibox::webrtc
