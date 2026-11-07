#include "aibox/config/TxtSnapshotLoader.hpp"
#include <iostream>
#include <cmath>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  aibox::pipeline::TxtSnapshotLoader loader;
  auto loaded=loader.Load(argv[1]);
  if (!loaded.status.ok) return 1;
  const auto& models=loaded.value.models;
  if (models.size()!=1) return 1;
  const auto& output=models.front().output;
  if (output.shape!=std::vector<int>{1,8400,8} || output.tensors.size()!=1 ||
      output.tensors.front().shape!=output.shape || output.dtype!=output.tensors.front().dtype ||
      output.class_count!=3 || output.name!="output") return 1;
  if (std::abs(models.front().postprocess.confidence_thresh-0.05f)>1e-6f ||
      std::abs(models.front().postprocess.nms_iou_thresh-0.45f)>1e-6f) return 1;
  std::cout << "PPE_SNAPSHOT_TEST_OK\n";
}
