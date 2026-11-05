// emulator-lift · 数学适配的 **pure** 夹具（CTest: math；`ctest -R math` 可单独跑）
//
// 判据（任务 A「数学迁移到 GLM」）：
//   ① D3DX 行主序/行向量 ↔ GLM 列主序/列向量的**乘法顺序**：
//      D3DX 的 `A*B`（先 A 后 B）在 GLM 里必须写成 `B*A`；写反了下面的 (12,26,42) 会变 (22,66,132)。
//   ② 左手投影 / 视图：PerspectiveFovLH 的 D3D 深度 [0,1] 映射（zn⇒0、zf⇒1）与 LookAtLH 的
//      "eye 变原点、at 变 +z" 必须成立。
//   ③ 零向量归一化：返回长度 0、输出 (0,0,0)（⛔ 不是 NaN / 1）。
//
// ★ 这些断言**会红**：把 multiplyD3D 改成 `a*b` ⇒ ① 红；把 perspective 换成 GLM 默认的 NO 深度
//   （[-1,1]）⇒ ② 的 zn⇒0 红；把 normalize3D3D 的零向量分支删掉 ⇒ ③ 红（除零出 NaN）。
#include <array>
#include <cmath>
#include <cstdint>

#include "check.h"
#include "runtime/math.h"

namespace {

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }
bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

// D3DXMATRIX 的 guest 布局 = 16 个 float（行主序）；这里用宿主数组模拟同一份布局。
float d3dOf(const lift::math::Mat4& m, int r, int c) {
    float d3d[16];
    lift::math::toD3DLayout(m, d3d);
    return d3d[r * 4 + c];
}

// 把 float 矩阵转成 double 数组（内部 T&L 的精度口径）；返回的数组在整条表达式内有效。
std::array<double, 16> d3dDouble(const lift::math::Mat4& m) {
    float d[16];
    lift::math::toD3DLayout(m, d);
    std::array<double, 16> out{};
    for (int i = 0; i < 16; ++i) out[static_cast<std::size_t>(i)] = static_cast<double>(d[i]);
    return out;
}

// ---------------------------------------------------------------------------------------
// ① 行/列主序的乘法顺序
// ---------------------------------------------------------------------------------------
void caseMultiplyOrder() {
    lift_test::g_current_case = "math/multiply-order";

    // D3DX 行向量：先 S 后 T ⇒ v' = v * S * T；平移**不**被缩放。
    const lift::math::Mat4 s = lift::math::scaling(2.0f, 3.0f, 4.0f);
    const lift::math::Mat4 t = lift::math::translation(10.0f, 20.0f, 30.0f);
    const lift::math::Mat4 st = lift::math::multiplyD3D(s, t);

    // 平铺布局也必须是 D3DX 语义（row3 = 平移，不受 scale 影响）
    LIFT_CHECK(near(d3dOf(st, 0, 0), 2.0f));
    LIFT_CHECK(near(d3dOf(st, 1, 1), 3.0f));
    LIFT_CHECK(near(d3dOf(st, 2, 2), 4.0f));
    LIFT_CHECK(near(d3dOf(st, 3, 0), 10.0f));
    LIFT_CHECK(near(d3dOf(st, 3, 1), 20.0f));
    LIFT_CHECK(near(d3dOf(st, 3, 2), 30.0f));
    LIFT_CHECK(near(d3dOf(st, 3, 3), 1.0f));

    const lift::math::Vec4 v(1.0f, 2.0f, 3.0f, 1.0f);
    const lift::math::Vec4 got = lift::math::transformVec4D3D(st, v);
    LIFT_CHECK(near(got.x, 12.0f));  // 2*1 + 10
    LIFT_CHECK(near(got.y, 26.0f));  // 3*2 + 20
    LIFT_CHECK(near(got.z, 42.0f));  // 4*3 + 30
    LIFT_CHECK(near(got.w, 1.0f));

    // ★ 守卫：如果误写成 GLM 的 `a * b`（不反转顺序），得到的是 D3DX 的 T*S ⇒ (22,66,132)。
    //   这条断言让"不小心把 multiplyD3D 改成 a*b"当场红。
    const lift::math::Vec4 wrong_order =
        lift::math::transformVec4D3D(s * t, v);  // 故意绕开 multiplyD3D
    LIFT_CHECK(near(wrong_order.x, 22.0f));
    LIFT_CHECK(!near(wrong_order.x, got.x));

    // 适配器往返：任意 16 个 float 经过 from→to 必须**逐字节**回到原布局
    float probe[16];
    for (int i = 0; i < 16; ++i) probe[i] = static_cast<float>(i) * 1.25f - 7.0f;
    float back[16];
    lift::math::toD3DLayout(lift::math::fromD3DLayout(probe), back);
    for (int i = 0; i < 16; ++i) LIFT_CHECK(back[i] == probe[i]);

    // ★ 单位阵：D3DX 布局必须是 1/0（这是 d3d9_pipeline 的 writeIdentity 与 d3dx9_sprite 的
    //   writeIdentityMatrix 的**同一个**来源；改成别的布局/顺序时这里红，而不是靠人眼审计两份手写）
    float id[16];
    lift::math::toD3DLayout(lift::math::identity<float>(), id);
    for (int i = 0; i < 16; ++i) LIFT_CHECK(id[i] == ((i % 5 == 0) ? 1.0f : 0.0f));
}

