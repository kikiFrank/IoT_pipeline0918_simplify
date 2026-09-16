#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "IsolatedNats.hpp"
#include "FrameEventChecks.hpp"
#include "aibox/adapter/NatsAdapter.hpp"
#include "aibox/aggregator/ResultAggregator.hpp"
#include "aibox/detection/FrameEventPublisher.hpp"
#include "aibox/detection/NatsFrameTransport.hpp"

namespace {

constexpr int kSkipNoNats = 77;
constexpr int kPort = 14224;

#define CHECK_TRUE(expr)                                                     \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::cerr << "CHECK failed: " #expr << " line=" << __LINE__ << '\n'; \
      return false;                                                          \
    }                                                                        \
  } while (false)

aibox::aggregator::AggregatedResult MakeFrame(int index,
                                             const char* camera_id = "cam-001") {
  aibox::aggregator::AggregatedResult result;
  result.camera_id = camera_id;
  result.frame_id = static_cast<uint64_t>(index);
  result.capture_timestamp_ms = 1750000000000ULL + static_cast<uint64_t>(index) * 40;
  result.frame_width = 1920;
  result.frame_height = 1080;
  if (index % 5 == 0) return result;
  aibox::aggregator::Detection det;
  det.label = "person";
  det.confidence = 90.0f;
  det.x = 10;
  det.y = 10;
  det.w = 20;
  det.h = 20;
  result.detections.push_back(det);
  return result;
}

