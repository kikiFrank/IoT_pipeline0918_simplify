/// @file QnnInferencer.cpp
/// @brief 纯 C++ QNN SDK 推理封装实现
///
/// 参考: gst-plugin-faceqnn/ml-faceqnn-engine.cc (行号标注见注释)
/// 关键差异: 无 GStreamer 依赖, 无 GST_ML_MAX_TENSORS 限制, 直接支持全部输出 tensor

#include "QnnInferencer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <vector>
#include <string>

#include <dlfcn.h>

#include <QnnInterface.h>
#include <QnnTypes.h>
#include <QnnGraph.h>
#include <QnnContext.h>
#include <QnnBackend.h>
#include <QnnDevice.h>
#include <QnnLog.h>
#include <QnnProfile.h>
#include <QnnMem.h>
#include <System/QnnSystemInterface.h>
#include <System/QnnSystemContext.h>

// ============================================================
// QNN Tensor V1/V2 兼容宏 (ml-faceqnn-engine.cc:37-54)
// ============================================================
#if defined(QNN_TENSOR_V2_INIT)
  #define QNN_GET_TENSOR(tensor) ((tensor)->v2)
  #define QNN_TENSOR_VERSION_SUPPORTED(tensor) \
      (((tensor)->version == QNN_TENSOR_VERSION_1) || \
       ((tensor)->version == QNN_TENSOR_VERSION_2))
#elif defined(QNN_TENSOR_V1_INIT)
  #define QNN_GET_TENSOR(tensor) ((tensor)->v1)
  #define QNN_TENSOR_VERSION_SUPPORTED(tensor) \
      ((tensor)->version == QNN_TENSOR_VERSION_1)
#else
  #error "Not supported QNN tensor version!"
#endif

// ============================================================
// System context graph info 版本宏 (ml-faceqnn-engine.cc:56-84)
// ============================================================
#if defined(QNN_SYSTEM_CONTEXT_GRAPH_INFO_V3_INIT)
  #define QNN_GET_SYSTEM_CONTEXT_GRAPH_INFO(graphInfo) ((graphInfo)->graphInfoV3)
  #define QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_SUPPORTED(graphInfo) \
      (((graphInfo)->version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1) || \
       ((graphInfo)->version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2) || \
       ((graphInfo)->version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_3))
#elif defined(QNN_SYSTEM_CONTEXT_GRAPH_INFO_V2_INIT)
  #define QNN_GET_SYSTEM_CONTEXT_GRAPH_INFO(graphInfo) ((graphInfo)->graphInfoV2)
  #define QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_SUPPORTED(graphInfo) \
      (((graphInfo)->version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1) || \
       ((graphInfo)->version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2))
#elif defined(QNN_SYSTEM_CONTEXT_GRAPH_INFO_V1_INIT)
  #define QNN_GET_SYSTEM_CONTEXT_GRAPH_INFO(graphInfo) ((graphInfo)->graphInfoV1)
  #define QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_SUPPORTED(graphInfo) \
      ((graphInfo)->version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1)
#else
  #error "Not supported QNN system context graph info version!"
#endif

// ============================================================
// System context binary info 版本宏 (ml-faceqnn-engine.cc:86-114)
// ============================================================
#if defined(QNN_SYSTEM_CONTEXT_BINARY_INFO_V3_INIT)
  #define QNN_GET_SYSTEM_CONTEXT_BINARY_INFO(bi) ((bi)->contextBinaryInfoV3)
  #define QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_SUPPORTED(bi) \
      (((bi)->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1) || \
       ((bi)->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2) || \
       ((bi)->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3))
#elif defined(QNN_SYSTEM_CONTEXT_BINARY_INFO_V2_INIT)
  #define QNN_GET_SYSTEM_CONTEXT_BINARY_INFO(bi) ((bi)->contextBinaryInfoV2)
  #define QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_SUPPORTED(bi) \
      (((bi)->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1) || \
       ((bi)->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2))
#elif defined(QNN_SYSTEM_CONTEXT_BINARY_INFO_V1_INIT)
  #define QNN_GET_SYSTEM_CONTEXT_BINARY_INFO(bi) ((bi)->contextBinaryInfoV1)
  #define QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_SUPPORTED(bi) \
      ((bi)->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1)
#else
  #error "Not supported QNN system context binary info version!"
#endif

// ============================================================
// 便捷访问宏 (ml-faceqnn-engine.cc:116-132)
// ============================================================
#define QNN_TENSOR_DATA_TYPE(tensor)       (QNN_GET_TENSOR(tensor).dataType)
#define QNN_TENSOR_DIMENSION(tensor, idx)  (QNN_GET_TENSOR(tensor).dimensions[(idx)])
#define QNN_TENSOR_NAME(tensor)            (QNN_GET_TENSOR(tensor).name)
#define QNN_TENSOR_RANK(tensor)            (QNN_GET_TENSOR(tensor).rank)
#define QNN_TENSOR_CLIENTBUF(tensor)       (QNN_GET_TENSOR(tensor).clientBuf)
#define QNN_TENSOR_QUANTIZE_PARAMS(tensor) (QNN_GET_TENSOR(tensor).quantizeParams)

