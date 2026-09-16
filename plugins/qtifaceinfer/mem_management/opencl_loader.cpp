/// @file opencl_loader.cpp
/// @brief OpenCL API dlopen 加载器实现 (参考 overlay 插件 open_cl_funcs.cc)

#include "opencl_loader.hpp"

#include <cstdio>
#include <dlfcn.h>

namespace face_infer {

// --- 单例 ---
std::shared_ptr<OpenClLoader> OpenClLoader::Get() {
    static std::shared_ptr<OpenClLoader> instance;
    if (!instance) {
        instance = std::make_shared<OpenClLoader>();
        if (!instance->Init()) {
            instance.reset();
        }
    }
    return instance;
}

OpenClLoader::OpenClLoader() = default;

OpenClLoader::~OpenClLoader() {
    if (lib_handle_) {
        dlclose(lib_handle_);
        lib_handle_ = nullptr;
    }
}

bool OpenClLoader::Init() {
    if (lib_handle_) return true;  // 已加载

    dlerror();  // 清除旧错误
    lib_handle_ = dlopen("libOpenCL.so", RTLD_LAZY);
    if (!lib_handle_) {
        fprintf(stderr, "[OpenClLoader] dlopen libOpenCL.so failed: %s\n", dlerror());
        return false;
    }

    return LoadSymbols();
}

bool OpenClLoader::LoadSymbols() {
#define LOAD_SYM(member, name) \
    member = reinterpret_cast<decltype(member)>(dlsym(lib_handle_, #name)); \
    if (!member) { \
        fprintf(stderr, "[OpenClLoader] dlsym " #name " failed\n"); \
    }

    LOAD_SYM(BuildProgram,             clBuildProgram)
    LOAD_SYM(CreateBuffer,             clCreateBuffer)
    LOAD_SYM(CreateCommandQueueWithProperties, clCreateCommandQueueWithProperties)
    LOAD_SYM(CreateContext,            clCreateContext)
    LOAD_SYM(CreateKernel,             clCreateKernel)
    LOAD_SYM(CreateProgramWithSource,  clCreateProgramWithSource)
    LOAD_SYM(EnqueueNDRangeKernel,     clEnqueueNDRangeKernel)
    LOAD_SYM(EnqueueReadBuffer,        clEnqueueReadBuffer)
    LOAD_SYM(EnqueueWriteBuffer,       clEnqueueWriteBuffer)
    LOAD_SYM(Finish,                   clFinish)
    LOAD_SYM(Flush,                    clFlush)
    LOAD_SYM(GetDeviceIDs,             clGetDeviceIDs)
    LOAD_SYM(GetDeviceInfo,            clGetDeviceInfo)
    LOAD_SYM(GetPlatformIDs,           clGetPlatformIDs)
    LOAD_SYM(GetProgramBuildInfo,      clGetProgramBuildInfo)
    LOAD_SYM(ReleaseCommandQueue,      clReleaseCommandQueue)
    LOAD_SYM(ReleaseContext,           clReleaseContext)
    LOAD_SYM(ReleaseKernel,            clReleaseKernel)
    LOAD_SYM(ReleaseMemObject,         clReleaseMemObject)
    LOAD_SYM(ReleaseProgram,           clReleaseProgram)
    LOAD_SYM(RetainMemObject,          clRetainMemObject)
    LOAD_SYM(SetKernelArg,             clSetKernelArg)

#undef LOAD_SYM

    // 关键 API 必须存在
    if (!CreateBuffer || !ReleaseMemObject || !Finish || !GetPlatformIDs || !GetDeviceIDs) {
        fprintf(stderr, "[OpenClLoader] critical OpenCL symbols missing\n");
        return false;
    }

    return true;
}

// 将旧式props参数转为cl_queue_properties[]数组
cl_command_queue OpenClLoader::CreateCommandQueue(cl_context ctx, cl_device_id dev,
                                                   cl_command_queue_properties props,
                                                   cl_int* err) {
    if (!CreateCommandQueueWithProperties) {
        if (err) *err = CL_INVALID_VALUE;
        return nullptr;
    }
    // clCreateCommandQueueWithProperties 接受 cl_queue_properties[] (0-terminated)
    cl_queue_properties qprops[] = { 0 };
    if (props != 0) {
        cl_queue_properties qprops_with_flags[] = {
            CL_QUEUE_PROPERTIES, static_cast<cl_queue_properties>(props), 0
        };
        return CreateCommandQueueWithProperties(ctx, dev, qprops_with_flags, err);
    }
    return CreateCommandQueueWithProperties(ctx, dev, qprops, err);
}

}  // namespace face_infer