bool WaitForConnect(aibox::adapter::NatsAdapter& nats,
                    const aibox::adapter::NatsAdapter::Config& cfg,
                    int attempts) {
  for (int i = 0; i < attempts; ++i) {
    if (nats.Connect(cfg)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

bool WaitAvailable(aibox::adapter::NatsAdapter& nats, int attempts) {
  for (int i = 0; i < attempts; ++i) {
    if (nats.IsAvailable()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

bool SubmitAndWait(aibox::aggregator::ResultAggregator& aggregator,
                   aibox::test::FrameEventCheckStats& stats,
                   std::mutex& mu, std::condition_variable& cv,
                   int start_index, int count, uint64_t already_received,
                   int timeout_s) {
  for (int i = 0; i < count; ++i) {
    CHECK_TRUE(aggregator.Submit(MakeFrame(start_index + i)));
  }
  std::unique_lock<std::mutex> lock(mu);
  const bool ok = cv.wait_for(lock, std::chrono::seconds(timeout_s), [&] {
    return stats.received >= already_received + static_cast<uint64_t>(count);
  });
  if (!ok) {
    std::cerr << "wait failed received=" << stats.received
              << " want=" << (already_received + count) << std::endl;
  }
  return ok;
}

bool RunReconnect(const std::string& nats_bin) {
  aibox::test::IsolatedNatsGuard nats;
  nats.reset(aibox::test::StartIsolatedNats(nats_bin, kPort,
                                            "aibox-events-nfr2"));
  CHECK_TRUE(nats.pid > 0);
  if (!aibox::test::WaitForPort(kPort, 40)) return false;

  aibox::adapter::NatsAdapter::Config cfg;
  cfg.nats_url = "nats://127.0.0.1:" + std::to_string(kPort);
  cfg.max_pending = 0;
  cfg.client_name = "aibox-nfr2-sub";

  aibox::adapter::NatsAdapter subscriber;
  CHECK_TRUE(WaitForConnect(subscriber, cfg, 30));

  std::mutex mu;
  std::condition_variable cv;
  aibox::test::FrameEventCheckStats stats;
  CHECK_TRUE(subscriber.Subscribe(
      aibox::detection::NatsFrameTransport::kSubject,
      [&](const std::string&, const std::string&, const std::string& payload) {
        {
          std::lock_guard<std::mutex> lock(mu);
          aibox::test::CheckFrameEventPayload(payload, stats, 1920, 1080);
        }
        cv.notify_all();
      }));

  cfg.client_name = "aibox-nfr2-pub";
  aibox::adapter::NatsAdapter publisher_nats;
  CHECK_TRUE(WaitForConnect(publisher_nats, cfg, 10));
  aibox::detection::NatsFrameTransport transport(publisher_nats);
  aibox::aggregator::ResultAggregator::Options agg_opts;
  agg_opts.per_camera_queue_depth = 256;
  agg_opts.log_every = 0;
  aibox::aggregator::ResultAggregator aggregator(agg_opts);
  aibox::detection::FrameEventPublisher publisher(aggregator, transport);
  publisher.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CHECK_TRUE(SubmitAndWait(aggregator, stats, mu, cv, 0, 20, 0, 5));
  const uint64_t after_first = stats.received;
  CHECK_TRUE(after_first >= 20);
  CHECK_TRUE(publisher.PublishFailures() == 0);

  nats.reset(-1);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  for (int i = 0; i < 8; ++i) {
    aggregator.Submit(MakeFrame(200 + i));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  const uint64_t received_down = stats.received;
  std::cout << "[NFR-2] during_disconnect received=" << received_down
            << " publish_failures=" << publisher.PublishFailures()
            << " (max_pending=0 drops silently)" << std::endl;
  CHECK_TRUE(received_down <= after_first);

  nats.reset(aibox::test::StartIsolatedNats(nats_bin, kPort, "aibox-events-nfr2"));
  CHECK_TRUE(nats.pid > 0);
  CHECK_TRUE(aibox::test::WaitForPort(kPort, 40));
  CHECK_TRUE(WaitAvailable(publisher_nats, 50));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const uint64_t received_before_resume = stats.received;
  CHECK_TRUE(SubmitAndWait(aggregator, stats, mu, cv, 400, 20,
                           received_before_resume, 8));
  CHECK_TRUE(stats.received >= received_before_resume + 20);
  CHECK_TRUE(aibox::test::FrameEventChecksPassed(stats));

  publisher.Stop();
  publisher_nats.Disconnect();
  subscriber.Disconnect();
  nats.reset(-1);
  std::cout << "[NFR-2] first=" << after_first
            << " after_resume received=" << stats.received
            << " published=" << publisher.Published()
            << " publish_failures=" << publisher.PublishFailures() << std::endl;
  return true;
}

bool RunLatestPerCameraRepublish(const std::string& nats_bin) {
  aibox::test::IsolatedNatsGuard nats;
  nats.reset(aibox::test::StartIsolatedNats(nats_bin, kPort,
                                            "aibox-events-republish"));
  CHECK_TRUE(nats.pid > 0);
  if (!aibox::test::WaitForPort(kPort, 40)) return false;

  aibox::adapter::NatsAdapter::Config cfg;
  cfg.nats_url = "nats://127.0.0.1:" + std::to_string(kPort);
  cfg.max_pending = 0;
  cfg.client_name = "aibox-republish-sub";

  aibox::adapter::NatsAdapter subscriber;
  CHECK_TRUE(WaitForConnect(subscriber, cfg, 30));

  std::mutex mu;
  std::condition_variable cv;
  uint64_t received = 0;
  std::map<std::string, int64_t> last_ts;
  std::map<std::string, int64_t> post_reconnect_ts;
  uint64_t reconnect_gate = 0;
  CHECK_TRUE(subscriber.Subscribe(
      aibox::detection::NatsFrameTransport::kSubject,
      [&](const std::string&, const std::string&, const std::string& payload) {
        aibox::FrameEvent frame;
        if (!frame.ParseFromArray(payload.data(),
                                  static_cast<int>(payload.size()))) {
          return;
        }
        {
          std::lock_guard<std::mutex> lock(mu);
          ++received;
          last_ts[frame.camera_id()] = frame.timestamp_ms();
          if (reconnect_gate > 0 && received > reconnect_gate) {
            post_reconnect_ts[frame.camera_id()] = frame.timestamp_ms();
          }
        }
        cv.notify_all();
      }));

  cfg.client_name = "aibox-republish-pub";
  aibox::adapter::NatsAdapter publisher_nats;
  CHECK_TRUE(WaitForConnect(publisher_nats, cfg, 10));
  aibox::detection::NatsFrameTransport transport(publisher_nats);
  aibox::aggregator::ResultAggregator::Options agg_opts;
  agg_opts.per_camera_queue_depth = 256;
  agg_opts.log_every = 0;
  aibox::aggregator::ResultAggregator aggregator(agg_opts);
  aibox::detection::FrameEventPublisher publisher(aggregator, transport);
  publisher.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const int64_t ts_cam1 = 1750000000000LL + 4 * 40;
  const int64_t ts_cam2 = 1750000000000LL + 14 * 40;
  for (int i = 0; i < 5; ++i) CHECK_TRUE(aggregator.Submit(MakeFrame(i, "cam-001")));
  for (int i = 0; i < 5; ++i) CHECK_TRUE(aggregator.Submit(MakeFrame(10 + i, "cam-002")));

  {
    std::unique_lock<std::mutex> lock(mu);
    CHECK_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] {
      return received >= 10 && last_ts["cam-001"] == ts_cam1 &&
             last_ts["cam-002"] == ts_cam2;
    }));
    reconnect_gate = received;
  }

  nats.reset(-1);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  nats.reset(aibox::test::StartIsolatedNats(nats_bin, kPort,
                                            "aibox-events-republish"));
  CHECK_TRUE(nats.pid > 0);
  CHECK_TRUE(aibox::test::WaitForPort(kPort, 40));
  CHECK_TRUE(WaitAvailable(publisher_nats, 50));
  CHECK_TRUE(WaitAvailable(subscriber, 50));
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  {
    std::unique_lock<std::mutex> lock(mu);
    const bool ok = cv.wait_for(lock, std::chrono::seconds(8), [&] {
      return post_reconnect_ts.count("cam-001") &&
             post_reconnect_ts.count("cam-002");
    });
    if (!ok) {
      std::cerr << "republish wait failed received=" << received
                << " republished=" << publisher.Republished()
                << " post_cams=" << post_reconnect_ts.size() << std::endl;
    }
    CHECK_TRUE(ok);
    CHECK_TRUE(post_reconnect_ts["cam-001"] == ts_cam1);
    CHECK_TRUE(post_reconnect_ts["cam-002"] == ts_cam2);
  }
  CHECK_TRUE(publisher.Republished() >= 2);

  publisher.Stop();
  publisher_nats.Disconnect();
  subscriber.Disconnect();
  nats.reset(-1);
  std::cout << "[A+] latest-per-camera republish cam-001/cam-002 OK"
            << " republished=" << publisher.Republished() << std::endl;
  return true;
}

bool RunLifecycle20(const std::string& nats_bin) {
  aibox::test::IsolatedNatsGuard nats;
  nats.reset(aibox::test::StartIsolatedNats(nats_bin, kPort, "aibox-events-nfr3"));
  CHECK_TRUE(nats.pid > 0);
  if (!aibox::test::WaitForPort(kPort, 40)) return false;

  aibox::adapter::NatsAdapter::Config cfg;
  cfg.nats_url = "nats://127.0.0.1:" + std::to_string(kPort);
  cfg.max_pending = 0;
  cfg.client_name = "aibox-nfr3";

  for (int i = 0; i < 20; ++i) {
    aibox::adapter::NatsAdapter nats;
    CHECK_TRUE(WaitForConnect(nats, cfg, 20));
    std::atomic<int> got{0};
    CHECK_TRUE(nats.Subscribe(
        aibox::detection::NatsFrameTransport::kSubject,
        [&](const std::string&, const std::string&, const std::string&) {
          got.fetch_add(1);
        }));
    aibox::FrameEvent frame;
    frame.set_camera_id("cam-001");
    frame.set_timestamp_ms(1750000000000LL + i);
    auto* det = frame.add_detections();
    det->set_event_id("550e8400-e29b-41d4-a716-446655440000");
    det->set_confidence(1.0f);
    det->set_class_id(std::string("p") + '\0' + "x");
    det->set_x_min(1);
    det->set_y_min(1);
    det->set_x_max(2);
    det->set_y_max(2);
    std::string bytes;
    CHECK_TRUE(frame.SerializeToString(&bytes));
    CHECK_TRUE(bytes.find('\0') != std::string::npos);
    CHECK_TRUE(nats.Publish(aibox::detection::NatsFrameTransport::kSubject,
                            std::vector<uint8_t>(bytes.begin(), bytes.end())));
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (got.load() == 0 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_TRUE(got.load() >= 1);
    nats.Disconnect();
    CHECK_TRUE(aibox::adapter::NatsAdapter::CloseLibraryAndWait());
  }

  nats.reset(-1);
  std::cout << "[NFR-3] Connect/Subscribe/Publish/Disconnect/Close x20 OK"
            << std::endl;
  return true;
}

}  // namespace

int main() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  const std::string nats_bin = aibox::test::FindNatsServer();
  if (nats_bin.empty()) {
    std::cerr << "SKIP: nats-server binary not found" << std::endl;
    return kSkipNoNats;
  }

  aibox::test::KillIsolatedPort(kPort);

  bool ok = false;
  try {
    ok = RunReconnect(nats_bin);
    if (ok) ok = RunLatestPerCameraRepublish(nats_bin);
    if (ok) ok = RunLifecycle20(nats_bin);
  } catch (...) {
    ok = false;
  }

  aibox::test::KillIsolatedPort(kPort);

  aibox::adapter::NatsAdapter::CloseLibraryAndWait();
  google::protobuf::ShutdownProtobufLibrary();
  if (!ok) {
    std::cerr << "FAILED aggregator_nats_nfr_tests" << std::endl;
    return 1;
  }
  std::cout << "PASSED aggregator_nats_nfr_tests" << std::endl;
  return 0;
}
