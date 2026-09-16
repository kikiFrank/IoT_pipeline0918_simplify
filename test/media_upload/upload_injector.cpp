// IR-UPLOAD-001 injector: writes a file, publishes alarm.event, sleeps, then media_ready (same event_id).
#include "aibox/adapter/NatsAdapter.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {
std::string Q(const std::string& s) { return std::string() + '"' + s + '"'; }
}

int main(int argc, char** argv) {
  std::string url = "nats://127.0.0.1:4222";
  std::string event_id = "test-evt-1";
  std::string alarm_id = "test-alm-1";
  std::string file_path = "/tmp/upload_test/test-evt-1.jpg";
  int sleep_ms = 2000;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--url" && i + 1 < argc) url = argv[++i];
    else if (a == "--event-id" && i + 1 < argc) event_id = argv[++i];
    else if (a == "--alarm-id" && i + 1 < argc) alarm_id = argv[++i];
    else if (a == "--file" && i + 1 < argc) file_path = argv[++i];
    else if (a == "--sleep-ms" && i + 1 < argc) sleep_ms = std::atoi(argv[++i]);
  }

  const std::string body = "IR-UPLOAD-001 synthetic image body";
  const long file_size = static_cast<long>(body.size());
  {
    std::ofstream f(file_path, std::ios::binary);
    if (!f.is_open()) { std::cerr << "[ERROR] cannot write " << file_path << std::endl; return 2; }
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    f.close();
    std::cout << "[INFO] wrote file " << file_path << " bytes=" << file_size << std::endl;
  }

  aibox::adapter::NatsAdapter nats;
  aibox::adapter::NatsAdapter::Config cfg;
  cfg.nats_url = url;
  cfg.client_name = "aibox-upload-injector";
  if (!nats.Connect(cfg)) { std::cerr << "[ERROR] connect failed: " << url << std::endl; return 1; }

  const std::string ts = std::to_string(static_cast<long long>(std::time(nullptr)) * 1000LL);
  const std::string alarm = R"({"alarm_id":)" + Q(alarm_id) +
      R"(,"event_id":)" + Q(event_id) +
      R"(,"rule_id":)" + Q("standard_actions") +
      R"(,"severity":2)" +
      R"(,"label":)" + Q("test") +
      R"(,"description":)" + Q("IR-UPLOAD-001 synthetic alarm") +
      R"(,"timestamp_ms":)" + ts +
      R"(,"local_path":)" + Q("") +
      R"(,"camera_id":)" + Q("cam-001") +
      R"(,"zone_id":)" + Q("zone-cam-001") +
      R"(,"action_type":)" + Q("Alert") + "}";

  const std::string media = R"({"event_id":)" + Q(event_id) +
      R"(,"file_path":)" + Q(file_path) +
      R"(,"size_bytes":)" + std::to_string(file_size) + "}";

  nats.Publish("alarm.event", alarm);
  std::cout << "[INFO] published alarm.event event_id=" << event_id << std::endl;
  if (sleep_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  nats.Publish("event.stream.media_ready", media);
  std::cout << "[INFO] published media_ready event_id=" << event_id << " alarm_id=" << alarm_id << std::endl;

  nats.Disconnect();
  aibox::adapter::NatsAdapter::CloseLibraryAndWait();
  return 0;
}
