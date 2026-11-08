// emulator-lift · native override `sub_4CCC60` 的纯核（Live2D `BDAffine` 逐顶点仿射）
//
// ★ 契约：**调用方必须先 `x87ApplyControlWord(...)`**（与 guest 的每条 FP 指令同口径）。
//   本文件的函数只做 x87 语义的算术；不读 guest、不写 guest。
// ★ 为什么单独一个头：`Ext80` / x87 原语只属于这一项；⛔ 不要把 fpu_bridge 拖进 `native_overrides.h`。
// ★ 逐位口径：四则 / 取号走 SoftFloat `extF80`；`fsin`/`fcos` 走
//   `x87ToDouble → std::sin/cos → x87FromDouble`（与 `exec_fpu.cpp` 的 `transcendental()` 同一条路径）。
//   ⛔ 不许用 `double` 重写"数学上等价"的版本：那会差最后一位 float，破坏逐字节 A/B。
#pragma once

#include <cstdint>

#include "runtime/fpu_bridge.h"

namespace lift {

// guest 的四个仿射系数（`.lst` 0x4CCCB0..0x4CCCD2 的 FPU 栈末态）：
//   A = sx*r*cosθ · B = -(r*sy*sinθ) · C = r*sy*cosθ · D = sx*r*sinθ
struct AffineCoeffs {
    Ext80 a{};
    Ext80 b{};
    Ext80 c{};
    Ext80 d{};
};

// 保守的**无副作用**范围判据：|angle * angle_scale| 是否远低于 2^63（fsin 的 C2 界）。
// ⛔ 只用宿主 double 做判定、不碰软浮点 / x87 状态字 ⇒ 返回 false 时可以直接回退解释器。
// ★ 阈值取 2^63/4（留一个数量级的余量）：真实角度远小于它；落在带内的极值只会**多回退**，不会算错。
bool affineAngleInRange(float angle, float angle_scale);

// 四系数。`flip_x` = `[obj+0x18] != 0`（⇒ sx=-1），`flip_y` 同理（`[obj+0x19]`）。
// 返回 false ⇒ |θ| >= 2^63（防御性；正常路径由 affineAngleInRange 先判掉）。
bool affineCoeffs(const Ext80 angle, const Ext80 angle_scale, const Ext80 radius, bool flip_x, bool flip_y,
                  AffineCoeffs* out);

// 单点：`out0 = (iy*b) + (ix*a) + add4`；`out1 = (iy*c) + (ix*d) + add8`。
// ★ 运算顺序逐字照 `.lst` 的 `fmul` / `fld st` / `faddp` / `fadd`（顺序影响最后一位）。
void affinePoint(const AffineCoeffs& k, const Ext80 ix, const Ext80 iy, const Ext80 add4, const Ext80 add8,
                 Ext80 out0, Ext80 out1);

}  // namespace lift
