#include <System/QnnSystemInterface.h>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <iostream>
#include <iomanip>
#include <vector>
#include <stdexcept>

int main(int argc,char** argv){
 if(argc!=2)return 2;
 std::ifstream file(argv[1],std::ios::binary);
 std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
 void* lib=dlopen("/usr/lib/libQnnSystem.so",RTLD_NOW|RTLD_LOCAL);
 if(!lib||bytes.empty())return 3;
 auto providers=reinterpret_cast<decltype(&QnnSystemInterface_getProviders)>(dlsym(lib,"QnnSystemInterface_getProviders"));
 const QnnSystemInterface_t** list=nullptr;uint32_t count=0;
 if(!providers||providers(&list,&count)!=QNN_SUCCESS||!count)return 4;
 auto api=list[0]->QNN_SYSTEM_INTERFACE_VER_NAME;
 QnnSystemContext_Handle_t context=nullptr;
 if(api.systemContextCreate(&context)!=QNN_SUCCESS)return 5;
 const QnnSystemContext_BinaryInfo_t* info=nullptr;Qnn_ContextBinarySize_t size=0;
 if(api.systemContextGetBinaryInfo(context,bytes.data(),bytes.size(),&info,&size)!=QNN_SUCCESS)return 6;
 QnnSystemContext_GraphInfo_t* graphs=nullptr;
 if(info->version==QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1)graphs=info->contextBinaryInfoV1.graphs;
 if(info->version==QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2)graphs=info->contextBinaryInfoV2.graphs;
 if(info->version==QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3)graphs=info->contextBinaryInfoV3.graphs;
 if(!graphs)return 7;
 auto print=[](const char* direction,const auto& tensor){
   const auto q=tensor.quantizeParams.scaleOffsetEncoding;
   std::cout<<direction<<' '<<tensor.name<<" dtype="<<tensor.dataType<<" shape=";
   for(uint32_t i=0;i<tensor.rank;++i)std::cout<<tensor.dimensions[i]<<',';
   std::cout<<std::setprecision(17)<<" scale="<<q.scale<<" offset="<<q.offset<<'\n';
 };
 auto graph=[&](const auto& g){
   for(uint32_t i=0;i<g.numGraphInputs;++i){const auto& t=g.graphInputs[i];if(t.version==QNN_TENSOR_VERSION_1)print("input",t.v1);else if(t.version==QNN_TENSOR_VERSION_2)print("input",t.v2);}
   for(uint32_t i=0;i<g.numGraphOutputs;++i){const auto& t=g.graphOutputs[i];if(t.version==QNN_TENSOR_VERSION_1)print("output",t.v1);else if(t.version==QNN_TENSOR_VERSION_2)print("output",t.v2);}
 };
 if(graphs[0].version==QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1)graph(graphs[0].graphInfoV1);
 else if(graphs[0].version==QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2)graph(graphs[0].graphInfoV2);
 else if(graphs[0].version==QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_3)graph(graphs[0].graphInfoV3);
 api.systemContextFree(context);dlclose(lib);
}
