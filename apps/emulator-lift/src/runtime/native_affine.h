// emulator-lift · native override `sub_4CCC60` 的纯核（Live2D `BDAffine` 逐顶点仿射）
//
// ★ **D49（x87 = double）之后本文件的形状**：值就是 `double`（`X87Val`），算术走 `fpu_bridge.h`
//   的同一份模型（`x87Mul` / `x87Add` / `x87Sin` / `x87Cos`）。⛔ 没有"宿主的 softfloat 全局
//   舍入模式"这一步了 ⇒ 调用方**不再**需要先 `x87ApplyControlWord(...)`（那个函数已删除）。
// ★ 为什么仍然走 `fpu_bridge` 而不是直接写 `a*b`：**异常位**（OE/IE）要按模型置位，而且
//   `x87Sin`/`x87Cos` 必须是解释器与生成代码的**同一份**（`fpu_bridge.cpp` 的唯一进出口）。
// ★ 逐位口径：两臂（本 override / 解释器）用的是同一份 double 模型 ⇒ 运算符顺序照抄即可逐位相同。
//   ⚠ D49 之前这里刻意用 `Ext80` 保 80 位；现在两边都是 53 位，那条理由随 D49 一起作废。
#pragma once

#include <cstdint>

#include "runtime/fpu_bridge.h"

namespace lift {

// guest 的四个仿射系数（`.lst` 0x4CCCB0..0x4CCCD2 的 FPU 栈末态）：
//   A = sx*r*cosθ · B = -(r*sy*sinθ) · C = r*sy*cosθ · D = sx*r*sinθ
struct AffineCoeffs {
    X87Val a = 0.0;
    X87Val b = 0.0;
    X87Val c = 0.0;
    X87Val d = 0.0;
};

// 保守的**无副作用**范围判据：|angle * angle_scale| 是否远低于 2^63（fsin 的 C2 界）。
// ⛔ 只用宿主 double 做判定、不碰 x87 状态字 ⇒ 返回 false 时可以直接回退解释器。
// ★ 阈值取 2^63/4（留一个数量级的余量）：真实角度远小于它；落在带内的极值只会**多回退**，不会算错。
bool affineAngleInRange(float angle, float angle_scale);

// 四系数。`flip_x` = `[obj+0x18] != 0`（⇒ sx=-1），`flip_y` 同理（`[obj+0x19]`）。
// 返回 false ⇒ |θ| >= 2^63（防御性；正常路径由 affineAngleInRange 先判掉）。
bool affineCoeffs(X87Val angle, X87Val angle_scale, X87Val radius, bool flip_x, bool flip_y,
                  AffineCoeffs* out);

// 单点：`out0 = (iy*b) + (ix*a) + add4`；`out1 = (iy*c) + (ix*d) + add8`。
// ★ 运算顺序逐字照 `.lst` 的 `fmul` / `fld st` / `faddp` / `fadd`（顺序影响最后一位）。
void affinePoint(const AffineCoeffs& k, X87Val ix, X87Val iy, X87Val add4, X87Val add8, X87Val* out0,
                 X87Val* out1);

}  // namespace lift
