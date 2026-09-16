/// @file scrfd_preprocess.cl
/// @brief NV12 DMABuf → float32 NCHW 640×640 (letterbox + BT.601 limited-range + normalize)
///
/// 移植自: face_preprocess.cu:14-68 (scrfd_preprocess_kernel)
/// 关键差异: CUDA 版输入为 RGB HWC uint8; 本版输入为 NV12 单 cl_mem + 偏移量
///
/// nv12_buf: 单个 cl_mem 覆盖整个 NV12 帧 (与 overlay_blit_rgba_kernel.cl 同模式)
/// Y/UV 通过 y_offset/uv_offset 偏移访问
/// 参考: overlay_blit_rgba_kernel.cl 使用 __global uchar* frame + y_offset + nv_offset

__kernel void scrfd_preprocess(
    __global const uchar* nv12_buf,   // 单个 DMABuf import (整帧)
    __global float* output,           // 3 * dst_size * dst_size NCHW
    uint y_offset,                    // Y 平面字节偏移
    uint uv_offset,                   // UV 平面字节偏移
    uint y_stride,                    // Y 平面行步长 (字节)
    uint uv_stride,                   // UV 平面行步长 (字节)
    uint src_w,                       // 源帧宽 (像素)
    uint src_h,                       // 源帧高 (像素)
    uint dst_size,                    // 目标尺寸 (640)
    uint new_w,                       // letterbox 有效宽
    uint new_h,                       // letterbox 有效高
    float scale)                      // src_w / new_w (逆缩放因子)
{
    int dx = get_global_id(0);
    int dy = get_global_id(1);
    if (dx >= (int)dst_size || dy >= (int)dst_size) return;

    uint plane_size = dst_size * dst_size;
    uint idx = dy * dst_size + dx;

    // ─── Letterbox 填充区域: (0 - 127.5) / 128.0 = -0.99609375 ───
    if (dx >= (int)new_w || dy >= (int)new_h) {
        output[0 * plane_size + idx] = -0.99609375f;
        output[1 * plane_size + idx] = -0.99609375f;
        output[2 * plane_size + idx] = -0.99609375f;
        return;
    }

    // ─── 坐标映射: dst → src (与 CUDA 参考一致, face_preprocess.cu:38-39) ───
    float src_x = (dx + 0.5f) * scale - 0.5f;
    float src_y = (dy + 0.5f) * scale - 0.5f;
    src_x = clamp(src_x, 0.0f, (float)(src_w - 1));
    src_y = clamp(src_y, 0.0f, (float)(src_h - 1));

    // ─── Y 平面双线性插值 ───
    int x0 = (int)floor(src_x);
    int y0 = (int)floor(src_y);
    int x1 = min(x0 + 1, (int)src_w - 1);
    int y1 = min(y0 + 1, (int)src_h - 1);
    float fx = src_x - (float)x0;
    float fy = src_y - (float)y0;

    float Y00 = (float)nv12_buf[y_offset + y0 * y_stride + x0];
    float Y01 = (float)nv12_buf[y_offset + y0 * y_stride + x1];
    float Y10 = (float)nv12_buf[y_offset + y1 * y_stride + x0];
    float Y11 = (float)nv12_buf[y_offset + y1 * y_stride + x1];

    // 注意: facealign.c:109 有 bug — (1-fy) 误写为 (1-fy), 应为 (1-fx)
    // 此处使用正确公式
    float Y = (1.0f - fy) * ((1.0f - fx) * Y00 + fx * Y01)
            + fy * ((1.0f - fx) * Y10 + fx * Y11);

    // ─── UV 平面双线性插值 (NV12 chroma 2×2 子采样) ───
    // 参考: facealign.c:113-151 (full bilinear on UV)
    float ux = src_x * 0.5f;
    float uy = src_y * 0.5f;
    int ux0 = (int)floor(ux);
    int uy0 = (int)floor(uy);
    int ux1 = min(ux0 + 1, (int)(src_w / 2) - 1);
    int uy1 = min(uy0 + 1, (int)(src_h / 2) - 1);
    ux0 = clamp(ux0, 0, (int)(src_w / 2) - 1);
    uy0 = clamp(uy0, 0, (int)(src_h / 2) - 1);
    float ufx = ux - floor(ux);
    float ufy = uy - floor(uy);

    // NV12: U 在偶数索引, V 在奇数索引
    float U00 = (float)nv12_buf[uv_offset + uy0 * uv_stride + ux0 * 2];
    float U01 = (float)nv12_buf[uv_offset + uy0 * uv_stride + ux1 * 2];
    float U10 = (float)nv12_buf[uv_offset + uy1 * uv_stride + ux0 * 2];
    float U11 = (float)nv12_buf[uv_offset + uy1 * uv_stride + ux1 * 2];

    float V00 = (float)nv12_buf[uv_offset + uy0 * uv_stride + ux0 * 2 + 1];
    float V01 = (float)nv12_buf[uv_offset + uy0 * uv_stride + ux1 * 2 + 1];
    float V10 = (float)nv12_buf[uv_offset + uy1 * uv_stride + ux0 * 2 + 1];
    float V11 = (float)nv12_buf[uv_offset + uy1 * uv_stride + ux1 * 2 + 1];

    float U = (1.0f - ufy) * ((1.0f - ufx) * U00 + ufx * U01)
            + ufy * ((1.0f - ufx) * U10 + ufx * U11);
    float V = (1.0f - ufy) * ((1.0f - ufx) * V00 + ufx * V01)
            + ufy * ((1.0f - ufx) * V10 + ufx * V11);

    // ─── BT.601 limited-range YUV → RGB ───
    // 参考: facealign.c:624-627 (板上已验证)
    float R = clamp(1.164f * (Y - 16.0f) + 1.596f * (V - 128.0f), 0.0f, 255.0f);
    float G = clamp(1.164f * (Y - 16.0f) - 0.813f * (V - 128.0f) - 0.391f * (U - 128.0f), 0.0f, 255.0f);
    float B = clamp(1.164f * (Y - 16.0f) + 2.018f * (U - 128.0f), 0.0f, 255.0f);

    // ─── SCRFD 归一化: (pixel - 127.5) / 128.0 ───
    // 参考: face_preprocess.cu:65-67, CLAUDE.md SCRFD 规格
    output[0 * plane_size + idx] = (R - 127.5f) / 128.0f;
    output[1 * plane_size + idx] = (G - 127.5f) / 128.0f;
    output[2 * plane_size + idx] = (B - 127.5f) / 128.0f;
}
