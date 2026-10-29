#include "aibox/siglip_worker/ImagePreprocess.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) return 3;
  std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>{in}, {});
  try {
    const auto result = aibox::siglip::PrepareImage(bytes);
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(result.png.data()), result.png.size());
    if (!out) return 4;
    std::cout << result.source_sha256 << ' ' << result.inference_sha256 << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 5;
  }
}
