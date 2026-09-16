// IR-UPLOAD-001 verifier: counts event.datamgr.alarm_ready + event.media.uploaded.
#include "aibox/adapter/NatsAdapter.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;
void OnSignal(int) { g_stop = 1; }

std::string ExtractField(const std::string& json, const std::string& key) {
  const std::string needle = std::string() + '"' + key + '"' + ':';
  const size_t pos = json.find(needle);
  if (pos == std::string::npos) return "";
  size_t start = pos + needle.size();
  if (start < json.size() && json[start] == '"') {
    ++start;
    const size_t end = json.find('"', start);
    if (end == std::string::npos) return "";
    return json.substr(start, end - start);
  }
  const size_t end = json.find_first_of(",}", start);
  if (end == std::string::npos) return json.substr(start);
  return json.substr(start, end - start);
}

}  // namespace

int main(int argc, char** argv) {
  std::string url = "nats://127.0.0.1:4222";
  int seconds = 20;
  std::string out_path;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--url" && i + 1 < argc) url = argv[++i];
    else if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
    else if (a == "--out" && i + 1 < argc) out_path = argv[++i];
  }
  if (seconds <= 0) seconds = 20;

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  aibox::adapter::NatsAdapter nats;
  aibox::adapter::NatsAdapter::Config cfg;
  cfg.nats_url = url;
  cfg.client_name = "aibox-upload-verifier";
  if (!nats.Connect(cfg)) {
    std::cerr << "[ERROR] connect failed: " << url << std::endl;
    return 1;
  }

  struct Stats {
    std::mutex mu;
    std::atomic<uint64_t> alarm_ready{0};
    std::atomic<uint64_t> uploaded{0};
    std::map<std::string, std::string> ready_by_event;
    std::vector<std::string> uploaded_events;
    std::vector<std::string> cloud_urls;
  } s;

  nats.Subscribe("event.datamgr.alarm_ready",
        [&](const std::string& /*subject*/, const std::string& /*reply*/, const std::string& payload) {
          s.alarm_ready.fetch_add(1);
          const std::string eid = ExtractField(payload, "event_id");
          const std::string aid = ExtractField(payload, "alarm_id");
          std::lock_guard<std::mutex> lk(s.mu);
          if (!eid.empty()) s.ready_by_event[eid] = aid;
        });

  nats.Subscribe("event.media.uploaded",
        [&](const std::string& /*subject*/, const std::string& /*reply*/, const std::string& payload) {
          s.uploaded.fetch_add(1);
          const std::string eid = ExtractField(payload, "event_id");
          const std::string cid = ExtractField(payload, "cloud_url");
          std::lock_guard<std::mutex> lk(s.mu);
          if (!eid.empty()) s.uploaded_events.push_back(eid);
          if (!cid.empty()) s.cloud_urls.push_back(cid);
        });

  std::cout << "[INFO] verifier ready url=" << url << " seconds=" << seconds << std::endl;

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (!g_stop && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  nats.Disconnect();
  aibox::adapter::NatsAdapter::CloseLibraryAndWait();

  std::ostringstream sum;
  sum << "alarm_ready=" << s.alarm_ready.load() << "\n";
  sum << "media_uploaded=" << s.uploaded.load() << "\n";
  {
    std::lock_guard<std::mutex> lk(s.mu);
    for (const auto& kv : s.ready_by_event) sum << "ready_" << kv.first << "=" << kv.second << "\n";
    for (size_t i = 0; i < s.uploaded_events.size(); ++i) sum << "uploaded_" << i << "=" << s.uploaded_events[i] << "\n";
    for (size_t i = 0; i < s.cloud_urls.size(); ++i) sum << "cloud_url_" << i << "=" << s.cloud_urls[i] << "\n";
  }
  const std::string summary = sum.str();
  std::cout << "==== SUMMARY ====\n" << summary << "==== END ====" << std::endl;
  if (!out_path.empty()) {
    std::ofstream f(out_path);
    if (f.is_open()) { f << summary; f.close(); }
    std::cout << "[INFO] summary written to " << out_path << std::endl;
  }

  const bool ok = s.alarm_ready.load() > 0 && s.uploaded.load() > 0;
  std::cout << (ok ? "[VERDICT] PASS" : "[VERDICT] FAIL") << std::endl;
  return ok ? 0 : 1;
}
