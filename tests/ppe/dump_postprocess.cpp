#include "../../plugins/mlpostprocess_roi/modules/object-detection/ml-postprocess-yolox-ppe.cc"
#include <fstream>
#include <iomanip>

int main(int argc,char** argv){
 if(argc!=4)return 2;
 std::vector<float> values(8400*8);
 std::ifstream input(argv[2],std::ios::binary);
 input.read(reinterpret_cast<char*>(values.data()),values.size()*sizeof(float));
 if(input.gcount()!=static_cast<std::streamsize>(values.size()*sizeof(float)))return 3;
 Module module([](uint32_t,const char*){});
 if(!module.Configure(argv[1],R"({"confidence":0.1})"))return 4;
 Tensor tensor;tensor.name="output";tensor.type=kFloat32;
 tensor.dimensions={1,8400,8};tensor.data=values.data();
 Dictionary params;params["input-tensor-region"]=Region(0,0,640,640);
 std::any output=ObjectDetections{};
 if(!module.Process({tensor},params,output))return 5;
 std::ofstream csv(argv[3]);csv<<std::setprecision(9);
 for(const auto& d:std::any_cast<const ObjectDetections&>(output))
  csv<<d.name<<','<<d.confidence<<','<<d.left<<','<<d.top<<','<<d.right<<','<<d.bottom<<'\n';
 return csv?0:6;
}
