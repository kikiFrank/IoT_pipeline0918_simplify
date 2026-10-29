#include "aibox/media/SnapshotRequestSubscriber.hpp"

#include <cstdlib>
#include <iostream>
#include <utility>

#include "aibox/media/MediaReadyEvent.hpp"

namespace aibox::media {
namespace {

// 极简的 JSON 字段读取器。管线没有链接任何 JSON 库，而且
// 这里的 payload 都是扁平对象、UUID / id 值不带转义，因此
// 定向扫描已经足够。无法解析的内容一律拒绝，而不是
// 去猜测。
bool ExtractString(const std::string& json, const std::string& key,
                   std::string& out) {
  const std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return false;
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                               json[pos] == '\n' || json[pos] == '\r')) {
    ++pos;
  }
  if (pos >= json.size() || json[pos] != '"') return false;
  ++pos;
  std::string value;
  while (pos < json.size()) {
    const char c = json[pos];
    if (c == '\\' && pos + 1 < json.size()) {
      value += json[pos + 1];
      pos += 2;
      continue;
    }
    if (c == '"') {
      out = std::move(value);
      return true;
    }
    value += c;
    ++pos;
  }
  return false;
}

bool ExtractInt64(const std::string& json, const std::string& key,
                  int64_t& out) {
  const std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return false;
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  const char* start = json.c_str() + pos + 1;
  char* end = nullptr;
  const long long value = std::strtoll(start, &end, 10);
  if (end == start) return false;
  out = static_cast<int64_t>(value);
  return true;
}


}  // namespace

SnapshotRequestSubscriber::SnapshotRequestSubscriber(
    aibox::adapter::NatsAdapter& nats, AnomalySnapshotService& service)
    : nats_(nats), service_(service) {}

SnapshotRequestSubscriber::~SnapshotRequestSubscriber() { Stop(); }

bool SnapshotRequestSubscriber::Parse(const std::string& json,
                                      AlarmSnapshotRequest& out) {
  AlarmSnapshotRequest parsed;
  if (!ExtractString(json, "alarm_id", parsed.alarm_id) ||
      parsed.alarm_id.empty()) {
    return false;
  }
  if (!ExtractString(json, "camera_id", parsed.camera_id) ||
      parsed.camera_id.empty()) {
    return false;
  }
  // 可选字段：缺失表示「仅按相机和时间匹配」。
  ExtractString(json, "frame_id", parsed.frame_id);
  ExtractString(json, "source_event_id", parsed.source_event_id);
  if (!ExtractInt64(json, "occurred_at_ms", parsed.occurred_at_ms)) {
    parsed.occurred_at_ms = 0;
  }
  out = std::move(parsed);
  return true;
}

bool SnapshotRequestSubscriber::Start() {
  if (started_.exchange(true)) return true;

  service_.SetFailureSink([this](const std::string& alarm_id,
                                 const std::string& reason) {
    PublishFailure(alarm_id, reason);
  });

  const bool ok = nats_.Subscribe(
      kSubject, [this](const std::string& subject, const std::string& reply,
                       const std::string& payload) {
        OnMessage(subject, reply, payload);
      });
  if (!ok) {
    service_.SetFailureSink(nullptr);
    started_.store(false);
    std::cerr << "[WARN] snapshot request subscribe failed subject="
              << kSubject << std::endl;
  } else {
    std::cout << "[INFO] SnapshotRequestSubscriber listening on " << kSubject
              << std::endl;
  }
  return ok;
}

void SnapshotRequestSubscriber::Stop() {
  if (!started_.exchange(false)) return;
  nats_.Unsubscribe(kSubject);
  // 该 sink 捕获了 `this`；必须在本对象销毁前清除。
  service_.SetFailureSink(nullptr);
}

void SnapshotRequestSubscriber::OnMessage(const std::string& /*subject*/,
                                          const std::string& /*reply*/,
                                          const std::string& payload) {
  AlarmSnapshotRequest request;
  if (!Parse(payload, request)) {
    rejected_.fetch_add(1);
    std::cerr << "[WARN] snapshot request rejected: unparsable payload"
              << std::endl;
    return;
  }
  received_.fetch_add(1);

  // 这里只做解析和入队。编码和磁盘 I/O 都在服务的
  // worker 线程上进行，绝不在此处执行。
  service_.EnqueueAlarmSnapshotRequest(std::move(request));
}

void SnapshotRequestSubscriber::PublishFailure(const std::string& alarm_id,
                                               const std::string& reason) {
  std::string json = "{\"alarm_id\":\"";
  json += MediaReadyEvent::EscapeJson(alarm_id);
  json += "\",\"reason\":\"";
  json += MediaReadyEvent::EscapeJson(reason);
  json += "\"}";
  if (!nats_.Publish(kFailedSubject, json)) {
    std::cerr << "[WARN] snapshot failed publish failed alarm_id=" << alarm_id
              << " reason=" << reason << std::endl;
  }
}

uint64_t SnapshotRequestSubscriber::Received() const {
  return received_.load();
}

uint64_t SnapshotRequestSubscriber::Rejected() const {
  return rejected_.load();
}

}  // namespace aibox::media
