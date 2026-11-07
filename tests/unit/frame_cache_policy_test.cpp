// Standalone unit test for FrameSnapshotCache. No GStreamer/QNN dependency.
#include "aibox/media/FrameSnapshotCache.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::cerr << "FAIL line " << __LINE__ << ": " << #cond << std::endl;  \
      ++failures;                                                           \
    }                                                                       \
  } while (0)

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

aibox::media::CachedFrame MakeFrame(const std::string& camera_id,
                                    const std::string& frame_id,
                                    int64_t timestamp_ms,
                                    std::size_t bytes = 100) {
  aibox::media::CachedFrame frame;
  frame.camera_id = camera_id;
  frame.frame_id = frame_id;
  frame.timestamp_ms = timestamp_ms;
  frame.width = 10;
  frame.height = 10;
  frame.packed_nv12.assign(bytes, 0x42);
  return frame;
}

aibox::media::FrameCacheConfig BaseConfig() {
  aibox::media::FrameCacheConfig cfg;
  cfg.max_total_bytes = 1u * 1024u * 1024u;
  return cfg;
}

void ZeroIntervalStoresEveryFrame() {
  auto cfg = BaseConfig();
  aibox::media::FrameSnapshotCache cache(cfg);
  const int64_t base = NowMs();

  CHECK(cache.Put(MakeFrame("cam", "f0", base)));
  CHECK(cache.Put(MakeFrame("cam", "f1", base + 50)));
  CHECK(cache.Put(MakeFrame("cam", "f2", base + 100)));
  CHECK(cache.Frames() == 3);
  CHECK(cache.Dropped() == 0);
}

void SamplingSkipsWithinInterval() {
  auto cfg = BaseConfig();
  cfg.min_interval_ms = 150;
  aibox::media::FrameSnapshotCache cache(cfg);
  const int64_t base = NowMs();

  CHECK(cache.Put(MakeFrame("cam", "f0", base)));
  CHECK(cache.Frames() == 1);

  // 100 ms after the stored frame: sampled out, not a capacity drop.
  CHECK(!cache.Put(MakeFrame("cam", "f1", base + 100)));
  CHECK(cache.Frames() == 1);
  CHECK(cache.Dropped() == 0);

  // 200 ms after the last stored frame: stored again.
  CHECK(cache.Put(MakeFrame("cam", "f2", base + 200)));
  CHECK(cache.Frames() == 2);
  CHECK(cache.Dropped() == 0);
}

void ShouldAcceptMatchesPutSampling() {
  auto cfg = BaseConfig();
  cfg.min_interval_ms = 150;
  aibox::media::FrameSnapshotCache cache(cfg);
  const int64_t base = NowMs();

  CHECK(cache.ShouldAccept("cam", "f0", base));
  CHECK(cache.Put(MakeFrame("cam", "f0", base)));
  CHECK(!cache.ShouldAccept("cam", "f1", base + 100));
  CHECK(cache.ShouldAccept("cam", "f0", base + 100));
  CHECK(cache.ShouldAccept("cam", "f1", base + 200));
  CHECK(cache.ShouldAccept("cam", "z", 0));
}

void ZeroTimestampFramesBypassSampling() {
  auto cfg = BaseConfig();
  cfg.min_interval_ms = 150;
  aibox::media::FrameSnapshotCache cache(cfg);

  // A frame with an unknown timestamp is stored (sampling guard needs > 0).
  CHECK(cache.Put(MakeFrame("cam", "z0", 0)));
  CHECK(cache.Put(MakeFrame("cam", "z1", 0)));
  CHECK(cache.Frames() == 2);

  // The last stored timestamp is 0, so the next positive timestamp is stored.
  CHECK(cache.Put(MakeFrame("cam", "f2", NowMs())));
  CHECK(cache.Frames() == 3);
  CHECK(cache.Dropped() == 0);
}

void SameFrameIdReplacementBypassesSampling() {
  auto cfg = BaseConfig();
  cfg.min_interval_ms = 150;
  aibox::media::FrameSnapshotCache cache(cfg);
  const int64_t base = NowMs();

  CHECK(cache.Put(MakeFrame("cam", "f0", base)));
  // Same frame_id inside the interval replaces in place.
  CHECK(cache.Put(MakeFrame("cam", "f0", base + 50)));
  CHECK(cache.Frames() == 1);
  CHECK(cache.Dropped() == 0);
}

void FindExactAndNearest() {
  auto cfg = BaseConfig();
  aibox::media::FrameSnapshotCache cache(cfg);
  const int64_t base = NowMs();

  CHECK(cache.Put(MakeFrame("cam", "f0", base)));
  CHECK(cache.Put(MakeFrame("cam", "f1", base + 100)));
  CHECK(cache.Put(MakeFrame("cam", "f2", base + 200)));

  aibox::media::CachedFrame out;
  CHECK(cache.Find("cam", "f1", 0, 500, out));
  CHECK(out.frame_id == "f1");

  // No exact id: nearest timestamp within max_delta.
  CHECK(cache.Find("cam", "missing", base + 160, 500, out));
  CHECK(out.frame_id == "f2");

  // Outside max_delta: no frame.
  CHECK(!cache.Find("cam", "missing", base + 1000, 500, out));
}

void CapacityDropIsCounted() {
  auto cfg = BaseConfig();
  cfg.max_total_bytes = 250;
  aibox::media::FrameSnapshotCache cache(cfg);
  const int64_t base = NowMs();

  CHECK(cache.Put(MakeFrame("cam", "f0", base, 100)));
  CHECK(cache.Put(MakeFrame("cam", "f1", base + 50, 100)));
  CHECK(cache.Put(MakeFrame("cam", "f2", base + 100, 100)));
  CHECK(cache.Frames() == 2);
  CHECK(cache.Dropped() == 1);
}

void ReplacementRespectsGlobalBudget() {
  auto cfg = BaseConfig();
  cfg.max_total_bytes = 250;
  aibox::media::FrameSnapshotCache cache(cfg);
  const int64_t base = NowMs();
  CHECK(cache.Put(MakeFrame("cam-a", "old", base, 100)));
  CHECK(cache.Put(MakeFrame("cam-b", "same", base + 10, 100)));
  CHECK(cache.Put(MakeFrame("cam-b", "same", base + 10, 200)));
  CHECK(cache.Bytes() == 200);
  CHECK(cache.Frames() == 1);
  aibox::media::CachedFrame out;
  CHECK(cache.Find("cam-b", "same", 0, 500, out));
  CHECK(out.packed_nv12.size() == 200);
}

}  // namespace

int main() {
  ZeroIntervalStoresEveryFrame();
  SamplingSkipsWithinInterval();
  ShouldAcceptMatchesPutSampling();
  ZeroTimestampFramesBypassSampling();
  SameFrameIdReplacementBypassesSampling();
  FindExactAndNearest();
  CapacityDropIsCounted();
  ReplacementRespectsGlobalBudget();

  if (failures != 0) {
    std::cerr << failures << " frame-cache check(s) failed" << std::endl;
    return 1;
  }
  std::cout << "FRAME_CACHE_POLICY_TEST=PASS" << std::endl;
  return 0;
}
