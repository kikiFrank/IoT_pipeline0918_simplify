/// @file inverse_affine.hpp
/// @brief Umeyama 4-DOF 相似变换: 5 点 landmark → ArcFace 112×112 逆仿射矩阵
///
/// 算法移植自 test/Neel_cpp/trt_face.cpp:168-209
/// 输入: 检测到的 5 点 landmark (原图像素坐标, ScrfdDecode 输出)
/// 输出: 逆仿射矩阵 inv[6] = {m00, m01, m02, m10, m11, m12}
///       用于 FacePreprocess::RunArcface() 的 dst→src 坐标映射

#pragma once
#include <cmath>

namespace face_infer {

/// ArcFace 标准参考点 (112×112 坐标系)
/// 顺序: left eye, right eye, nose, left mouth, right mouth
inline constexpr float kArcfaceRefLandmarks[5][2] = {
    {38.2946f, 51.6963f},
    {73.5318f, 51.5014f},
    {56.0252f, 71.7366f},
    {41.5493f, 92.3655f},
    {70.7299f, 92.2041f}
};

/// 从检测 landmarks 计算到 ArcFace 参考点的逆仿射矩阵
/// @param src  检测到的 5 点 [5][2] (原图坐标)
/// @param inv  输出逆仿射矩阵 [6] = {m00,m01,m02, m10,m11,m12}
///             dst(dx,dy) → src: sx = m00*dx + m01*dy + m02
///                               sy = m10*dx + m11*dy + m12
inline void ComputeInverseAffine(const float src[5][2], float inv[6])
{
    // 质心
    double sx = 0, sy = 0, dx = 0, dy = 0;
    for (int i = 0; i < 5; i++) {
        sx += src[i][0]; sy += src[i][1];
        dx += kArcfaceRefLandmarks[i][0]; dy += kArcfaceRefLandmarks[i][1];
    }
    sx /= 5; sy /= 5; dx /= 5; dy /= 5;

    // 最小二乘 (Umeyama 4-DOF: a, b 为旋转+缩放参数)
    double num_a = 0, num_b = 0, denom = 0;
    for (int i = 0; i < 5; i++) {
        double sxi = src[i][0] - sx;
        double syi = src[i][1] - sy;
        double dxi = kArcfaceRefLandmarks[i][0] - dx;
        double dyi = kArcfaceRefLandmarks[i][1] - dy;
        num_a += dxi * sxi + dyi * syi;
        num_b += dxi * syi - dyi * sxi;
        denom += sxi * sxi + syi * syi;
    }

    // 退化保护: 所有 landmarks 重合时 denom=0, 退化为纯平移
    if (denom < 1e-10) {
        inv[0] = 1.0f; inv[1] = 0.0f; inv[2] = static_cast<float>(sx - dx);
        inv[3] = 0.0f; inv[4] = 1.0f; inv[5] = static_cast<float>(sy - dy);
        return;
    }

    double a = num_a / denom;
    double b = num_b / denom;
    double tx = dx - a * sx + b * sy;
    double ty = dy - b * sx - a * sy;

    // 正向: M = [a, -b, tx; b, a, ty]
    // 逆向: M_inv = (1/det) * [a, b, -(a*tx+b*ty); -b, a, (b*tx-a*ty)]
    double det = a * a + b * b;
    if (det < 1e-10) det = 1e-10;
    double inv_det = 1.0 / det;

    inv[0] = static_cast<float>(a * inv_det);
    inv[1] = static_cast<float>(b * inv_det);
    inv[2] = static_cast<float>(-(a * tx + b * ty) * inv_det);
    inv[3] = static_cast<float>(-b * inv_det);
    inv[4] = static_cast<float>(a * inv_det);
    inv[5] = static_cast<float>((b * tx - a * ty) * inv_det);
}

}  // namespace face_infer
