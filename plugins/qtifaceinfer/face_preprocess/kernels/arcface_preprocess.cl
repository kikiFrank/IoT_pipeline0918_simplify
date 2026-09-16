/// @file arcface_preprocess.cl
/// @brief NV12 DMABuf → 逆 Umeyama 仿射 → BT.601 → p/127.5-1.0 → float32 NCHW 112×112
///
/// 移植自: face_preprocess.cu:71-117 (arcface_crop_kernel)
/// 复用与 scrfd 同一个 cl_mem (同帧同 fd)

__kernel void arcface_preprocess(
    __global const uchar* nv12_buf,   // 单个 DMABuf import (整帧)
    __global float* output,           // 3 * dst_size * dst_size NCHW
    uint y_offset,                    // Y 平面字节偏移
    uint uv_offset,                   // UV 平面字节偏移
    uint y_stride,                    // Y 平面行步长 (字节)
    uint uv_stride,                   // UV 平面行步长 (字节)
    uint src_w,                       // 源帧宽 (像素)
    uint src_h,                       // 源帧高 (像素)
    uint dst_size,                    // 目标尺寸 (112)
    float m00, float m01, float m02,  // 逆仿射矩阵第 0 行
    float m10, float m11, float m12)  // 逆仿射矩阵第 1 行
{
    int dx = get_global_id(0);
    int dy = get_global_id(1);
    if (dx >= (int)dst_size || dy >= (int)dst_size) return;

    uint plane_size = dst_size * dst_size;
    uint idx = dy * dst_size + dx;

    // ─── 逆仿射映射 dst → src ───
    // 参考: face_preprocess.cu:84-85, facealign.c:611-612
    float sx = m00 * (float)dx + m01 * (float)dy + m02;
    float sy = m10 * (float)dx + m11 * (float)dy + m12;

    // ─── 边界处理: 越界填充 (-1, -1, -1) ───
    // -1.0 = 0/127.5 - 1.0 (ArcFace 归一化下的黑色)
    // 参考: face_preprocess.cu:89 边界检查 + 114-116 归一化后 r=g=b=0 → -1.0
    if (sx < 0.0f || sx >= (float)(src_w - 1) || sy < 0.0f || sy >= (float)(src_h - 1)) {
        output[0 * plane_size + idx] = -1.0f;
        output[1 * plane_size + idx] = -1.0f;
        output[2 * plane_size + idx] = -1.0f;
        return;
    }

    // ─── Y 平面双线性插值 (与 scrfd 内核相同) ───
    int x0 = (int)floor(sx);
    int y0 = (int)floor(sy);
    int x1 = min(x0 + 1, (int)src_w - 1);
    int y1 = min(y0 + 1, (int)src_h - 1);
    float fx = sx - (float)x0;
    float fy = sy - (float)y0;

    float Y00 = (float)nv12_buf[y_offset + y0 * y_stride + x0];
    float Y01 = (float)nv12_buf[y_offset + y0 * y_stride + x1];
    float Y10 = (float)nv12_buf[y_offset + y1 * y_stride + x0];
    float Y11 = (float)nv12_buf[y_offset + y1 * y_stride + x1];

    float Y = (1.0f - fy) * ((1.0f - fx) * Y00 + fx * Y01)
            + fy * ((1.0f - fx) * Y10 + fx * Y11);

    // ─── UV 平面双线性插值 (NV12 chroma 2×2 子采样) ───
    float ux = sx * 0.5f;
    float uy = sy * 0.5f;
    int ux0 = (int)floor(ux);
    int uy0 = (int)floor(uy);
    int ux1 = min(ux0 + 1, (int)(src_w / 2) - 1);
    int uy1 = min(uy0 + 1, (int)(src_h / 2) - 1);
    ux0 = clamp(ux0, 0, (int)(src_w / 2) - 1);
    uy0 = clamp(uy0, 0, (int)(src_h / 2) - 1);
    float ufx = ux - floor(ux);
    float ufy = uy - floor(uy);

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
    float R = clamp(1.164f * (Y - 16.0f) + 1.596f * (V - 128.0f), 0.0f, 255.0f);
    float G = clamp(1.164f * (Y - 16.0f) - 0.813f * (V - 128.0f) - 0.391f * (U - 128.0f), 0.0f, 255.0f);
    float B = clamp(1.164f * (Y - 16.0f) + 2.018f * (U - 128.0f), 0.0f, 255.0f);

    // ─── ArcFace 归一化: pixel / 127.5 - 1.0 ───
    // 参考: face_preprocess.cu:114-116, facealign.c:632-634
    // 数学等价: (R-127.5)/127.5 == R/127.5 - 1.0
    output[0 * plane_size + idx] = R / 127.5f - 1.0f;
    output[1 * plane_size + idx] = G / 127.5f - 1.0f;
    output[2 * plane_size + idx] = B / 127.5f - 1.0f;
}
