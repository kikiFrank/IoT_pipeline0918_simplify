/// @file opencl_loader.hpp
/// @brief OpenCL API dlopen 加载器 (参考 overlay 插件 open_cl_funcs.h)
/// 扩展: clFinish, clEnqueueReadBuffer, clEnqueueWriteBuffer, clRetainMemObject,
///       clGetDeviceInfo, clCreateCommandQueue (非 WithProperties 版本, 测试用)

#pragma once

#include <memory>
#include <CL/cl.h>
#include <CL/cl_ext_qcom.h>

namespace face_infer {

// --- function pointer types ---
using clBuildProgram_fnp             = decltype(clBuildProgram);
using clCreateBuffer_fnp             = decltype(clCreateBuffer);
using clCreateCommandQueueWithProperties_fnp = decltype(clCreateCommandQueueWithProperties);
using clCreateContext_fnp            = decltype(clCreateContext);
using clCreateKernel_fnp             = decltype(clCreateKernel);
using clCreateProgramWithSource_fnp  = decltype(clCreateProgramWithSource);
using clEnqueueNDRangeKernel_fnp     = decltype(clEnqueueNDRangeKernel);
using clEnqueueReadBuffer_fnp        = decltype(clEnqueueReadBuffer);
using clEnqueueWriteBuffer_fnp       = decltype(clEnqueueWriteBuffer);
using clFinish_fnp                   = decltype(clFinish);
using clFlush_fnp                    = decltype(clFlush);
using clGetDeviceIDs_fnp             = decltype(clGetDeviceIDs);
using clGetDeviceInfo_fnp            = decltype(clGetDeviceInfo);
using clGetPlatformIDs_fnp           = decltype(clGetPlatformIDs);
using clGetProgramBuildInfo_fnp      = decltype(clGetProgramBuildInfo);
using clReleaseCommandQueue_fnp      = decltype(clReleaseCommandQueue);
using clReleaseContext_fnp           = decltype(clReleaseContext);
using clReleaseKernel_fnp            = decltype(clReleaseKernel);
using clReleaseMemObject_fnp         = decltype(clReleaseMemObject);
using clReleaseProgram_fnp           = decltype(clReleaseProgram);
using clRetainMemObject_fnp          = decltype(clRetainMemObject);
using clSetKernelArg_fnp             = decltype(clSetKernelArg);

/// dlopen("libOpenCL.so") 单例加载器
class OpenClLoader {
public:
    /// 获取全局单例 (首次调用 dlopen)
    static std::shared_ptr<OpenClLoader> Get();

    OpenClLoader();
    ~OpenClLoader();

    /// 手动初始化 (非单例场景, 如测试)
    bool Init();

    // --- OpenCL API function pointers ---
    clBuildProgram_fnp*             BuildProgram             = nullptr; // 编译OpenCL内核程序
    clCreateBuffer_fnp*             CreateBuffer             = nullptr; // 创建cl_mem缓冲区(含DMABuf导入)
    clCreateCommandQueueWithProperties_fnp* CreateCommandQueueWithProperties = nullptr;
    clCreateContext_fnp*            CreateContext             = nullptr;
    clCreateKernel_fnp*             CreateKernel             = nullptr;
    clCreateProgramWithSource_fnp*  CreateProgramWithSource  = nullptr;
    clEnqueueNDRangeKernel_fnp*     EnqueueNDRangeKernel     = nullptr;
    clEnqueueReadBuffer_fnp*        EnqueueReadBuffer        = nullptr;
    clEnqueueWriteBuffer_fnp*       EnqueueWriteBuffer       = nullptr;
    clFinish_fnp*                   Finish                   = nullptr;
    clFlush_fnp*                    Flush                    = nullptr;
    clGetDeviceIDs_fnp*             GetDeviceIDs             = nullptr;
    clGetDeviceInfo_fnp*            GetDeviceInfo            = nullptr;
    clGetPlatformIDs_fnp*           GetPlatformIDs           = nullptr;
    clGetProgramBuildInfo_fnp*      GetProgramBuildInfo      = nullptr;
    clReleaseCommandQueue_fnp*      ReleaseCommandQueue      = nullptr;
    clReleaseContext_fnp*           ReleaseContext           = nullptr;
    clReleaseKernel_fnp*            ReleaseKernel            = nullptr;
    clReleaseMemObject_fnp*         ReleaseMemObject         = nullptr;
    clReleaseProgram_fnp*           ReleaseProgram           = nullptr;
    clRetainMemObject_fnp*          RetainMemObject          = nullptr;
    clSetKernelArg_fnp*             SetKernelArg             = nullptr;

    // clCreateCommandQueue 便捷包装 (内部调用 WithProperties, properties=nullptr)
    cl_command_queue CreateCommandQueue(cl_context ctx, cl_device_id dev,
                                        cl_command_queue_properties props,
                                        cl_int* err);

private:
    bool LoadSymbols();
    void* lib_handle_ = nullptr;
};

}  // namespace face_infer
