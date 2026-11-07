#include "aibox/siglip_worker/SiglipPipelineBuilder.hpp"
#include "aibox/siglip_worker/ImagePreprocess.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  try {
    const auto config = aibox::siglip::LoadConfig(argv[1]);
    std::ifstream file(argv[2], std::ios::binary);
    const std::vector<std::uint8_t> jpeg(std::istreambuf_iterator<char>{file}, {});
    const auto image = aibox::siglip::PrepareImage(jpeg);
    aibox::siglip::SiglipPipelineBuilder pipeline(config);
    pipeline.WaitReady();
    std::vector<double> latency;
    for (int i = 0; i < 60; ++i) {
      const auto begin = std::chrono::steady_clock::now();
      pipeline.Infer(image.png, config.infer_timeout_ms, {});
      latency.push_back(std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - begin).count());
      std::this_thread::sleep_until(begin + std::chrono::milliseconds(500));
    }
    std::sort(latency.begin(), latency.end());
    std::cout << "BENCHMARK_OK device=" << config.htp_device_id << " count=" << latency.size()
              << " p50_ms=" << latency[30] << " p95_ms=" << latency[57]
              << " max_ms=" << latency.back() << '\n';
    pipeline.Stop();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
