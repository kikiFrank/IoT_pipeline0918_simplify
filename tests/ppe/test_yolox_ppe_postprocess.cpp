// Standalone module test: compile this translation unit only (header helpers
// in the vendor module interface are not designed for separate linkage).
#include "../../plugins/mlpostprocess_roi/modules/object-detection/ml-postprocess-yolox-ppe.cc"
#include <iostream>
#include <stdexcept>

static void Check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) return 2;
#ifndef AIBOX_YOLOX_STACKLIGHT
  Module module([](uint32_t, const char*) {});
  Check(module.Caps().find("[1, 8400, 8]") != std::string::npos &&
        module.Caps().find("[1, 8400, 6]") == std::string::npos, "PPE-only caps");
  if (argc == 3) {
    Module wrong_labels([](uint32_t, const char*) {});
    Check(!wrong_labels.Configure(argv[2], R"({"confidence":10})"), "PPE rejects stacklight labels");
  }
  Check(module.Configure(argv[1], R"({"confidence":10})"), "configure");
  std::vector<float> values(8400 * 8, 0.0f);
  {
    Tensor empty; empty.name="output"; empty.type=kFloat32;
    empty.dimensions={1,8400,8}; empty.data=values.data();
    Dictionary params; params["input-tensor-region"]=Region(0,0,640,640);
    std::any result=ObjectDetections{};
    Check(module.Process({empty},params,result) &&
          std::any_cast<const ObjectDetections&>(result).empty(), "empty detections");
  }
  auto row = [&](int index, float objectness, int cls) {
    auto* p = values.data() + index * 8;
    p[0]=100; p[1]=100; p[2]=40; p[3]=60; p[4]=objectness;
    p[5]=0; p[6]=0; p[7]=0; p[5+cls]=0.8f;
  };
  row(0, 0.9f, 0); row(1, 0.8f, 0); row(2, 0.7f, 2);
  row(3, 0.01f, 1);
  row(4, 0.95f, 1); values[4*8]=std::numeric_limits<float>::quiet_NaN();
  Tensor tensor; tensor.type=kFloat32; tensor.name="output";
  tensor.dimensions={1,8400,8}; tensor.data=values.data();
  // Float values must not be dequantized even with non-identity metadata.
  tensor.qscale=0.01f; tensor.qoffset=-2577;
  Dictionary params; params["input-tensor-region"]=Region(0,0,640,640);
  std::any output=ObjectDetections{};
  Check(module.Process({tensor}, params, output), "float process");
  auto detections=std::any_cast<ObjectDetections>(output);
  Check(detections.size()==2, "class-wise NMS, score floor and NaN rejection");
  Check(detections[0].name=="Human" && detections[1].name=="white hat", "labels");
  Check(std::abs(detections[0].confidence-72.0f)<0.001f, "objectness product");
  Check(std::abs(detections[0].left-0.125f)<0.0001f, "region transform");
  tensor.dimensions={1,8,8400};
  Check(!module.Process({tensor},params,output), "reject transpose");
  tensor.dimensions={1,8400,8}; tensor.name="wrong";
  Check(!module.Process({tensor},params,output), "reject name");
  tensor.name="output"; tensor.type=kUint16;
  tensor.qscale=0.01f; tensor.qoffset=-2577.0f;
  values[4*8]=100;
  std::vector<uint16_t> quantized(values.size());
  for (size_t i=0;i<values.size();++i)
    quantized[i]=static_cast<uint16_t>(std::round(values[i]/tensor.qscale-tensor.qoffset));
  tensor.data=quantized.data(); output=ObjectDetections{};
  Check(module.Process({tensor},params,output), "uint16 process");
  detections=std::any_cast<ObjectDetections>(output);
  Check(detections.size()==3, "quantized NMS");
  quantized[4]=0; output=ObjectDetections{};
  Check(module.Process({tensor},params,output), "negative offset no unsigned wrap");
  tensor.qscale=0;
  Check(!module.Process({tensor},params,output), "reject invalid quantization");
  std::cout << "PPE_POSTPROCESS_TEST_OK\n";
#else
  if (argc != 3) return 2;
  Dictionary params; params["input-tensor-region"]=Region(0,0,640,640);
  if (argc == 3) {
    Module light([](uint32_t, const char*) {});
    Check(light.Caps().find("[1, 8400, 6]") != std::string::npos &&
          light.Caps().find("[1, 8400, 8]") == std::string::npos, "Stacklight-only caps");
    Module wrong_labels([](uint32_t, const char*) {});
    Check(!wrong_labels.Configure(argv[1], R"({"confidence":25})"), "stacklight rejects PPE labels");
    Check(light.Configure(argv[2], R"({"confidence":25})"), "light configure");
    std::vector<float> rows(8400 * 6, 0.0f);
    Tensor lt; lt.name="output"; lt.type=kFloat32; lt.dimensions={1,8400,6}; lt.data=rows.data();
    std::any result=ObjectDetections{};
    Check(light.Process({lt},params,result) && std::any_cast<const ObjectDetections&>(result).empty(), "light zero ROI");
    for (int i=0; i<3; ++i) {
      float* p=rows.data()+i*6;
      p[0]=(i==2 ? 400 : 100); p[1]=100; p[2]=40; p[3]=60; p[4]=0.9f; p[5]=0.8f;
    }
    result=ObjectDetections{};
    Check(light.Process({lt},params,result), "light float");
    const auto& boxes=std::any_cast<const ObjectDetections&>(result);
    Check(boxes.size()==2 && boxes.front().name=="Stacklight", "light NMS and single class label");
    Check(std::abs(boxes.front().confidence-72.0f)<0.001f, "light product score");
    lt.dimensions={1,8400,8};
    Check(!light.Process({lt},params,result), "light rejects PPE shape");
    lt.dimensions={1,8400,6}; lt.type=kUint16; lt.qscale=0.033320073038339615f; lt.qoffset=-12;
    std::vector<uint16_t> qrows(rows.size());
    for (size_t i=0;i<rows.size();++i) qrows[i]=static_cast<uint16_t>(std::round(rows[i]/lt.qscale-lt.qoffset));
    lt.data=qrows.data(); result=ObjectDetections{};
    Check(light.Process({lt},params,result) && std::any_cast<const ObjectDetections&>(result).size()==2, "light actual quantization");
    std::cout << "LIGHT_POSTPROCESS_TEST_OK\n";
  }
#endif
}
