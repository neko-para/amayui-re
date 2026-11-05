// emulator-lift · **D3DX ↔ GLM 的唯一适配点**（M2 · 数学层）
//
// ★ 两套约定（这是本文件存在的原因，⛔ 不要把两者混着用）：
//   · D3DX（guest 里的 `D3DXMATRIX`）：16 个 float，**行主序存储**，**行向量**约定 `v' = v * M`。
//   · GLM（`glm::mat4` / `glm::dmat4`）：**列主序存储**，**列向量**约定 `v' = M * v`。
//
// ★ 适配规则（写死在这里，其它文件只许调这里的 API）：
//   ① 布局：同一组数按各自布局读入时，**flat 索引恰好相同**（行主序×行向量 ≡ 列主序×列向量）。
//      但这不是"可以直接 memcpy 互换"的理由 —— 真正的差别在下面两条运算语义：
//   ② 乘法：D3DX 的 `A * B`（`v' = v*A*B`，先 A 后 B）在 GLM 里是 `B^T * A^T`
//      ⇒ `multiplyD3D(a, b)` 返回 **`b * a`**（顺序反转，⛔ 不是 `a * b`）。
//   ③ 变换：D3DX 的 `v * M` 在 GLM 里是 `M^T * v` ⇒ `transformVec4D3D(m, v)` 返回 **`m * v`**。
//   所以 `fromD3DLayout` / `toD3DLayout` 虽然按 flat 下标走，语义上是**转置**：把 D3DX 的行
//   当作 GLM 的列。⛔ 读代码时别把 flat 相同误当成"不需要适配"。
//
// ★ guest ABI：`D3DXMATRIX` 永远是 guest 内存里的 16 个 float（64 B）。本文件只在宿主侧
//   转成 GLM 类型，⛔ 不把 `glm::mat4` 写进 guest 结构、也不改变 shim 的签名。
#pragma once

#include <cstddef>

#include <glm/glm.hpp>

namespace lift {
namespace math {

using Mat4 = glm::mat4;    // float：D3DX 输出的换算（guest 矩阵按 float 存）
using Mat4d = glm::dmat4;  // double：内部 T&L / 光栅（保持既有中间精度）
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using Vec3d = glm::dvec3;
using Vec4d = glm::dvec4;

template <typename T>
using Mat4T = glm::tmat4x4<T, glm::defaultp>;
template <typename T>
using Vec3T = glm::tvec3<T, glm::defaultp>;
template <typename T>
using Vec4T = glm::tvec4<T, glm::defaultp>;

static_assert(sizeof(Mat4) == 16 * sizeof(float), "glm::mat4 必须是 16 个 float");

// 单位阵（GLM 的默认构造**不**初始化 ⇒ 所有生成器都显式写这一行）
// ★ 这是 D3DX 单位阵的唯一来源：`d3d9_pipeline` 的 writeIdentity 与 `d3dx9_sprite` 的
//   writeIdentityMatrix 都走它（⛔ 不再各写一遍 `i % 5`）。
template <typename T>
inline Mat4T<T> identity() {
    return Mat4T<T>(T(1));
}

// D3DX 行主序 / 行向量 → GLM 列主序 / 列向量（见文件头 ①②）
template <typename T>
inline Mat4T<T> fromD3DLayout(const T d3d[16]) {
    Mat4T<T> out;
    // 数学上是 out = M^T；存储上两边的 flat 下标相同（见文件头 ①）。
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) out[col][row] = d3d[col * 4 + row];
    return out;
}

// GLM → D3DX（`fromD3DLayout` 的逆；存储上同样是 flat 相同）
template <typename T>
inline void toD3DLayout(const Mat4T<T>& m, T d3d[16]) {
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) d3d[row * 4 + col] = m[row][col];
}

// D3DX 语义的矩阵乘（见文件头 ②：GLM 里顺序反转）
template <typename T>
inline Mat4T<T> multiplyD3D(const Mat4T<T>& a, const Mat4T<T>& b) {
    return b * a;
}

// D3DX 语义的 4D 向量变换（`v' = v * M` ⇒ GLM 的 `M^T * v`）
template <typename T>
inline Vec4T<T> transformVec4D3D(const Mat4T<T>& m, const Vec4T<T>& v) {
    return m * v;
}

// 3D 点（w=1）/ 3D 方向（w=0）：内部 T&L 与 Sprite 都只走这两个入口
template <typename T>
inline Vec3T<T> transformPoint3D3D(const Mat4T<T>& m, const Vec3T<T>& p) {
    return Vec3T<T>(m * Vec4T<T>(p, T(1)));
}

template <typename T>
inline Vec3T<T> transformVector3D3D(const Mat4T<T>& m, const Vec3T<T>& v) {
    return Vec3T<T>(m * Vec4T<T>(v, T(0)));
}

// 归一化：返回长度；零向量 ⇒ `*out = (0,0,0)`、返回 0（与既有 D3DXVec3Normalize / 流水线一致）
template <typename T>
inline T normalize3D3D(const Vec3T<T>& v, Vec3T<T>* out) {
    const T len = glm::length(v);
    if (len == T(0)) {
        *out = Vec3T<T>(T(0));
        return T(0);
    }
    *out = v / len;
    return len;
}

template <typename T>
inline Vec3T<T> normalized3D3D(const Vec3T<T>& v) {
    Vec3T<T> out;
    normalize3D3D(v, &out);
    return out;
}

// ---- 生成器（float：直接供 guest 的 D3DXMATRIX；内部用 double 中间量保持既有位级结果）----
Mat4 scaling(float sx, float sy, float sz);
Mat4 translation(float x, float y, float z);
Mat4 rotationAxis(float x, float y, float z, float angle);
Mat4 rotationYawPitchRoll(float yaw, float pitch, float roll);
Mat4 perspectiveFovLH(float fovy, float aspect, float zn, float zf);
Mat4 lookAtLH(const float eye[3], const float at[3], const float up[3]);
Mat4 orthoLH(float w, float h, float zn, float zf);

// 归一化便捷版（float；返回长度，零向量 ⇒ out=(0,0,0)、返回 0）
float normalize3(const float in[3], float out[3]);

}  // namespace math
}  // namespace lift
