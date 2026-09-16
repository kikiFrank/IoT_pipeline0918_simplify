#include <arpa/inet.h>
#include <cstdio>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "aibox/adapter/NatsAdapter.hpp"
#include "aibox/aggregator/ResultAggregator.hpp"
#include "aibox/detection/FrameEventPublisher.hpp"
#include "aibox/detection/NatsFrameTransport.hpp"
#include "FrameEventChecks.hpp"

namespace {

constexpr int kSkipNoNats = 77;
constexpr int kPort = 14223;
constexpr int kFrameCount = 100;
constexpr uint32_t kFrameWidth = 1920;
constexpr uint32_t kFrameHeight = 1080;
constexpr int64_t kBaseTs = 1750000000000LL;

#define CHECK_TRUE(expr)                                                     \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::cerr << "CHECK failed: " #expr << " line=" << __LINE__ << '\n'; \
      return false;                                                          \
    }                                                                        \
  } while (false)

std::string FindNatsServer() {
  const char* env = std::getenv("NATS_SERVER_BIN");
  if (env && env[0] != '\0' && access(env, X_OK) == 0) return env;
  const char* candidates[] = {
      "/usr/sbin/nats-server",
      "/usr/bin/nats-server",
      "/usr/local/bin/nats-server",
  };
  for (const char* path : candidates) {
    if (access(path, X_OK) == 0) return path;
  }
  FILE* pipe = popen("command -v nats-server", "r");
  if (!pipe) return {};
  char buf[256] = {};
  const char* got = fgets(buf, sizeof(buf), pipe);
  pclose(pipe);
  if (!got) return {};
  std::string path(buf);
  while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) {
    path.pop_back();
  }
  if (!path.empty() && access(path.c_str(), X_OK) == 0) return path;
  return {};
}

bool PortOpen(int port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  const int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  close(fd);
  return rc == 0;
}

