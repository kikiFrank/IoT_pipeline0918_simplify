/// @file config/FaceConfigLoader.hpp
/// @brief 人脸模块独立配置加载器
///
/// 仅解析 face_config.json, 不依赖主应用 ConfigLoader 或第三方 JSON 库。
/// 使用逐行 key-value 解析 (JSON 格式简单, 无嵌套对象)。
/// Header-only, 无 .cpp。
///
/// 用法:
///   face_infer::FaceProcessorConfig cfg;
///   face_infer::LoadFaceConfig("config/face_config.json", cfg);

#pragma once

#include "../FaceProcessor.hpp"
#include <string>
#include <fstream>
#include <cstdio>
#include <cstdlib>

namespace face_infer {

namespace detail {

/// 解析 "key": "value" 模式
inline bool parse_json_string(const std::string& line, const char* key,
                               std::string& out) {
    std::string k = std::string("\"") + key + "\"";
    auto pos = line.find(k);
    if (pos == std::string::npos) return false;
    auto colon = line.find(':', pos + k.size());
    if (colon == std::string::npos) return false;
    auto q1 = line.find('"', colon + 1);
    if (q1 == std::string::npos) return false;
    auto q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;
    out = line.substr(q1 + 1, q2 - q1 - 1);
    return true;
}

/// 解析 "key": 0.5 模式
inline bool parse_json_number(const std::string& line, const char* key,
                               double& out) {
    std::string k = std::string("\"") + key + "\"";
    auto pos = line.find(k);
    if (pos == std::string::npos) return false;
    auto colon = line.find(':', pos + k.size());
    if (colon == std::string::npos) return false;
    auto start = colon + 1;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
        start++;
    char* end = nullptr;
    out = strtod(line.c_str() + start, &end);
    return end != line.c_str() + start;
}

/// 相对路径 → 绝对路径 (相对于 config_dir)
inline std::string resolve_path(const std::string& config_dir,
                                 const std::string& path) {
    if (path.empty() || path[0] == '/') return path;
    return config_dir + "/" + path;
}

/// 从文件路径中提取目录部分
inline std::string dir_of(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return ".";
    return path.substr(0, pos);
}

}  // namespace detail

/// 从 face_config.json 加载人脸模块配置
///
/// @param json_path  face_config.json 文件路径
/// @param cfg        输出配置 (已填默认值, 仅覆盖 JSON 中出现的字段)
/// @return true on success
inline bool LoadFaceConfig(const std::string& json_path,
                            FaceProcessorConfig& cfg) {
    std::ifstream ifs(json_path);
    if (!ifs.is_open()) {
        fprintf(stderr, "[FaceConfig] Cannot open: %s\n", json_path.c_str());
        return false;
    }

    std::string config_dir = detail::dir_of(json_path);
    std::string line, val;
    double num;

    while (std::getline(ifs, line)) {
        if (detail::parse_json_string(line, "scrfd_model", val))
            cfg.scrfd_model = detail::resolve_path(config_dir, val);
        else if (detail::parse_json_string(line, "arcface_model", val))
            cfg.arcface_model = detail::resolve_path(config_dir, val);
        else if (detail::parse_json_string(line, "opencl_kernel_path", val))
            cfg.opencl_kernel_path = detail::resolve_path(config_dir, val);
        else if (detail::parse_json_string(line, "qnn_backend", val))
            cfg.qnn_backend = val;
        else if (detail::parse_json_string(line, "qnn_system", val))
            cfg.qnn_system = val;
        else if (detail::parse_json_number(line, "conf_threshold", num))
            cfg.conf_threshold = static_cast<float>(num);
        else if (detail::parse_json_number(line, "nms_threshold", num))
            cfg.nms_threshold = static_cast<float>(num);
        else if (detail::parse_json_number(line, "max_faces", num))
            cfg.max_faces = static_cast<int>(num);
    }

    if (cfg.scrfd_model.empty()) {
        fprintf(stderr, "[FaceConfig] 'scrfd_model' is required\n");
        return false;
    }
    if (cfg.opencl_kernel_path.empty()) {
        fprintf(stderr, "[FaceConfig] 'opencl_kernel_path' is required\n");
        return false;
    }

    fprintf(stderr, "[FaceConfig] Loaded from: %s\n", json_path.c_str());
    fprintf(stderr, "  scrfd_model:   %s\n", cfg.scrfd_model.c_str());
    fprintf(stderr, "  arcface_model: %s\n",
            cfg.arcface_model.empty() ? "(disabled)"
                                       : cfg.arcface_model.c_str());
    fprintf(stderr, "  kernel_path:   %s\n", cfg.opencl_kernel_path.c_str());
    fprintf(stderr, "  conf=%.2f  nms=%.2f  max_faces=%d\n",
            cfg.conf_threshold, cfg.nms_threshold, cfg.max_faces);
    return true;
}

}  // namespace face_infer
