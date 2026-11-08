# emulator-lift · sub_4CCC60 native override（Live2D 顶点仿射，已评估可行、未实现）

- id: REQ-01M6XEXHKWBBN5P6ZG14DERW1G
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift native-override live2d x87]

## 一句话

`sub_4CCC60`（EA **`0x4CCC60`**，Live2D `BDAffine` 虚表第 6 项）**能做**函数级 native override：
它是**纯算术叶子**（x87 `fsin`/`fcos` + 逐顶点仿射；无 guest 回调 / 无锁 / 无全局写）。
★ 本单只做**评估**（结论：可行，**尚未实现**）。★ 这是本线**第一项会碰 x87** 的 override
⇒ 判据不是"看起来对"，而是与解释器**逐位相同**。

## 身份与契约

- `0x4CCC60` · `.text` · **`__stdcall` `retn 1Ch`** ⇒ 7 个栈参数 = 28 B。`[ebp+8]`（第 1 个）**全程未读**；
  实参是 `[ebp+0Ch..0x20]`（`.lst` 只声明了 `arg_4..arg_18`）。
- 唯一调用证据 = `??_7BDAffine@live2d@@6B@`（@`0x52F0AC`）的**第 6 项**（表项 @`0x52F0C0`）；直接 `call` = 0。
- 参数：`p1=[ebp+0Ch]` state · `p2=[ebp+10h]` src(float*) · `p3=[ebp+14h]` dst(float*) ·
  `p4=[ebp+18h]` count · `p5=[ebp+1Ch]` i0 · `p6=[ebp+20h]` stride（下标步长）。
- 语义（`.lst` `0x4CCC60..0x4CCD34` 逐条）：`o = [state+0x24] ?: [state+0x20]`；
  `θ=[o+0x14]*flt_52F1B0`；`r=[state+0x10]`；`sx=[o+0x18]==0 ? +1 : -1`；`sy=[o+0x19]==0 ? +1 : -1`；
  `for (i=p5; i<p4*p6; i+=p6) { ix=src[i]; iy=src[i+1];
   dst[i]   = iy*(-r*sy*sinθ) + ix*( r*sx*cosθ) + [o+4];
   dst[i+1] = iy*( r*sy*cosθ) + ix*( r*sx*sinθ) + [o+8]; }`
  返回 `EAX = dst + 4*(i0+1+N*stride)`（进循环时；`N` = 迭代数），否则 `o`。
- 净 x87 深度 0；唯一全局 `flt_52F1B0`（`.lst` `fmul flt_52F1B0` ⇒ **读**；工具的 globals 按操作数
  位置记为"写"，那是启发式，不是数据流）。

## 可行性判据（逐条，全过）

- **拦截**：函数级 override 只拦"上一条是 call 且 eip==目标"；dispatcher 对**任意** `Op::Call` 都取
  `next_call_target = cpu_.eip`（含 `call [eax+14h]`）⇒ 虚表分派**能**被拦。先例：`sub_46D9F0`。
- **ABI**：stdcall(28B)，只有 `EAX` 返回；`EBX/ESI/EDI` 由 guest 自己保存 ⇒ 宿主不碰。
- **纯度**：只读 state/o、只写 dst；0 回调 / 0 锁 / 0 shim / 0 全局写。
- **x87 可复现**（本单关键）：本线 x87 = SoftFloat `extF80`（64 位尾数）；`fsin`/`fcos` 走
  `exec_fpu.cpp` 的 `transcendental()`（`x87ToDouble` → `std::sin/cos` → `x87FromDouble`）。
  override 按 guest 的**指令顺序**用 `fpu_bridge.h` 的同一组原语
  （`x87MemToReg` / `x87Mul` / `x87Add` / `x87ChangeSign` / `x87RegToMem`）即可**逐位相同**。
  ⛔ 用 `double` 重写"数学上等价"会差最后一位 float ⇒ 破坏逐字节 A/B。
