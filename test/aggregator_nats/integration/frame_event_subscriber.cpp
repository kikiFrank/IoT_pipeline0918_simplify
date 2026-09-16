#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "aibox/adapter/NatsAdapter.hpp"
#include "aibox/detection/NatsFrameTransport.hpp"
#include "FrameEventChecks.hpp"

namespace {

struct Args {
  std::string url = "nats://127.0.0.1:4222";
  std::string subject = aibox::detection::NatsFrameTransport::kSubject;
  int seconds = 8;
  uint64_t expected_count = 0;
  uint64_t min_detections = 0;
  std::string expected_camera_id;
  uint32_t frame_width = 0;
  uint32_t frame_height = 0;
};

void PrintHelp(const char* prog) {
  std::cout << "Usage: " << prog
            << " [--url nats://127.0.0.1:4222] [--subject detection.frame]"
            << " [--seconds 8] [--expected-count N] [--min-detections N]"
            << " [--expected-camera-id ID] [--frame-width W] [--frame-height H]\n";
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
    } else if (arg == "--subject") {
      args.subject = next("--subject");
    } else if (arg == "--seconds") {
      args.seconds = std::atoi(next("--seconds"));
    } else if (arg == "--expected-count") {
      args.expected_count = static_cast<uint64_t>(std::atoll(next("--expected-count")));
    } else if (arg == "--min-detections") {
      args.min_detections = static_cast<uint64_t>(std::atoll(next("--min-detections")));
    } else if (arg == "--expected-camera-id") {
      args.expected_camera_id = next("--expected-camera-id");
    } else if (arg == "--frame-width") {
      args.frame_width = static_cast<uint32_t>(std::atoi(next("--frame-width")));
    } else if (arg == "--frame-height") {
      args.frame_height = static_cast<uint32_t>(std::atoi(next("--frame-height")));
    } else {
      std::cerr << "[WARN] unknown argument: " << arg << std::endl;
    }
  }
  if (args.seconds <= 0) args.seconds = 8;
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  const Args args = ParseArgs(argc, argv);

  aibox::adapter::NatsAdapter nats;
  aibox::adapter::NatsAdapter::Config cfg;
  cfg.nats_url = args.url;
  cfg.client_name = "aibox-frame-event-subscriber";
  cfg.max_pending = 0;
  if (!nats.Connect(cfg)) {
    std::cerr << "[ERROR] connect failed: " << args.url << std::endl;
    return 1;
  }

  std::mutex mu;
  aibox::test::FrameEventCheckStats stats;
  int printed_nonempty = 0;
  uint64_t camera_mismatch = 0;
  if (!nats.Subscribe(args.subject, [&](const std::string&, const std::string&,
                                        const std::string& payload) {
        std::lock_guard<std::mutex> lock(mu);
        aibox::test::CheckFrameEventPayload(payload, stats, args.frame_width,
                                            args.frame_height);
        aibox::FrameEvent frame;
        if (!frame.ParseFromArray(payload.data(),
                                  static_cast<int>(payload.size()))) {
          return;
        }
        if (!args.expected_camera_id.empty() &&
            frame.camera_id() != args.expected_camera_id) {
          ++camera_mismatch;
        }
        if (frame.detections_size() > 0 && printed_nonempty < 5) {
          ++printed_nonempty;
          std::cout << "[DET] camera=" << frame.camera_id()
                    << " ts=" << frame.timestamp_ms()
                    << " dets=" << frame.detections_size();
          for (int i = 0; i < frame.detections_size(); ++i) {
            const auto& d = frame.detections(i);
            std::cout << " | " << d.class_id()
                      << " conf=" << d.confidence()
                      << " id=" << d.event_id()
                      << " bbox=[" << d.x_min() << "," << d.y_min()
                      << "," << d.x_max() << "," << d.y_max() << "]";
          }
          std::cout << std::endl;
        }
      })) {
    std::cerr << "[ERROR] subscribe failed: " << args.subject << std::endl;
    nats.Disconnect();
    aibox::adapter::NatsAdapter::CloseLibraryAndWait();
    return 1;
  }

  std::cout << "[INFO] subscribed subject=" << args.subject
            << " url=" << args.url
            << " seconds=" << args.seconds << std::endl;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(args.seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (args.expected_count > 0) {
      std::lock_guard<std::mutex> lock(mu);
      if (stats.received >= args.expected_count) break;
    }
  }

  nats.Disconnect();
  aibox::adapter::NatsAdapter::CloseLibraryAndWait();

  aibox::test::PrintFrameEventCheckStats(stats);
  google::protobuf::ShutdownProtobufLibrary();

  if (stats.received == 0) {
    std::cerr << "[ERROR] no FrameEvent received" << std::endl;
    return 2;
  }
  if (args.expected_count > 0 && stats.received < args.expected_count) {
    std::cerr << "[ERROR] expected_count=" << args.expected_count
              << " received=" << stats.received << std::endl;
    return 1;
  }
  if (camera_mismatch > 0) {
    std::cerr << "[ERROR] camera_id mismatch count=" << camera_mismatch
              << " expected=" << args.expected_camera_id << std::endl;
    return 1;
  }
  if (args.min_detections > 0 && stats.detections < args.min_detections) {
    std::cerr << "[ERROR] min_detections=" << args.min_detections
              << " detections=" << stats.detections << std::endl;
    return 1;
  }
  if (!aibox::test::FrameEventChecksPassed(stats)) {
    std::cerr << "[ERROR] FrameEvent contract checks failed" << std::endl;
    return 1;
  }
  std::cout << "[INFO] FrameEvent subscriber checks passed" << std::endl;
  return 0;
}
