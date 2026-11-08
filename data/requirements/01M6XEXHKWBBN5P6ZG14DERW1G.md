# emulator-lift · sub_4CCC60 native override（Live2D 顶点仿射；debug 层）

- id: REQ-01M6XEXHKWBBN5P6ZG14DERW1G
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/native_override_assets.cpp#native-override-assets
- tags: [emulator-lift native-override live2d x87]

## 一句话

`sub_4CCC60`（EA **`0x4CCC60`**，Live2D `BDAffine` 虚表第 6 项）**已实现**为函数级 native override，
首版 **`debug = true`**（⛔ 不进 `all`；必须显式 `--native-overrides all,sub_4CCC60`）。
它是**纯算术叶子**（x87 `fsin`/`fcos` + 逐顶点仿射；无 guest 回调 / 无锁 / 无全局写）。
★ 本线**第一项 x87 override**：判据不是"看起来对"，而是与解释器**逐位相同**（见 G1）。

## 身份与契约

- `0x4CCC60` · `.text` · **`__stdcall` `retn 1Ch`** ⇒ 7 个栈参数 = 28 B；`cpu.arg(0)`（`[ebp+8]`）**全程未读**。
  实参 = `cpu.arg(1..6)` = state / src / dst / count / i0 / stride。
- 唯一调用证据 = `??_7BDAffine@live2d@@6B@`（@`0x52F0AC`）第 6 项（@`0x52F0C0`）；直接 `call` = 0。
- 语义（`.lst` `0x4CCC60..0x4CCD34`）：`o=[state+0x24] ?: [state+0x20]`；`θ=[o+0x14]*flt_52F1B0`；
  `r=[state+0x10]`；`sx=[o+0x18]==0?+1:-1`、`sy=[o+0x19]==0?+1:-1`；
  `for(i=i0; i<count*stride; i+=stride){ ix=src[i]; iy=src[i+1];
   dst[i]=iy*(-r*sy*sinθ)+ix*(r*sx*cosθ)+[o+4]; dst[i+1]=iy*(r*sy*cosθ)+ix*(r*sx*sinθ)+[o+8]; }`
  `EAX = dst+4*(i0+1+N*stride)`（进循环）否则 `o`；`stride==1` 时相邻两点**重叠写**（后写赢）。
- 唯一全局 `flt_52F1B0` 是**读**（工具按操作数位置记成"写"，是启发式）。

## 实现（2026-11-08）

- 落点：`native_affine.cpp`（准入 + 循环）+ `native_affine.h`（纯核）+ `native_overrides.h` 声明
  + `native_overrides.cpp` 的 `kKnown[]` 一行 + `CMakeLists.txt`。
- x87：四则/取号走 SoftFloat `extF80`；`fsin`/`fcos` 走 `exec_fpu.cpp` 的 `transcendental()` 同路径
  （`x87ToDouble → std::sin/cos → x87FromDouble`）。⛔ 不许用 `double` 重写。
- 状态字：末尾按 `syncStatus` 的映射补一次。C0..C3 由 `fsin`/`fcos` 清 0、之后无指令改；
  异常位 OR 可交换 ⇒ 与解释器"每条指令 sync"等价（G1 比 `fpu.status` 验证）。
- 准入（全部在**任何 FPU 副作用 / guest 写之前**）：state/obj/全局提交；**x87 栈必须为空**
  （guest 峰值深度 8，非空栈在 guest 里是 push 溢出路径）；`|θ| < 2^63`（宿主 double 保守判）；
  空循环早返回（`EAX=obj`）；非空循环要求 `stride>0 && i0>=0`；src/dst 访问区间提交。
- 出口：dst 逐点写（先读两个再写两个）；`EAX = dst+4*(last+stride+1)`（32 位回绕）。

## 守卫与结果

- **G1 assets 真字节差分**：`repo:apps/emulator-lift/tests/native_override_assets.cpp` —
  **505 checks / 0 fail**。比 dst 逐字节 + `EAX/ESP/EIP` + `EBX/ESI/EDI/EBP` + **`fpu.status/control/tag/top`**；
  覆盖 stride 1/2/3/4、i0 偏移、原地 alias、sx/sy 四种、r<0、空循环、`[state+0x24]` 分支、
  以及 huge-angle / 非空 x87 栈两种 **Declined**。
- **G2 pure 核**：`repo:apps/emulator-lift/tests/native_override.cpp` — **1218 checks / 0 fail**
  （θ=0 的四系数含**零的符号位** + 单点乘加顺序 + C2 区拒绝）。
- **分层**：`debug=true`；`all` 不含它（启动行 11 项 vs `all,sub_4CCC60` 12 项，已核）。
- **变异自检**（手工施加后恢复，哈希比对通过）：flip_x 反转 ⇒ 8 failures；sin/cos 对调 ⇒ 8；
  两个平移对调 ⇒ 8；去掉 `fchs` ⇒ 7。
- **G3 帧级 A/B 未机械化**：点 OPTION 的输入脚本命中不稳定（同一脚本多次运行有的进 OPTION、有的没进）⇒
  暂不作判据；与 `sub_46D9F0` 同口径登记为残余。⛔ G3 绿之前不进 `all`。

## 残余 / 下一步

1. **G3**：补一个与 boot 时序解耦的可重放 OPTION 输入，再比逐帧 hash（`all` vs `all,-sub_4CCC60`）。
2. 进 `all` 的条件：G3 逐帧一致 + `hits>0` / `fallbacks=0` + 至少一次真实 OPTION 场景。
3. 语法 / 语义仍以 `.lst` 为锚；⛔ 不把 `native_affine.h` 的纯核当"引擎规范"。
