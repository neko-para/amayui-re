# emulator-lift · sub_4CCC60 native override（Live2D 顶点仿射；debug 层）

- id: REQ-01M6XEXHKWBBN5P6ZG14DERW1G
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/native_override_assets.cpp#native-override-assets
- tags: [emulator-lift native-override live2d x87]

## 一句话

`sub_4CCC60`（EA **`0x4CCC60`**，`__stdcall retn 1Ch`，Live2D `BDAffine` 虚表第 6 项）是函数级 native override，
**已进 `all`（2026-11-11）**；它是本线第一项 x87 override，判据是**与解释器逐位相同**（`dst` + 寄存器 + `fpu.status/tag/top/control`）。

## 身份与契约（锚只有 EA）

- `.lst` `0x4CCC60..0x4CCD34`；7 个栈参 = 28 B，`[ebp+8]`（`cpu.arg(0)`）**全程未读**。
- 唯一引用点 = `.data:0052F0C0 dd offset sub_4CCC60`（vtable 第 6 项）；直接 `call` = 0。可核调用方 `sub_4CCD40`
  的三个 `call [[edi]+14h]`（`0x4CCD76` / `0x4CCDB6` / `0x4CCE22`）都在 x87 栈深 0。
- 语义：`o = [state+0x24] ?: [state+0x20]`；`θ = [o+0x14] * flt_52F1B0`；`r = [state+0x10]`；
  `sx/sy` 由 `[o+0x18]/[o+0x19]` 取 ±1；四系数 + `[o+4]/[o+8]` 平移；`stride==1` 时相邻点重叠（后写赢）；
  `EAX = dst + 4*(i0+1+N*stride)`。

## 实现

- 纯核 `affineCoeffs` / `affinePoint`（`native_affine.h`）+ 准入与循环 `nativeAffine4CCC60`（`native_affine.cpp`）。
- x87：SoftFloat `extF80` 四则 + `fsin`/`fcos` 走 `transcendental()` 同路径；`x87ApplyControlWord(cpu.fpu.control)`；
  出口补一次 `syncX87Status`。

## 进 `all` 时修的两处（2026-11-11）

1. **空循环**：`.lst 0x4CCCDE jge` 在 `0x4CCC80 fsin` / `0x4CCC84 fcos` / `0x4CCCC0..4CCCD0 fmulp` /
   `0x4CCCD2..4CCCDB` **之后** ⇒ guest 即使不进循环，也已置 `fpu.status` 的 PE/ES 并清 C0..C3。
   旧实现提前成功返回 ⇒ 改成 `return false`（交解释器重放）。真实调用方 `count=1, i0=0` ⇒ 该支产品路径走不到。
2. 回退零写：17 条 `return false` 全部在 guest 写之前（独立复审逐条核过）。

## 守卫

- assets `empty-loop-inexact` / `empty-loop-preset-cc`：`fpu.status` 逐位（修前红：`00A0 vs 0000` / `00A0 vs 4000`）。
- assets `rc/round-down` / `pc/53-bit`：控制字真的被应用（删 `x87ApplyControlWord` ⇒ 2 条红）。
- assets 原有：stride 1/2/3、`i0` 偏移、原地 alias、sx/sy 四种、r<0、`[state+0x24]` 分支、huge-angle / 非空 x87 栈两种回退。
- pure `affine/decline-no-write`：直接调 fn（`i0<0` / dst 跨度未提交 / x87 栈非空）⇒ 返回 false 且 `dst`／`status`／
  `tag`／`top` 与快照逐字节相同。

## 口径

- **帧级 / 逐像素 A/B 不是准入门**：函数级 override 在 dispatcher 里只记 1 条指令 + 1 tick ⇒ 跳过 N 条就少走
  `(N−1)×100ns` 虚拟时间（本项在 TITLE 实测 `Δinsns = 115,025` ≈ 11.5 ms）。准入五条见
  `apps/emulator-lift/src/runtime/native_overrides.h` 的契约段。
- 隔离：`all,-sub_4CCC60`。

## 残余

- `pc/53-bit` 这一维在本函数出口（f32 + 状态字）基本被 `fstp dword` 吃掉；用例只钉"控制字真的被应用"。
- `edi` 是否恒为 `BDAffine` **未取证**；只证到"唯一引用点是该 vtable 第 6 项"。
