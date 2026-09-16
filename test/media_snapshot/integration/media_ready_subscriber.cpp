#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include "aibox/adapter/NatsAdapter.hpp"
#include "aibox/detection/NatsFrameTransport.hpp"
#include "aibox/media/NatsMediaReadyTransport.hpp"
#include "aibox_contracts.pb.h"

namespace {

struct Args {
  std::string url = "nats://127.0.0.1:4222";
  int seconds = 35;
  int run_seconds = 25;
  int camera_count = 1;
  uint64_t min_detections = 0;
  std::string expected_camera_id = "cam-001";
  bool expect_zero_ready = false;
};

void PrintHelp(const char* prog) {
  std::cout << "Usage: " << prog
            << " [--url nats://127.0.0.1:4222] [--seconds 35]"
            << " [--run-seconds 25] [--camera-count 1]"
            << " [--min-detections N] [--expected-camera-id cam-001]"
            << " [--expect-zero-ready]\n";
}

Args ParseArgs(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "[ERROR] missing value for " << name << std::endl;
        std::exit(1);
      }
      return argv[++i];
    };
    if (arg == "--help") {
      PrintHelp(argv[0]);
      std::exit(0);
    } else if (arg == "--url") {
      args.url = next("--url");
    } else if (arg == "--seconds") {
      args.seconds = std::atoi(next("--seconds"));
    } else if (arg == "--run-seconds") {
      args.run_seconds = std::atoi(next("--run-seconds"));
    } else if (arg == "--camera-count") {
      args.camera_count = std::atoi(next("--camera-count"));
    } else if (arg == "--min-detections") {
      args.min_detections = static_cast<uint64_t>(std::atoll(next("--min-detections")));
    } else if (arg == "--expected-camera-id") {
      args.expected_camera_id = next("--expected-camera-id");
    } else if (arg == "--expect-zero-ready") {
      args.expect_zero_ready = true;
    } else {
      std::cerr << "[WARN] unknown argument: " << arg << std::endl;
    }
  }
  if (args.seconds <= 0) args.seconds = 35;
  if (args.run_seconds <= 0) args.run_seconds = 25;
  if (args.camera_count <= 0) args.camera_count = 1;
  return args;
}

bool ExtractJsonString(const std::string& json, const std::string& key,
                       std::string& out) {
  const std::string needle = "\"" + key + "\":\"";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return false;
  size_t i = pos + needle.size();
  out.clear();
  while (i < json.size()) {
    if (json[i] == '\\' && i + 1 < json.size()) {
      out.push_back(json[i + 1]);
      i += 2;
      continue;
    }
    if (json[i] == '"') return true;
    out.push_back(json[i]);
    ++i;
  }
  return false;
}

bool ExtractJsonInt64(const std::string& json, const std::string& key,
                      int64_t& out) {
  const std::string needle = "\"" + key + "\":";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return false;
  char* end = nullptr;
  const long long value =
      std::strtoll(json.c_str() + pos + needle.size(), &end, 10);
  if (end == json.c_str() + pos + needle.size()) return false;
  out = static_cast<int64_t>(value);
  return true;
}

bool LooksLikeJpegFile(const std::string& path, int64_t expected_size) {
  struct stat st {};
  if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
  if (static_cast<int64_t>(st.st_size) != expected_size) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
  if (bytes.size() < 4) return false;
  return bytes[0] == 0xFF && bytes[1] == 0xD8 &&
         bytes[bytes.size() - 2] == 0xFF && bytes[bytes.size() - 1] == 0xD9;
}

// Same as above but without a size expectation (thumbnail size is unknown).
bool LooksLikeJpegFileAny(const std::string& path) {
  struct stat st {};
  if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
  if (bytes.size() < 4) return false;
  return bytes[0] == 0xFF && bytes[1] == 0xD8 &&
         bytes[bytes.size() - 2] == 0xFF && bytes[bytes.size() - 1] == 0xD9;
}