- **退化分支**：`stride<=0`、`|θ|>=2^63`（fsin 的 C2 路径）、`i` 越过 int32 ⇒ Declined（让解释器复现）。
- **未决偏差**：x87 **状态字**（PE / C1）。解释器每个 FP 指令都 `syncStatus` ⇒ 末态 PE 几乎必然为 1、
  C1 = 最后一次 `fstp dword` 的舍入方向。实现时要么给 bridge 加一个 sync 入口（推荐），
  要么显式登记为"状态字不建模"（⛔ 不许装作没有）。

## 收益（profile 观测，非入库锚）

`.tmp/after-option.out`（OPTION 的 Live2D 帧，`LIFT_FRAME_TRACE=1`）：322 帧中 **321 帧是本函数第一**；
均值 **2.42 ms/帧**、占 guest **21.6%**（guest 均值 11.22 ms、墙钟均值 39.0 ms）⇒ 估省 ~2 ms/帧（≈5% 墙钟）。
★ 同一帧里 guest 只占 ~11 ms、shim/other 占 ~28 ms ⇒ **不是帧时间的主杠杆**；本单要的是少掉一段已证可行的 guest 时间。
★ 指令数：每顶点约 13 条 guest 指令；override 一次调用只计 1 条 ⇒ 虚拟时钟会少走（README §4.3 已声明不补偿）。

## 守卫草案（实现时落地）

- **G1 assets 差分（主判据）**：沿用 `tests/native_override_assets.cpp` 的现成套路 —— 从 AGE.EXE 取
  `0x4CCC60` 的**真字节**（0xD4 = 212 B，含 `retn 1Ch`）放回同一 EA，合成 state/o/src/dst；
  解释器 vs override 比：dst 逐字节 · `EAX/ESP/EIP` · `EBX/ESI/EDI` · **x87 ST0..ST7 与状态字**。
  矩阵：sx×sy × θ（0 / ±小 / ±π/2 / 大） × stride(1/2/4) × i0(0/中) × 空循环/整段。
- **G2 pure 核**：把 FPU 序列抽成"给定 Ext80 输入 → 4 个系数 + dst"的纯核（仍走 fpu_bridge）；
  与 G1 共用同一份代码 ⇒ 防"测试抄实现"。
- **G3 帧级 A/B**：`--native-overrides all` vs `all,-sub_4CCC60` 同虚拟时钟逐帧 hash 相同 + `hits>0`。
  现在缺可重放的 OPTION 输入（`click-option.txt` 只到 TITLE）⇒ G3 前先补输入脚本。
- **变异**（手工施加后恢复）：sx/sy 的 ±1 反转 · sin/cos 对调 · `+[o+4]`/`+[o+8]` 对调 · 去掉 `fchs`
  ⇒ 每条都必须当场红。

## 落点

- 新文件 `apps/emulator-lift/src/runtime/native_affine.cpp`（2026-11-08 的 `native_*.cpp` 拆分就是为它留的位置）
  + `repo:apps/emulator-lift/src/runtime/native_overrides.h` 的声明 + `repo:apps/emulator-lift/src/runtime/native_overrides.cpp` 的 `kKnown[]` 一行。
- G1：`repo:apps/emulator-lift/tests/native_override_assets.cpp`；G3 的 A/B 范本是
  `repo:apps/emulator-lift/tests/movie_logo.cpp`。
- 首版按 `debug = true`（显式点名，**不进** `all`）；G3 绿了再讨论进 `all`。

## 与已有单的关系

- 族父 = `REQ-01M6H6G6DSRG1ZRD810Z0HTH8V`；可行性试点 `REQ-01M6NZSAAD0WCNBE3RKMFF84R5`；
  x87 的 1 ulp 登记见 `repo:apps/emulator-lift/src/runtime/exec_fpu.cpp` 文件头（本单不改它）。
