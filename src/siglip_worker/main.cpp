#include "aibox/siglip_worker/WorkerConfig.hpp"
#include "aibox/siglip_worker/SiglipNatsClient.hpp"
#include "aibox/siglip_worker/SiglipWorker.hpp"
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
std::atomic_bool stop_requested{false};
void SignalHandler(int) { stop_requested.store(true); }
}
int main(int argc, char** argv) {
  std::cout.setf(std::ios::unitbuf);
  std::string config_path;
  bool prepare = false, check = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) config_path = argv[++i];
    else if (arg == "--prepare-consumer") prepare = true;
    else if (arg == "--check-config") check = true;
    else { std::cerr << "usage: aibox_siglip_worker --config FILE [--check-config|--prepare-consumer]\n"; return 2; }
  }
  if (config_path.empty() || (prepare && check)) return 2;
  try {
    const auto config = aibox::siglip::LoadConfig(config_path, !prepare);
    if (check) { std::cout << "SIGLIP_CONFIG_OK\n"; return 0; }
    if (prepare) {
      aibox::siglip::SiglipNatsClient client(config);
      client.PrepareConsumer();
      std::cout << "SIGLIP_CONSUMER_READY\n";
      return 0;
    }
    std::filesystem::create_directories(config.log_dir);
    const auto lock_path = config.log_dir + "/worker.instance.lock";
    const int lock = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0640);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0)
      throw std::runtime_error("another SigLIP Worker owns instance lock");
    struct sigaction action{};
    action.sa_handler = SignalHandler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
    aibox::siglip::SiglipWorker worker(config, stop_requested);
    worker.Run();
    close(lock);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "[SIGLIP_FATAL] " << error.what() << '\n';
    return 1;
  }
}
