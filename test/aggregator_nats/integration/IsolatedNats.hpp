#pragma once

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace aibox::test {

inline std::string FindNatsServer() {
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

inline bool PortOpen(int port) {
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

inline bool WaitForPort(int port, int attempts) {
  for (int i = 0; i < attempts; ++i) {
    if (PortOpen(port)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

inline pid_t StartIsolatedNats(const std::string& bin, int port,
                               const char* name = "aibox-events-itest") {
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
          "-n", name, static_cast<char*>(nullptr));
    _exit(127);
  }
  return pid;
}

inline void StopIsolatedNats(pid_t pid) {
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

inline void KillIsolatedPort(int port) {
  const std::string cmd =
      "fuser -k " + std::to_string(port) + "/tcp >/dev/null 2>&1 || true";
  (void)std::system(cmd.c_str());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

struct IsolatedNatsGuard {
  pid_t pid = -1;
  IsolatedNatsGuard() = default;
  IsolatedNatsGuard(const IsolatedNatsGuard&) = delete;
  IsolatedNatsGuard& operator=(const IsolatedNatsGuard&) = delete;
  ~IsolatedNatsGuard() { reset(-1); }
  void reset(pid_t next) {
    if (pid > 0) StopIsolatedNats(pid);
    pid = next;
  }
};

}  // namespace aibox::test