bool WaitForPort(int port, int attempts) {
  for (int i = 0; i < attempts; ++i) {
    if (PortOpen(port)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

pid_t StartIsolatedNats(const std::string& bin, int port) {
  const pid_t pid = fork();
  if (pid == 0) {
    const int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
      close(null_fd);
    }
    const std::string port_text = std::to_string(port);
    execl(bin.c_str(), bin.c_str(), "-a", "127.0.0.1", "-p", port_text.c_str(),
          "-n", "aibox-events-itest", static_cast<char*>(nullptr));
    _exit(127);
  }
  return pid;
}

void StopIsolatedNats(pid_t pid) {
  if (pid <= 0) return;
  kill(pid, SIGTERM);
  int status = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
}

aibox::aggregator::AggregatedResult MakeFrame(int index) {
  aibox::aggregator::AggregatedResult result;
  result.camera_id = (index % 10 == 3) ? "cam-002" : "cam-001";
  result.frame_id = static_cast<uint64_t>(index);
  result.capture_timestamp_ms =
      static_cast<uint64_t>(kBaseTs + index * 33 + (result.camera_id == "cam-002" ? 7 : 0));
  result.frame_width = kFrameWidth;
  result.frame_height = kFrameHeight;

  if (index % 10 == 0) return result;

  aibox::aggregator::Detection person;
  person.label = "person";
  person.confidence = (index == 1) ? 100.0f : 95.0f;
  person.x = 120;
  person.y = 80;
  person.w = 160;
  person.h = 340;
  result.detections.push_back(person);

  if (index % 7 == 0) {
    aibox::aggregator::Detection helmet;
    helmet.label = "helmet";
    helmet.confidence = 72.0f;
    helmet.x = 1900;
    helmet.y = 1070;
    helmet.w = 100;
    helmet.h = 100;
    result.detections.push_back(helmet);
  }
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

bool RunLoopback() {
  aibox::adapter::NatsAdapter::Config sub_cfg;
  sub_cfg.nats_url = "nats://127.0.0.1:" + std::to_string(kPort);
  sub_cfg.client_name = "aibox-events-itest-sub";
  sub_cfg.max_pending = 0;

  aibox::adapter::NatsAdapter::Config pub_cfg = sub_cfg;
  pub_cfg.client_name = "aibox-events-itest-pub";

  aibox::adapter::NatsAdapter subscriber;
  CHECK_TRUE(WaitForConnect(subscriber, sub_cfg, 30));

  std::mutex mu;
  std::condition_variable cv;
  aibox::test::FrameEventCheckStats stats;
  CHECK_TRUE(subscriber.Subscribe(
      aibox::detection::NatsFrameTransport::kSubject,
      [&](const std::string& subject, const std::string&,
          const std::string& payload) {
        if (subject != aibox::detection::NatsFrameTransport::kSubject) {
          std::lock_guard<std::mutex> lock(mu);
          ++stats.decode_failures;
          return;
        }
        {
          std::lock_guard<std::mutex> lock(mu);
          aibox::test::CheckFrameEventPayload(payload, stats, kFrameWidth,
                                              kFrameHeight);
        }
        cv.notify_all();
      }));

  aibox::adapter::NatsAdapter publisher_nats;
  CHECK_TRUE(WaitForConnect(publisher_nats, pub_cfg, 10));
  aibox::detection::NatsFrameTransport transport(publisher_nats);
  aibox::aggregator::ResultAggregator::Options agg_opts;
  agg_opts.per_camera_queue_depth = 256;
  agg_opts.log_each = false;
  agg_opts.log_every = 0;
  aibox::aggregator::ResultAggregator aggregator(agg_opts);
  aibox::detection::FrameEventPublisher publisher(aggregator, transport);
  publisher.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  int expected_empty = 0;
  int expected_detections = 0;
  for (int i = 0; i < kFrameCount; ++i) {
    auto frame = MakeFrame(i);
    if (frame.detections.empty()) {
      ++expected_empty;
    } else {
      expected_detections += static_cast<int>(frame.detections.size());
    }
    CHECK_TRUE(aggregator.Submit(std::move(frame)));
  }

  {
    std::unique_lock<std::mutex> lock(mu);
    const bool got_all = cv.wait_for(lock, std::chrono::seconds(10), [&] {
      return stats.received >= static_cast<uint64_t>(kFrameCount);
    });
    if (!got_all) {
      std::cerr << "CHECK failed: got_all received=" << stats.received
                << " published=" << publisher.Published()
                << " publish_failures=" << publisher.PublishFailures()
                << " build_failures=" << publisher.BuildFailures()
                << " dropped=" << aggregator.TotalDropped() << std::endl;
      return false;
    }
  }

  publisher.Stop();
  publisher_nats.Disconnect();
  subscriber.Disconnect();

  aibox::test::PrintFrameEventCheckStats(stats);
  std::cout << "[INFO] publisher published=" << publisher.Published()
            << " build_failures=" << publisher.BuildFailures()
            << " publish_failures=" << publisher.PublishFailures() << std::endl;

  CHECK_TRUE(aggregator.TotalDropped() == 0);
  CHECK_TRUE(publisher.Published() == static_cast<uint64_t>(kFrameCount));
  CHECK_TRUE(publisher.BuildFailures() == 0);
  CHECK_TRUE(publisher.PublishFailures() == 0);
  CHECK_TRUE(aggregator.Stats("cam-001").queue_depth == 0);
  CHECK_TRUE(aggregator.Stats("cam-002").queue_depth == 0);
  CHECK_TRUE(stats.received == static_cast<uint64_t>(kFrameCount));
  CHECK_TRUE(stats.decode_ok == static_cast<uint64_t>(kFrameCount));
  CHECK_TRUE(stats.empty_frames == static_cast<uint64_t>(expected_empty));
  CHECK_TRUE(stats.detections == static_cast<uint64_t>(expected_detections));
  CHECK_TRUE(stats.payloads_with_nul > 0);
  CHECK_TRUE(stats.last_timestamp_ms.count("cam-001") == 1);
  CHECK_TRUE(stats.last_timestamp_ms.count("cam-002") == 1);
  CHECK_TRUE(aibox::test::FrameEventChecksPassed(stats));
  return true;
}

}  // namespace

int main() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  const std::string nats_bin = FindNatsServer();
  if (nats_bin.empty()) {
    std::cerr << "SKIP: nats-server binary not found (set NATS_SERVER_BIN)"
              << std::endl;
    return kSkipNoNats;
  }

  const pid_t nats_pid = StartIsolatedNats(nats_bin, kPort);
  if (nats_pid <= 0) {
    std::cerr << "SKIP: failed to start isolated nats-server on " << kPort
              << std::endl;
    return kSkipNoNats;
  }
  if (!WaitForPort(kPort, 40) || kill(nats_pid, 0) != 0) {
    std::cerr << "SKIP: isolated nats-server did not listen on " << kPort
              << " bin=" << nats_bin << std::endl;
    StopIsolatedNats(nats_pid);
    return kSkipNoNats;
  }

  bool ok = false;
  try {
    ok = RunLoopback();
  } catch (...) {
    ok = false;
  }

  StopIsolatedNats(nats_pid);
  aibox::adapter::NatsAdapter::CloseLibraryAndWait();
  google::protobuf::ShutdownProtobufLibrary();

  if (!ok) {
    std::cerr << "FAILED aggregator_nats_integration_tests" << std::endl;
    return 1;
  }
  std::cout << "PASSED aggregator_nats_integration_tests" << std::endl;
  return 0;
}