// ---------------------------------------------------------------------------------------
// ② 左手投影 / 视图
// ---------------------------------------------------------------------------------------
void caseLeftHandedCamera() {
    lift_test::g_current_case = "math/lh-camera";

    const double h = 1.0 / std::tan(0.5);
    const lift::math::Mat4 p = lift::math::perspectiveFovLH(1.0f, 2.0f, 1.0f, 1001.0f);
    LIFT_CHECK(near(d3dOf(p, 0, 0), static_cast<float>(h / 2.0), 1e-5f));
    LIFT_CHECK(near(d3dOf(p, 1, 1), static_cast<float>(h), 1e-5f));
    LIFT_CHECK(near(d3dOf(p, 2, 2), 1001.0f / 1000.0f, 1e-5f));
    LIFT_CHECK(near(d3dOf(p, 2, 3), 1.0f));  // D3DX 布局 m[11] = 1
    LIFT_CHECK(near(d3dOf(p, 3, 2), -1001.0f / 1000.0f, 1e-5f));
    LIFT_CHECK(near(d3dOf(p, 3, 3), 0.0f));

    // D3D 的深度范围是 [0,1]：zn ⇒ 0、zf ⇒ 1（⛔ 不是 OpenGL 的 [-1,1]）
    const std::array<double, 16> pd_raw =
        d3dDouble(lift::math::perspectiveFovLH(1.0f, 2.0f, 1.0f, 1001.0f));
    const lift::math::Mat4d pd = lift::math::fromD3DLayout(pd_raw.data());
    auto clipZ = [&](double z) {
        const lift::math::Vec4d c = lift::math::transformVec4D3D(pd, lift::math::Vec4d(0.0, 0.0, z, 1.0));
        return c.z / c.w;
    };
    // 矩阵是 float（D3DXMATRIX）⇒ 用 1e-6 的容差；语义是"zn 映到 0、zf 映到 1"
    LIFT_CHECK(near(clipZ(1.0), 0.0, 1e-6));
    LIFT_CHECK(near(clipZ(1001.0), 1.0, 1e-6));

    // LookAtLH：eye=(0,0,-10) at 原点 up=+y ⇒ 旋转为单位阵、平移 z=+10
    const float eye[3] = {0.0f, 0.0f, -10.0f};
    const float at[3] = {0.0f, 0.0f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const lift::math::Mat4 view = lift::math::lookAtLH(eye, at, up);
    LIFT_CHECK(near(d3dOf(view, 0, 0), 1.0f));
    LIFT_CHECK(near(d3dOf(view, 1, 1), 1.0f));
    LIFT_CHECK(near(d3dOf(view, 2, 2), 1.0f));
    LIFT_CHECK(near(d3dOf(view, 3, 2), 10.0f));
    LIFT_CHECK(near(d3dOf(view, 3, 0), 0.0f));
    LIFT_CHECK(near(d3dOf(view, 3, 1), 0.0f));

    // 视图变换：eye ⇒ 原点；at ⇒ (0,0,+10)（左手 +z 朝前）
    const std::array<double, 16> view_raw = d3dDouble(view);
    const lift::math::Mat4d viewd = lift::math::fromD3DLayout(view_raw.data());
    const lift::math::Vec3d eye_v =
        lift::math::transformPoint3D3D(viewd, lift::math::Vec3d(0.0, 0.0, -10.0));
    LIFT_CHECK(near(eye_v.x, 0.0));
    LIFT_CHECK(near(eye_v.y, 0.0));
    LIFT_CHECK(near(eye_v.z, 0.0));
    const lift::math::Vec3d at_v = lift::math::transformPoint3D3D(viewd, lift::math::Vec3d(0.0, 0.0, 0.0));
    LIFT_CHECK(near(at_v.z, 10.0));

    // 正交：2/w、2/h、1/(zf-zn)、-zn/(zf-zn)
    const lift::math::Mat4 o = lift::math::orthoLH(2.0f, 4.0f, 1.0f, 101.0f);
    LIFT_CHECK(near(d3dOf(o, 0, 0), 1.0f));
    LIFT_CHECK(near(d3dOf(o, 1, 1), 0.5f));
    LIFT_CHECK(near(d3dOf(o, 2, 2), 0.01f, 1e-7f));
    LIFT_CHECK(near(d3dOf(o, 3, 2), -0.01f, 1e-7f));
}

// ---------------------------------------------------------------------------------------
// ③ 零向量归一化
// ---------------------------------------------------------------------------------------
void caseZeroNormalize() {
    lift_test::g_current_case = "math/zero-normalize";

    lift::math::Vec3d out(1.0, 2.0, 3.0);
    const double zero_len = lift::math::normalize3D3D(lift::math::Vec3d(0.0), &out);
    LIFT_CHECK(zero_len == 0.0);
    LIFT_CHECK(out.x == 0.0 && out.y == 0.0 && out.z == 0.0);  // ⛔ 不是 NaN

    const double len = lift::math::normalize3D3D(lift::math::Vec3d(3.0, 4.0, 0.0), &out);
    LIFT_CHECK(near(len, 5.0));
    LIFT_CHECK(near(out.x, 0.6));
    LIFT_CHECK(near(out.y, 0.8));
    LIFT_CHECK(near(out.z, 0.0));

    // float 便捷版（D3DXVec3Normalize 用的就是它）
    const float in[3] = {0.0f, 0.0f, 0.0f};
    float o[3] = {9.0f, 9.0f, 9.0f};
    LIFT_CHECK(lift::math::normalize3(in, o) == 0.0f);
    LIFT_CHECK(o[0] == 0.0f && o[1] == 0.0f && o[2] == 0.0f);

    const float in2[3] = {3.0f, 4.0f, 0.0f};
    LIFT_CHECK(near(lift::math::normalize3(in2, o), 5.0f));
    LIFT_CHECK(near(o[0], 0.6f, 1e-6f));
    LIFT_CHECK(near(o[1], 0.8f, 1e-6f));
}

}  // namespace

int main() {
    using namespace lift_test;
    caseMultiplyOrder();
    caseLeftHandedCamera();
    caseZeroNormalize();
    return finish("math");
}