// ============================================================
// GraphInfo_t 工作区结构 (ml-faceqnn-engine.cc:134-142)
// QNN SDK 未导出此结构, 需手动定义
// ============================================================
typedef struct {
    Qnn_GraphHandle_t graph;
    const char*       graphName;
    Qnn_Tensor_t*     inputTensors;
    uint32_t          numInputTensors;
    Qnn_Tensor_t*     outputTensors;
    uint32_t          numOutputTensors;
} GraphInfo_t;

// ============================================================
// dlsym function pointer types
// ============================================================
using QnnInterfaceGetProvidersFn      = decltype(QnnInterface_getProviders);
using QnnSystemInterfaceGetProvidersFn = decltype(QnnSystemInterface_getProviders);

// ============================================================
// Log 前缀
// ============================================================
#define LOG_TAG "[QnnInferencer] "

namespace face_infer {

// ============================================================
// 文件作用域辅助 (不依赖 Impl)
// ============================================================

// QNN的日志回调。只打印WARN及更高优先级日志到stderr
static void qnn_log_callback(const char* format, QnnLog_Level_t level,
                              uint64_t /*timestamp*/, va_list args) {
    if (level > QNN_LOG_LEVEL_WARN) return;
    fprintf(stderr, LOG_TAG "[QNN] ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
}

/**
* 加载符号 
* @param method 符号指针
* @param handle 库句柄
* @param name 符号名称
* @return 是否成功
*/
static bool load_symbol(void** method, void* handle, const char* name) {
    *method = dlsym(handle, name);
    if (!*method) {
        fprintf(stderr, LOG_TAG "dlsym('%s') failed: %s\n", name, dlerror());
        return false;
    }
    return true;
}

// 元素数计算
/**
把 tensor 各维度连乘，得到元素总数
@param tensor 张量
@return 元素总数
*/
static uint32_t tensor_element_count(Qnn_Tensor_t* tensor) {
    uint32_t count = 1;
    for (uint32_t d = 0; d < QNN_TENSOR_RANK(tensor); d++)
        count *= QNN_TENSOR_DIMENSION(tensor, d);
    return count;
}

/**
返回 QNN 数据类型单元素字节数（1/2/4/8）
@param dt 数据类型
@return 字节数
*/
static uint32_t qnn_datatype_size(Qnn_DataType_t dt) {
    switch (dt) {
        case QNN_DATATYPE_UINT_8:  case QNN_DATATYPE_UFIXED_POINT_8:
        case QNN_DATATYPE_INT_8:   case QNN_DATATYPE_SFIXED_POINT_8:
        case QNN_DATATYPE_BOOL_8:
            return 1;
        case QNN_DATATYPE_UINT_16: case QNN_DATATYPE_UFIXED_POINT_16:
        case QNN_DATATYPE_INT_16:  case QNN_DATATYPE_SFIXED_POINT_16:
        case QNN_DATATYPE_FLOAT_16:
            return 2;
        case QNN_DATATYPE_UINT_32: case QNN_DATATYPE_UFIXED_POINT_32:
        case QNN_DATATYPE_INT_32:  case QNN_DATATYPE_SFIXED_POINT_32:
        case QNN_DATATYPE_FLOAT_32:
            return 4;
        case QNN_DATATYPE_UINT_64: case QNN_DATATYPE_INT_64:
        case QNN_DATATYPE_FLOAT_64:
            return 8;
        default: return 4;
    }
}
/**
 * 打印 QNN tensor 的基础信息：name / rank / dims / dtype / bytes / quant params
 */
static void print_tensor_debug_info(const char* prefix,
                                    Qnn_Tensor_t* tensor,
                                    uint32_t n_elem,
                                    uint32_t total_bytes) {
    if (!tensor) return;

    const char* name = QNN_TENSOR_NAME(tensor) ? QNN_TENSOR_NAME(tensor) : "<null>";
    const auto dtype = QNN_TENSOR_DATA_TYPE(tensor);
    const uint32_t rank = QNN_TENSOR_RANK(tensor);

    std::fprintf(stderr, LOG_TAG "%s tensor '%s': rank=%u, dims=[",
                 prefix, name, rank);

    for (uint32_t d = 0; d < rank; ++d) {
        std::fprintf(stderr, "%u%s",
                     QNN_TENSOR_DIMENSION(tensor, d),
                     (d + 1 < rank) ? "," : "");
    }

    std::fprintf(stderr,
                 "], elements=%u, dtype=0x%x, bytes=%u",
                 n_elem,
                 static_cast<unsigned>(dtype),
                 total_bytes);

    // 对量化 tensor，额外打印 scale / offset，方便确认输入量化参数
    if (dtype == QNN_DATATYPE_UFIXED_POINT_8 ||
        dtype == QNN_DATATYPE_SFIXED_POINT_8 ||
        dtype == QNN_DATATYPE_UFIXED_POINT_16 ||
        dtype == QNN_DATATYPE_SFIXED_POINT_16 ||
        dtype == QNN_DATATYPE_UFIXED_POINT_32 ||
        dtype == QNN_DATATYPE_SFIXED_POINT_32) {
        const auto& qp = QNN_TENSOR_QUANTIZE_PARAMS(tensor).scaleOffsetEncoding;
        std::fprintf(stderr,
                     ", quant_scale=%g, quant_offset=%d",
                     qp.scale,
                     qp.offset);
    }

    std::fprintf(stderr, "\n");
}


/**
从 systemContextGetBinaryInfo 返回的结构里，
抽取 graph 名、输入输出 tensor 信息，转换成本地 GraphInfo_t 数组
@param binary_info 二进制信息
@param graph_infos 图信息 输出，指向 GraphInfo_t* 数组
@param n_graphs 图数量
@return 是否成功
*/
static bool graph_info_from_binary_info(
    const QnnSystemContext_BinaryInfo_t* binary_info,
    GraphInfo_t**& graph_infos, uint32_t& n_graphs)
{
    if (!binary_info) {
        fprintf(stderr, LOG_TAG "binary_info is nullptr\n");
        return false;
    }
    if (!QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_SUPPORTED(binary_info)) {
        fprintf(stderr, LOG_TAG "Unsupported binary info version\n");
        return false;
    }
    n_graphs = QNN_GET_SYSTEM_CONTEXT_BINARY_INFO(binary_info).numGraphs;
    auto* graphs = QNN_GET_SYSTEM_CONTEXT_BINARY_INFO(binary_info).graphs;

    graph_infos = new GraphInfo_t*[n_graphs];
    auto* arr   = new GraphInfo_t[n_graphs];

    for (uint32_t i = 0; i < n_graphs; i++) {
        if (!QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_SUPPORTED(&graphs[i])) {
            fprintf(stderr, LOG_TAG "Unsupported graph info version at idx %u\n", i);
            delete[] arr;
            delete[] graph_infos;
            graph_infos = nullptr;
            return false;
        }
        arr[i].graphName        = QNN_GET_SYSTEM_CONTEXT_GRAPH_INFO(&graphs[i]).graphName;
        arr[i].numInputTensors  = QNN_GET_SYSTEM_CONTEXT_GRAPH_INFO(&graphs[i]).numGraphInputs;
        arr[i].inputTensors     = QNN_GET_SYSTEM_CONTEXT_GRAPH_INFO(&graphs[i]).graphInputs;
        arr[i].numOutputTensors = QNN_GET_SYSTEM_CONTEXT_GRAPH_INFO(&graphs[i]).numGraphOutputs;
        arr[i].outputTensors    = QNN_GET_SYSTEM_CONTEXT_GRAPH_INFO(&graphs[i]).graphOutputs;
        graph_infos[i] = &arr[i];
    }
    return true;
}

/**
* 输出反量化。根据 dtype 类型，把原始数据转换为 float32 类型，存储到 out_buf 中
* @param tensor 用于读 dtype 和量化参数
* @param raw_data 原始字节缓冲
* @param out_buf 目标 float 缓冲
* @param n_elements 元素数量
*/
static void dequantize_output(Qnn_Tensor_t* tensor,
                              const uint8_t* raw_data, float* out_buf,
                              uint32_t n_elements)
{
    switch (QNN_TENSOR_DATA_TYPE(tensor)) {
        case QNN_DATATYPE_UFIXED_POINT_8: {
            int32_t offset = QNN_TENSOR_QUANTIZE_PARAMS(tensor).scaleOffsetEncoding.offset;
            float scale    = QNN_TENSOR_QUANTIZE_PARAMS(tensor).scaleOffsetEncoding.scale;
            for (uint32_t i = 0; i < n_elements; i++)
                out_buf[i] = static_cast<float>(raw_data[i] + offset) * scale;
            break;
        }
        case QNN_DATATYPE_UFIXED_POINT_16: {
            auto* data = reinterpret_cast<const uint16_t*>(raw_data);
            int32_t offset = QNN_TENSOR_QUANTIZE_PARAMS(tensor).scaleOffsetEncoding.offset;
            float scale    = QNN_TENSOR_QUANTIZE_PARAMS(tensor).scaleOffsetEncoding.scale;
            for (uint32_t i = 0; i < n_elements; i++)
                out_buf[i] = static_cast<float>(data[i] + offset) * scale;
            break;
        }
        case QNN_DATATYPE_UINT_8: {
            for (uint32_t i = 0; i < n_elements; i++)
                out_buf[i] = static_cast<float>(raw_data[i]);
            break;
        }
        case QNN_DATATYPE_UINT_16: {
            auto* data = reinterpret_cast<const uint16_t*>(raw_data);
            for (uint32_t i = 0; i < n_elements; i++)
                out_buf[i] = static_cast<float>(data[i]);
            break;
        }
        case QNN_DATATYPE_INT_8: {
            auto* data = reinterpret_cast<const int8_t*>(raw_data);
            for (uint32_t i = 0; i < n_elements; i++)
                out_buf[i] = static_cast<float>(data[i]);
            break;
        }
        case QNN_DATATYPE_INT_16: {
            auto* data = reinterpret_cast<const int16_t*>(raw_data);
            for (uint32_t i = 0; i < n_elements; i++)
                out_buf[i] = static_cast<float>(data[i]);
            break;
        }
        case QNN_DATATYPE_INT_32: {
            auto* data = reinterpret_cast<const int32_t*>(raw_data);
            for (uint32_t i = 0; i < n_elements; i++)
                out_buf[i] = static_cast<float>(data[i]);
            break;
        }
        case QNN_DATATYPE_UINT_32: {
            auto* data = reinterpret_cast<const uint32_t*>(raw_data);
            for (uint32_t i = 0; i < n_elements; i++)
                out_buf[i] = static_cast<float>(data[i]);
            break;
        }
        case QNN_DATATYPE_FLOAT_32: {
            std::memcpy(out_buf, raw_data, n_elements * sizeof(float));
            break;
        }
        default:
            fprintf(stderr, LOG_TAG "Unsupported output datatype 0x%x, zero-filling\n",
                    static_cast<unsigned>(QNN_TENSOR_DATA_TYPE(tensor)));
            std::memset(out_buf, 0, n_elements * sizeof(float));
            break;
    }
}

// ============================================================
// Impl 定义 — 包含 Phase A-D 方法
// ============================================================
struct QnnInferencer::Impl {
    // --- 字段 ---
    void* libhandle    = nullptr;
    void* syslibhandle = nullptr;

    QNN_INTERFACE_VER_TYPE         interface{};
    QNN_SYSTEM_INTERFACE_VER_TYPE  sysinterface{};

    Qnn_LogHandle_t            logger   = nullptr;
    Qnn_ProfileHandle_t        profiler = nullptr;
    Qnn_DeviceHandle_t         device   = nullptr;
    Qnn_BackendHandle_t        backend  = nullptr;
    Qnn_ContextHandle_t        context  = nullptr;
    QnnSystemContext_Handle_t  sysctx   = nullptr;
    const QnnDevice_PlatformInfo_t* device_platform = nullptr;

    GraphInfo_t** graph_infos = nullptr;
    uint32_t      n_graphs    = 0;

    std::vector<uint8_t> input_buf;

    std::vector<std::vector<uint8_t>> raw_output_bufs;
    std::vector<std::vector<float>>   output_bufs;
    std::vector<uint32_t>             output_elem_counts;
    std::vector<std::string>          output_names;

    std::vector<char> model_binary;

    int requested_device_id = 0;

    /**
    * Phase A: LoadBackend (ml-faceqnn-engine.cc:540-688)
    作用：1.dlopen后端库（如HTP）；2.获取QNN接口提供者；3.创建日志记录器；4.创建后端；5.创建性能分析器；
    6.尝试创建设备；7.创建上下文；8.创建图；9.创建张量缓冲区
    * @param backend_path 后端so路径
    * @return 是否成功
    */
    bool LoadBackend(const std::string& backend_path) {
        libhandle = dlopen(backend_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!libhandle) {
            fprintf(stderr, LOG_TAG "dlopen('%s') failed: %s\n",
                    backend_path.c_str(), dlerror());
            return false;
        }

        QnnInterfaceGetProvidersFn* GetProviders = nullptr;
        if (!load_symbol(reinterpret_cast<void**>(&GetProviders),
                         libhandle, "QnnInterface_getProviders"))
            return false;

        const QnnInterface_t** providers = nullptr;
        uint32_t n_providers = 0;
        if (GetProviders(&providers, &n_providers) != QNN_SUCCESS) {
            fprintf(stderr, LOG_TAG "QnnInterface_getProviders failed\n");
            return false;
        }
        if (!providers || n_providers == 0) {
            fprintf(stderr, LOG_TAG "No QNN interface providers\n");
            return false;
        }
        interface = providers[0]->QNN_INTERFACE_VER_NAME;

        auto status = interface.logCreate(qnn_log_callback,
                                           QNN_LOG_LEVEL_WARN, &logger);
        if (status != QNN_SUCCESS) {
            fprintf(stderr, LOG_TAG "logCreate failed (0x%lx)\n",
                    static_cast<unsigned long>(QNN_GET_ERROR_CODE(status)));
            return false;
        }

        status = interface.backendCreate(logger, nullptr, &backend);
        if (status != QNN_SUCCESS) {
            fprintf(stderr, LOG_TAG "backendCreate failed (0x%lx)\n",
                    static_cast<unsigned long>(QNN_GET_ERROR_CODE(status)));
            return false;
        }

        status = interface.profileCreate(backend,
                                          QNN_PROFILE_LEVEL_BASIC, &profiler);
        if (status != QNN_SUCCESS) {
            fprintf(stderr, LOG_TAG "profileCreate failed (0x%lx)\n",
                    static_cast<unsigned long>(QNN_GET_ERROR_CODE(status)));
            return false;
        }

        // deviceGetPlatformInfo → deviceCreate (ml-faceqnn-engine.cc:499-536)
        auto dev_status = interface.deviceGetPlatformInfo(nullptr, &device_platform);
        if (dev_status == QNN_SUCCESS && device_platform) {
            QnnDevice_HardwareDeviceInfo_t* chosen = nullptr;
            for (uint32_t i = 0; i < device_platform->v1.numHwDevices; i++) {
                if (device_platform->v1.hwDevices[i].v1.deviceId ==
                    static_cast<uint32_t>(requested_device_id)) {
                    chosen = &device_platform->v1.hwDevices[i];
                    break;
                }
            }
            if (chosen) {
                QnnDevice_PlatformInfo_t plat{};
                plat.version = QNN_DEVICE_PLATFORM_INFO_VERSION_1;
                plat.v1.numHwDevices = 1;
                plat.v1.hwDevices = chosen;

                QnnDevice_Config_t cfg{};
                cfg.option = QNN_DEVICE_CONFIG_OPTION_PLATFORM_INFO;
                cfg.hardwareInfo = &plat;
                const QnnDevice_Config_t* cfgs[2] = {&cfg, nullptr};
                auto dc_status = interface.deviceCreate(logger, cfgs, &device);
                if (dc_status != QNN_SUCCESS) {
                    fprintf(stderr,
                            LOG_TAG "deviceCreate(deviceId=%d) failed (0x%lx)\n",
                            requested_device_id,
                            static_cast<unsigned long>(
                                QNN_GET_ERROR_CODE(dc_status)));
                    return false;
                }
                fprintf(stderr, LOG_TAG "deviceCreate OK deviceId=%d\n",
                        requested_device_id);
            } else {
                fprintf(stderr,
                        LOG_TAG "HTP deviceId=%d not in platform info "
                                "(numHwDevices=%u), using default device\n",
                        requested_device_id,
                        device_platform->v1.numHwDevices);
            }
        } else if (dev_status != QNN_DEVICE_ERROR_UNSUPPORTED_FEATURE) {
            fprintf(stderr, LOG_TAG "deviceGetPlatformInfo failed (non-fatal)\n");
        }
        return true;
    }

    /**
    * Phase B: LoadSystemLib (ml-faceqnn-engine.cc:645-684)
    * 作用：加载 system so，拿到 QnnSystemInterface（用于解析 context binary 信息）
    * @param system_path 系统库路径
    * @return 是否成功
    */
    bool LoadSystemLib(const std::string& system_path) {
        syslibhandle = dlopen(system_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!syslibhandle) {
            fprintf(stderr, LOG_TAG "dlopen('%s') failed: %s\n",
                    system_path.c_str(), dlerror());
            return false;
        }

        QnnSystemInterfaceGetProvidersFn* GetSysProviders = nullptr;
        if (!load_symbol(reinterpret_cast<void**>(&GetSysProviders),
                         syslibhandle, "QnnSystemInterface_getProviders"))
            return false;

        const QnnSystemInterface_t** sys_providers = nullptr;
        uint32_t n = 0;
        if (GetSysProviders(
                const_cast<const QnnSystemInterface_t***>(&sys_providers), &n)
                != QNN_SUCCESS || !sys_providers || n == 0) {
            fprintf(stderr, LOG_TAG "QnnSystemInterface_getProviders failed\n");
            return false;
        }
        sysinterface = sys_providers[0]->QNN_SYSTEM_INTERFACE_VER_NAME;
        return true;
    }

    // --- Phase C: LoadCachedGraphs (ml-faceqnn-engine.cc:690-804) ---
    bool LoadCachedGraphs(const std::string& model_path) {
        /**
        作用：
        1.读取模型二进制文件到内存
        2.用 systemContextGetBinaryInfo 拿 graph 元信息
        3.graph_info_from_binary_info 转成本地结构
        4.contextCreateFromBinary 创建 QNN context
        5.graphRetrieve 取每个 graph handle

        */
        std::ifstream ifs(model_path, std::ios::binary | std::ios::ate);
        if (!ifs) {
            fprintf(stderr, LOG_TAG "Cannot open model file '%s'\n", model_path.c_str());
            return false;
        }
        auto file_size = ifs.tellg();
        ifs.seekg(0, std::ios::beg);
        model_binary.resize(static_cast<size_t>(file_size));
        if (!ifs.read(model_binary.data(), file_size)) {
            fprintf(stderr, LOG_TAG "Failed to read model file\n");
            return false;
        }
        ifs.close();
        fprintf(stderr, LOG_TAG "Loaded model binary: %zu bytes\n", model_binary.size());

        if (!sysinterface.systemContextCreate ||
            !sysinterface.systemContextGetBinaryInfo ||
            !sysinterface.systemContextFree) {
            fprintf(stderr, LOG_TAG "System interface function pointers are null\n");
            return false;
        }
        auto status = sysinterface.systemContextCreate(&sysctx);
        if (status != QNN_SUCCESS) {
            fprintf(stderr, LOG_TAG "systemContextCreate failed\n");
            return false;
        }

        const QnnSystemContext_BinaryInfo_t* binary_info = nullptr;
        Qnn_ContextBinarySize_t binary_info_size = 0;
        status = sysinterface.systemContextGetBinaryInfo(
            sysctx, static_cast<void*>(model_binary.data()),
            model_binary.size(), &binary_info, &binary_info_size);
        if (status != QNN_SUCCESS) {
            fprintf(stderr, LOG_TAG "systemContextGetBinaryInfo failed\n");
            return false;
        }

        if (!graph_info_from_binary_info(binary_info, graph_infos, n_graphs)) {
            fprintf(stderr, LOG_TAG "graph_info_from_binary_info failed\n");
            return false;
        }
        fprintf(stderr, LOG_TAG "Found %u graph(s) in model\n", n_graphs);

        if (!interface.contextCreateFromBinary) {
            fprintf(stderr, LOG_TAG "contextCreateFromBinary is null\n");
            return false;
        }
        auto ctx_status = interface.contextCreateFromBinary(
            backend, device, nullptr,
            static_cast<void*>(model_binary.data()), model_binary.size(),
            &context, profiler);
        if (ctx_status != QNN_SUCCESS) {
            fprintf(stderr, LOG_TAG "contextCreateFromBinary failed (0x%lx)\n",
                    static_cast<unsigned long>(QNN_GET_ERROR_CODE(ctx_status)));
            return false;
        }

        for (uint32_t i = 0; i < n_graphs; i++) {
            status = interface.graphRetrieve(
                context, graph_infos[i]->graphName, &graph_infos[i]->graph);
            if (status != QNN_SUCCESS) {
                fprintf(stderr, LOG_TAG "graphRetrieve failed for graph %u\n", i);
                return false;
            }
        }

        // 模型已被 QNN 内部消化, CPU 侧副本不再需要, 立即释放
        model_binary.clear();
        model_binary.shrink_to_fit();

        return true;
    }

    /**
    * Phase D: AllocTensorBuffers (plan_step4.md §2)
    * 作用：
    1.给输入 tensor 分配 input_buf 并挂到 clientBuf
    2.给每个输出分配原始缓冲 raw_output_bufs[i] 和 float 缓冲 output_bufs[i]
    3.保存输出元素数和输出名
    * @return 是否成功
    */
    bool AllocTensorBuffers() {
        const GraphInfo_t* gi = graph_infos[0];

        if (gi->numInputTensors < 1) {
            fprintf(stderr, LOG_TAG "No input tensors\n");
            return false;
        }
        {
            Qnn_Tensor_t* t = &gi->inputTensors[0];
            uint32_t n_elem  = tensor_element_count(t);
            uint32_t dt_size = qnn_datatype_size(QNN_TENSOR_DATA_TYPE(t));
            uint32_t total   = n_elem * dt_size;
            input_buf.resize(total, 0);
            QNN_TENSOR_CLIENTBUF(t).data     = input_buf.data();
            QNN_TENSOR_CLIENTBUF(t).dataSize = total;
            // fprintf(stderr, LOG_TAG "Input tensor '%s': %u elements, %u bytes\n",
            //         QNN_TENSOR_NAME(t), n_elem, total);
            print_tensor_debug_info("Input", t, n_elem, total);
        }

        uint32_t n_outputs = gi->numOutputTensors;
        raw_output_bufs.resize(n_outputs);
        output_bufs.resize(n_outputs);
        output_elem_counts.resize(n_outputs);
        output_names.resize(n_outputs);

        for (uint32_t i = 0; i < n_outputs; i++) {
            Qnn_Tensor_t* t = &gi->outputTensors[i];
            uint32_t n_elem      = tensor_element_count(t);
            uint32_t dt_size     = qnn_datatype_size(QNN_TENSOR_DATA_TYPE(t));
            uint32_t native_bytes = n_elem * dt_size;

            raw_output_bufs[i].resize(native_bytes, 0);
            output_bufs[i].resize(n_elem, 0.0f);
            output_elem_counts[i] = n_elem;
            output_names[i] = QNN_TENSOR_NAME(t) ? QNN_TENSOR_NAME(t) : "";

            QNN_TENSOR_CLIENTBUF(t).data     = raw_output_bufs[i].data();
            QNN_TENSOR_CLIENTBUF(t).dataSize = native_bytes;

            // fprintf(stderr, LOG_TAG "Output tensor[%u] '%s': %u elements, "
            //         "dtype=0x%x, %u bytes\n",
            //         i, output_names[i].c_str(), n_elem,
            //         static_cast<unsigned>(QNN_TENSOR_DATA_TYPE(t)), native_bytes);
            char prefix[64];
            std::snprintf(prefix, sizeof(prefix), "Output tensor[%u]", i);
            print_tensor_debug_info(prefix, t, n_elem, native_bytes);
        }
        return true;
    }
};

// ============================================================
// QnnInferencer 公共接口
// ============================================================

QnnInferencer::~QnnInferencer() {
    Destroy();
}

/**
作用：按 A→B→C→D 初始化所有运行所需资源；任何阶段失败即清理并返回 false。
参数：后端库路径、system 库路径、模型路径
*/
bool QnnInferencer::Init(const std::string& backend_path,
                          const std::string& system_path,
                          const std::string& model_path,
                          int device_id) {
    if (initialized_) {
        fprintf(stderr, LOG_TAG "Already initialized\n");
        return false;
    }

    impl_ = new Impl();
    impl_->requested_device_id = device_id;

    if (!impl_->LoadBackend(backend_path)) {
        fprintf(stderr, LOG_TAG "Phase A (LoadBackend) failed\n");
        Destroy(); return false;
    }
    fprintf(stderr, LOG_TAG "Phase A (LoadBackend) OK\n");

    if (!impl_->LoadSystemLib(system_path)) {
        fprintf(stderr, LOG_TAG "Phase B (LoadSystemLib) failed\n");
        Destroy(); return false;
    }
    fprintf(stderr, LOG_TAG "Phase B (LoadSystemLib) OK\n");

    if (!impl_->LoadCachedGraphs(model_path)) {
        fprintf(stderr, LOG_TAG "Phase C (LoadCachedGraphs) failed\n");
        Destroy(); return false;
    }
    fprintf(stderr, LOG_TAG "Phase C (LoadCachedGraphs) OK\n");

    if (!impl_->AllocTensorBuffers()) {
        fprintf(stderr, LOG_TAG "Phase D (AllocTensorBuffers) failed\n");
        Destroy(); return false;
    }
    fprintf(stderr, LOG_TAG "Phase D (AllocTensorBuffers) OK\n");

    initialized_ = true;
    fprintf(stderr, LOG_TAG "Init complete: %u output tensor(s)\n",
            GetNumOutputTensors());
    return true;
}

/**
作用：

根据输入 tensor dtype 处理输入：
    UFIXED_POINT_8：float32 → uint8 量化
    SFIXED_POINT_8：float32 → int8 量化
    其他：直接 memcpy（保持旧路径兼容）
调 graphExecute
对每个输出调用 dequantize_output 到 float
 */
bool QnnInferencer::Execute(const float* input_data, size_t input_bytes) {
    if (!initialized_ || !impl_) return false;

    const GraphInfo_t* gi = impl_->graph_infos[0];

    // ============================================================
    // 输入装填: 根据模型输入张量的 dtype 选择路径
    //
    //   1) FLOAT_32 / FLOAT_16 / 其它 (含原 SCRFD 浮点模型路径)
    //      → 与改动前完全一致: 直接 memcpy 字节流
    //
    //   2) UFIXED_POINT_8 (w8 非对称量化, 例如 mobilenet_v2_w8a8)
    //      → 调用方传入 float32 [实际值], 这里用 tensor 自带的
    //        scale/offset 量化为 uint8 写入 client buffer.
    //        QNN 公式:  q_u8 = clamp( round(x / scale) - offset, 0, 255 )
    //
    //   3) SFIXED_POINT_8 (w8 对称量化)
    //      → 同 2), 但量化到 int8 [-128, 127]
    //
    // 这样:
    //   - 调用方对所有量化/非量化模型统一传 float32 数据;
    //   - 原有 SCRFD (FLOAT_32) 调用点零修改, 行为逐字节一致.
    // ============================================================
    Qnn_Tensor_t* in_t = &gi->inputTensors[0];
    const Qnn_DataType_t in_dt = QNN_TENSOR_DATA_TYPE(in_t);

    if (stage_cb_) stage_cb_(face_async::FaceStage::QNN_COPY, true);
    if (in_dt == QNN_DATATYPE_UFIXED_POINT_8) {
        const size_t n_elem = impl_->input_buf.size();           // uint8 元素数
        const size_t expect_fbytes = n_elem * sizeof(float);
        if (input_bytes < expect_fbytes) {
            fprintf(stderr,
                LOG_TAG "Execute: UFIXED_POINT_8 input expects float32 buffer "
                "of %zu bytes, but got %zu\n",
                expect_fbytes, input_bytes);
            return false;
        }
        const auto& qp =
            QNN_TENSOR_QUANTIZE_PARAMS(in_t).scaleOffsetEncoding;
        const float   scale  = qp.scale;
        const int32_t offset = qp.offset;
        if (scale <= 0.0f) {
            fprintf(stderr,
                LOG_TAG "Execute: invalid quant scale=%g for UFIXED_POINT_8 input\n",
                scale);
            return false;
        }
        const float inv_scale = 1.0f / scale;
        uint8_t* dst = impl_->input_buf.data();
        for (size_t i = 0; i < n_elem; ++i) {
            int32_t q = static_cast<int32_t>(std::lrintf(input_data[i] * inv_scale))
                        - offset;
            if (q < 0)        q = 0;
            else if (q > 255) q = 255;
            dst[i] = static_cast<uint8_t>(q);
        }
    } else if (in_dt == QNN_DATATYPE_SFIXED_POINT_8) {
        const size_t n_elem = impl_->input_buf.size();           // int8 元素数
        const size_t expect_fbytes = n_elem * sizeof(float);
        if (input_bytes < expect_fbytes) {
            fprintf(stderr,
                LOG_TAG "Execute: SFIXED_POINT_8 input expects float32 buffer "
                "of %zu bytes, but got %zu\n",
                expect_fbytes, input_bytes);
            return false;
        }
        const auto& qp =
            QNN_TENSOR_QUANTIZE_PARAMS(in_t).scaleOffsetEncoding;
        const float   scale  = qp.scale;
        const int32_t offset = qp.offset;
        if (scale <= 0.0f) {
            fprintf(stderr,
                LOG_TAG "Execute: invalid quant scale=%g for SFIXED_POINT_8 input\n",
                scale);
            return false;
        }
        const float inv_scale = 1.0f / scale;
        int8_t* dst = reinterpret_cast<int8_t*>(impl_->input_buf.data());
        for (size_t i = 0; i < n_elem; ++i) {
            int32_t q = static_cast<int32_t>(std::lrintf(input_data[i] * inv_scale))
                        - offset;
            if (q < -128)      q = -128;
            else if (q >  127) q =  127;
            dst[i] = static_cast<int8_t>(q);
        }
    } else {
        // 原有路径 (FLOAT_32 / FLOAT_16 / 其它): 行为完全不变
        size_t copy_size = std::min(input_bytes, impl_->input_buf.size());
        std::memcpy(impl_->input_buf.data(), input_data, copy_size);
    }
    if (stage_cb_) stage_cb_(face_async::FaceStage::QNN_COPY, false);

    if (stage_cb_) stage_cb_(face_async::FaceStage::GRAPH_EXECUTE, true);
    auto status = impl_->interface.graphExecute(
        gi->graph,
        gi->inputTensors,  gi->numInputTensors,
        gi->outputTensors, gi->numOutputTensors,
        impl_->profiler, nullptr);
    if (stage_cb_) stage_cb_(face_async::FaceStage::GRAPH_EXECUTE, false);

    if (status != QNN_GRAPH_NO_ERROR) {
        fprintf(stderr, LOG_TAG "graphExecute failed (0x%lx)\n",
                static_cast<unsigned long>(QNN_GET_ERROR_CODE(status)));
        return false;
    }

    if (stage_cb_) stage_cb_(face_async::FaceStage::DEQUANT, true);
    for (uint32_t i = 0; i < gi->numOutputTensors; i++) {
        Qnn_Tensor_t* t = &gi->outputTensors[i];
        dequantize_output(t,
                          impl_->raw_output_bufs[i].data(),
                          impl_->output_bufs[i].data(),
                          impl_->output_elem_counts[i]);
    }
    if (stage_cb_) stage_cb_(face_async::FaceStage::DEQUANT, false);
    return true;
}

const float* QnnInferencer::GetOutputData(uint32_t tensor_idx) const {
    if (!initialized_ || !impl_ || tensor_idx >= impl_->output_bufs.size())
        return nullptr;
    return impl_->output_bufs[tensor_idx].data();
}

uint32_t QnnInferencer::GetOutputTensorSize(uint32_t tensor_idx) const {
    if (!initialized_ || !impl_ || tensor_idx >= impl_->output_elem_counts.size())
        return 0;
    return impl_->output_elem_counts[tensor_idx];
}

uint32_t QnnInferencer::GetNumOutputTensors() const {
    if (!initialized_ || !impl_) return 0;
    return static_cast<uint32_t>(impl_->output_bufs.size());
}

std::string QnnInferencer::GetOutputTensorName(uint32_t tensor_idx) const {
    if (!initialized_ || !impl_ || tensor_idx >= impl_->output_names.size())
        return "";
    return impl_->output_names[tensor_idx];
}

// ============================================================
// Destroy (ml-faceqnn-engine.cc:1092-1188, 逆序释放)
// ============================================================
void QnnInferencer::Destroy() {
    if (!impl_) return;

    if (impl_->graph_infos && impl_->n_graphs > 0) {
        const GraphInfo_t* gi = impl_->graph_infos[0];
        for (uint32_t i = 0; i < gi->numInputTensors; i++) {
            QNN_TENSOR_CLIENTBUF(&gi->inputTensors[i]).data = nullptr;
            QNN_TENSOR_CLIENTBUF(&gi->inputTensors[i]).dataSize = 0;
        }
        for (uint32_t i = 0; i < gi->numOutputTensors; i++) {
            QNN_TENSOR_CLIENTBUF(&gi->outputTensors[i]).data = nullptr;
            QNN_TENSOR_CLIENTBUF(&gi->outputTensors[i]).dataSize = 0;
        }
    }

    if (impl_->sysinterface.systemContextFree && impl_->sysctx) {
        impl_->sysinterface.systemContextFree(impl_->sysctx);
        impl_->sysctx = nullptr;
    }

    if (impl_->graph_infos) {
        delete[] impl_->graph_infos[0];
        delete[] impl_->graph_infos;
        impl_->graph_infos = nullptr;
        impl_->n_graphs = 0;
    }

    if (impl_->interface.contextFree && impl_->context) {
        impl_->interface.contextFree(impl_->context, nullptr);
        impl_->context = nullptr;
    }

    if (impl_->interface.deviceFree && impl_->device) {
        impl_->interface.deviceFree(impl_->device);
        impl_->device = nullptr;
    }

    if (impl_->interface.deviceFreePlatformInfo && impl_->device_platform) {
        impl_->interface.deviceFreePlatformInfo(nullptr, impl_->device_platform);
        impl_->device_platform = nullptr;
    }

    if (impl_->interface.profileFree && impl_->profiler) {
        impl_->interface.profileFree(impl_->profiler);
        impl_->profiler = nullptr;
    }

    if (impl_->interface.backendFree && impl_->backend) {
        impl_->interface.backendFree(impl_->backend);
        impl_->backend = nullptr;
    }

    if (impl_->interface.logFree && impl_->logger) {
        impl_->interface.logFree(impl_->logger);
        impl_->logger = nullptr;
    }

    if (impl_->syslibhandle) {
        dlclose(impl_->syslibhandle);
        impl_->syslibhandle = nullptr;
    }
    if (impl_->libhandle) {
        dlclose(impl_->libhandle);
        impl_->libhandle = nullptr;
    }

    delete impl_;
    impl_ = nullptr;
    initialized_ = false;
    fprintf(stderr, LOG_TAG "Destroyed\n");
}

}  // namespace face_infer
