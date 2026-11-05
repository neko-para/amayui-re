// emulator-lift · 数学适配的实现（D3DX ↔ GLM；约定见 runtime/math.h 文件头）
//
// ★ 精度口径：这些生成器**沿用既有实现**的 double 中间量（D3DX 的 float 输出不变），
//   只是把"行向量展开"换成 GLM 的列向量算子 ⇒ 既有手算断言的期望值不需要改。
#include "runtime/math.h"

#include <cmath>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace lift {
namespace math {
namespace {

// glm::dmat4 → glm::mat4：逐元素窄化（⛔ 不用 reinterpret/memcpy 掩盖精度语义）
Mat4 toFloat(const Mat4d& m) {
    Mat4 out;
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) out[col][row] = static_cast<float>(m[col][row]);
    return out;
}

Mat4d identityD() { return Mat4d(1.0); }

}  // namespace

Mat4 scaling(float sx, float sy, float sz) {
    Mat4d m = identityD();
    m[0][0] = static_cast<double>(sx);
    m[1][1] = static_cast<double>(sy);
    m[2][2] = static_cast<double>(sz);
    return toFloat(m);
}

Mat4 translation(float x, float y, float z) {
    Mat4d m = identityD();
    // GLM 的平移在**第 3 列**（D3DX 行向量是第 3 行；两者互为转置，flat 相同）
    m[3][0] = static_cast<double>(x);
    m[3][1] = static_cast<double>(y);
    m[3][2] = static_cast<double>(z);
    return toFloat(m);
}

Mat4 rotationAxis(float x, float y, float z, float angle) {
    double ax = static_cast<double>(x);
    double ay = static_cast<double>(y);
    double az = static_cast<double>(z);
    const double len = std::sqrt(ax * ax + ay * ay + az * az);
    if (len == 0.0) {
        // 既有实现的零轴回退：轴保持 (0,0,0) ⇒ 对角只剩 cos（glm::rotate 会对零轴 normalize 出 NaN，
        // 所以这里显式保留原行为，⛔ 不静默改成单位阵）。
        const double c = std::cos(static_cast<double>(angle));
        Mat4d m = identityD();
        m[0][0] = c;
        m[1][1] = c;
        m[2][2] = c;
        return toFloat(m);
    }
    ax /= len;
    ay /= len;
    az /= len;
    // glm::rotate 的列向量矩阵 = D3DXMatrixRotationAxis 行向量矩阵的转置（逐项一致）
    return toFloat(glm::rotate(identityD(), static_cast<double>(angle), glm::dvec3(ax, ay, az)));
}

Mat4 rotationYawPitchRoll(float yaw, float pitch, float roll) {
    const Mat4d rz = glm::rotate(identityD(), static_cast<double>(roll), glm::dvec3(0, 0, 1));
    const Mat4d rx = glm::rotate(identityD(), static_cast<double>(pitch), glm::dvec3(1, 0, 0));
    const Mat4d ry = glm::rotate(identityD(), static_cast<double>(yaw), glm::dvec3(0, 1, 0));
    // D3DX 文档：roll → pitch → yaw；行向量下 out = Rz*Rx*Ry
    // GLM 侧（转置）out^T = Ry^T*Rx^T*Rz^T ⇒ 用 multiplyD3D 的顺序组合
    return toFloat(multiplyD3D(multiplyD3D(rz, rx), ry));
}

Mat4 perspectiveFovLH(float fovy, float aspect, float zn, float zf) {
    // D3D 的深度范围是 [0,1] ⇒ 明确取 _ZO 变体（⛔ 不是 GLM 默认的 OpenGL [-1,1]）。
    // glm::perspectiveLH_ZO 的矩阵正是 D3DXMatrixPerspectiveFovLH 的转置。
    const double n = static_cast<double>(zn);
    const double f = static_cast<double>(zf);
    if (aspect == 0.0f) {
        // glm::perspectiveLH_ZO 对 aspect=0 会 assert（Debug 下直接终止进程）；既有实现只是除零
        // （±inf/NaN）。这里显式保留后者的数值语义，⛔ 不让宿主 abort（真机也不会 assert）。
        Mat4d m(0.0);
        const double h = 1.0 / std::tan(static_cast<double>(fovy) / 2.0);
        m[0][0] = h / static_cast<double>(aspect);
        m[1][1] = h;
        m[2][2] = f / (f - n);
        m[2][3] = 1.0;
        m[3][2] = -(f * n) / (f - n);
        return toFloat(m);
    }
    return toFloat(glm::perspectiveLH_ZO(static_cast<double>(fovy), static_cast<double>(aspect), n, f));
}

Mat4 lookAtLH(const float eye[3], const float at[3], const float up[3]) {
    const glm::dvec3 e(eye[0], eye[1], eye[2]);
    const glm::dvec3 a(at[0], at[1], at[2]);
    const glm::dvec3 u(up[0], up[1], up[2]);
    if (e == a) {
        // 既有实现：at-eye 长度为 0 时按 len=1 处理 ⇒ 全部轴为 0、平移为 0 ⇒ 单位阵。
        // glm::lookAtLH 会 normalize(0) 出 NaN ⇒ 显式保留原行为。
        return toFloat(identityD());
    }
    const glm::dvec3 f = glm::normalize(a - e);
    if (glm::length(glm::cross(u, f)) == 0.0) {
        // 退化（up ∥ 视线）：既有实现让 x/y 轴为 0，只保留 z 轴与平移；glm::lookAtLH 会出 NaN。
        // ⛔ 这是 D3DX 语义未定义区，保留原行为而不是引入 NaN。
        Mat4d m = identityD();
        m[0][0] = 0.0;
        m[0][1] = 0.0;
        m[0][2] = f.x;
        m[1][0] = 0.0;
        m[1][1] = 0.0;
        m[1][2] = f.y;
        m[2][0] = 0.0;
        m[2][1] = 0.0;
        m[2][2] = f.z;
        m[3][0] = 0.0;
        m[3][1] = 0.0;
        m[3][2] = -glm::dot(f, e);
        return toFloat(m);
    }
    // glm::lookAtLH 的列向量矩阵 = D3DXMatrixLookAtLH 行向量矩阵的转置（逐项一致）
    return toFloat(glm::lookAtLH(e, a, u));
}

Mat4 orthoLH(float w, float h, float zn, float zf) {
    // 对称正交：left=-w/2, right=w/2, bottom=-h/2, top=h/2；_ZO = D3D 的 [0,1] 深度
    const double half_w = static_cast<double>(w) / 2.0;
    const double half_h = static_cast<double>(h) / 2.0;
    return toFloat(glm::orthoLH_ZO(-half_w, half_w, -half_h, half_h, static_cast<double>(zn),
                                   static_cast<double>(zf)));
}

float normalize3(const float in[3], float out[3]) {
    glm::vec3 v;
    const float length = normalize3D3D(glm::vec3(in[0], in[1], in[2]), &v);
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    return length;
}

}  // namespace math
}  // namespace lift