// file_path "/var/aibox/media/<id>.jpg" → thumbnail "<id>.thumb.jpg".
std::string ExpectedThumbPath(const std::string& file_path) {
  const std::string suffix = ".jpg";
  if (file_path.size() > suffix.size() &&
      file_path.compare(file_path.size() - suffix.size(), suffix.size(),
                        suffix) == 0) {
    return file_path.substr(0, file_path.size() - suffix.size()) + ".thumb.jpg";
  }
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  const Args args = ParseArgs(argc, argv);

  aibox::adapter::NatsAdapter nats;
  aibox::adapter::NatsAdapter::Config cfg;
  cfg.nats_url = args.url;
  cfg.client_name = "aibox-media-ready-subscriber";
  cfg.max_pending = 0;
  if (!nats.Connect(cfg)) {
    std::cerr << "[ERROR] connect failed: " << args.url << std::endl;
    return 1;
  }

  std::mutex mu;
  uint64_t decode_ok = 0;
  uint64_t decode_fail = 0;
  uint64_t detections = 0;
  uint64_t empty_frames = 0;
  std::set<std::string> frame_event_ids;
  std::vector<std::string> ready_payloads;
  uint64_t ready_bad_subject = 0;

  const char* frame_subject = aibox::detection::NatsFrameTransport::kSubject;
  const char* ready_subject = aibox::media::NatsMediaReadyTransport::kSubject;

  if (!nats.Subscribe(frame_subject, [&](const std::string&, const std::string&,
                                         const std::string& payload) {
        std::lock_guard<std::mutex> lock(mu);
        aibox::FrameEvent frame;
        if (!frame.ParseFromArray(payload.data(),
                                  static_cast<int>(payload.size()))) {
          ++decode_fail;
          return;
        }
        ++decode_ok;
        if (frame.detections_size() == 0) {
          ++empty_frames;
          return;
        }
        detections += static_cast<uint64_t>(frame.detections_size());
        for (const auto& det : frame.detections()) {
          frame_event_ids.insert(det.event_id());
        }
      })) {
    std::cerr << "[ERROR] subscribe failed: " << frame_subject << std::endl;
    nats.Disconnect();
    aibox::adapter::NatsAdapter::CloseLibraryAndWait();
    return 1;
  }

  if (!nats.Subscribe(ready_subject, [&](const std::string& subject,
                                         const std::string&,
                                         const std::string& payload) {
        std::lock_guard<std::mutex> lock(mu);
        if (subject != ready_subject) {
          ++ready_bad_subject;
          return;
        }
        ready_payloads.push_back(payload);
      })) {
    std::cerr << "[ERROR] subscribe failed: " << ready_subject << std::endl;
    nats.Disconnect();
    aibox::adapter::NatsAdapter::CloseLibraryAndWait();
    return 1;
  }

  std::cout << "[INFO] subscribed " << frame_subject << " + " << ready_subject
            << " url=" << args.url << " seconds=" << args.seconds << std::endl;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(args.seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  nats.Disconnect();
  aibox::adapter::NatsAdapter::CloseLibraryAndWait();

  uint64_t ready_msgs = 0;
  uint64_t ready_field_fail = 0;
  uint64_t ready_id_unknown = 0;
  uint64_t ready_file_fail = 0;
  uint64_t ready_thumb_fail = 0;
  std::set<std::string> jpeg_paths;
  std::set<std::string> thumb_paths;
  {
    std::lock_guard<std::mutex> lock(mu);
    ready_msgs = ready_payloads.size();
    std::cout << "[INFO] decode_ok=" << decode_ok
              << " decode_fail=" << decode_fail
              << " empty_frames=" << empty_frames
              << " detections=" << detections
              << " unique_event_ids=" << frame_event_ids.size()
              << " ready_msgs=" << ready_msgs
              << " ready_bad_subject=" << ready_bad_subject << std::endl;

    for (const auto& payload : ready_payloads) {
      if (payload.empty() || payload.front() != '{') {
        ++ready_field_fail;
        continue;
      }
      std::string event_id;
      std::string file_path;
      std::string thumbnail_path;
      std::string camera_id;
      int64_t size_bytes = 0;
      int64_t timestamp_ms = 0;
      if (!ExtractJsonString(payload, "event_id", event_id) ||
          !ExtractJsonString(payload, "file_path", file_path) ||
          !ExtractJsonString(payload, "thumbnail_path", thumbnail_path) ||
          !ExtractJsonString(payload, "camera_id", camera_id) ||
          !ExtractJsonInt64(payload, "size_bytes", size_bytes) ||
          !ExtractJsonInt64(payload, "timestamp_ms", timestamp_ms)) {
        ++ready_field_fail;
        continue;
      }
      if (!args.expected_camera_id.empty() &&
          camera_id != args.expected_camera_id) {
        ++ready_field_fail;
        continue;
      }
      if (file_path.empty() || file_path[0] != '/') {
        ++ready_field_fail;
        continue;
      }
      if (frame_event_ids.find(event_id) == frame_event_ids.end()) {
        ++ready_id_unknown;
      }
      if (!LooksLikeJpegFile(file_path, size_bytes)) {
        ++ready_file_fail;
      } else {
        jpeg_paths.insert(file_path);
      }
      // Pipeline must have produced the thumbnail: path matches the
      // <id>.thumb.jpg convention and the file exists as a JPEG.
      const std::string expected_thumb = ExpectedThumbPath(file_path);
      if (thumbnail_path.empty() || expected_thumb.empty() ||
          thumbnail_path != expected_thumb ||
          !LooksLikeJpegFileAny(thumbnail_path)) {
        ++ready_thumb_fail;
      } else {
        thumb_paths.insert(thumbnail_path);
      }
      if (jpeg_paths.size() <= 3) {
        std::cout << "[READY] event_id=" << event_id
                  << " file_path=" << file_path
                  << " thumbnail_path=" << thumbnail_path
                  << " size_bytes=" << size_bytes
                  << " camera_id=" << camera_id
                  << " timestamp_ms=" << timestamp_ms << std::endl;
      }
    }
  }

  const uint64_t jpeg_count = jpeg_paths.size();
  const uint64_t jpeg_cap =
      static_cast<uint64_t>(args.run_seconds + args.camera_count);
  std::cout << "[INFO] jpeg_count=" << jpeg_count
            << " thumb_count=" << thumb_paths.size()
            << " jpeg_cap=" << jpeg_cap
            << " ready_field_fail=" << ready_field_fail
            << " ready_id_unknown=" << ready_id_unknown
            << " ready_file_fail=" << ready_file_fail
            << " ready_thumb_fail=" << ready_thumb_fail << std::endl;

  google::protobuf::ShutdownProtobufLibrary();

  if (decode_ok == 0) {
    std::cerr << "[ERROR] no FrameEvent received" << std::endl;
    return 2;
  }
  if (args.min_detections > 0 && detections < args.min_detections) {
    std::cerr << "[SKIP] min_detections=" << args.min_detections
              << " detections=" << detections << std::endl;
    return 77;
  }
  if (ready_bad_subject > 0 || ready_field_fail > 0 || ready_id_unknown > 0 ||
      ready_file_fail > 0 || ready_thumb_fail > 0) {
    std::cerr << "[ERROR] media_ready field/path/event_id/thumbnail checks "
                 "failed"
              << std::endl;
    return 1;
  }

  if (args.expect_zero_ready) {
    if (ready_msgs != 0) {
      std::cerr << "[ERROR] expected zero media_ready, got " << ready_msgs
                << std::endl;
      return 1;
    }
    std::cout << "[INFO] write-fail path: ready_msgs=0 as expected" << std::endl;
    return 0;
  }

  if (jpeg_count > jpeg_cap) {
    std::cerr << "[ERROR] jpeg_count=" << jpeg_count
              << " exceeds cap=" << jpeg_cap << " (holdoff broken)" << std::endl;
    return 1;
  }
  if (!(jpeg_count >= 1 && thumb_paths.size() >= 1 &&
        ready_msgs >= jpeg_count && ready_msgs < decode_ok)) {
    std::cerr << "[ERROR] gate failed jpeg_count=" << jpeg_count
              << " thumb_count=" << thumb_paths.size()
              << " ready_msgs=" << ready_msgs
              << " detection_frames=" << decode_ok << std::endl;
    return 1;
  }

  std::cout << "[INFO] media_ready subscriber checks passed" << std::endl;
  return 0;
}
