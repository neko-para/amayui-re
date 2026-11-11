# 将 ftol 的 override 加入 override（__ftol2_sse → 函数级 native override，并入 all）

- id: REQ-01M75PW4MXS06MN6FKGXB36ETH
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/native_override.cpp#ftol/abi
- tags: [emulator-lift audio performance native-override]

## 目标（已达成）

把 `__ftol2_sse` 做成函数级 native override 并进 `--native-overrides all`。它在 `sub_48D610`（Ogg 解码）底下。

## 证据（本机 2026-11-12，`graphs/sn0000.graph` click1/click2，`--native-overrides all --audio off`）

`sub_48D610 -> __ftol2_sse` = **176.4 ms / 66 176 次**（click2：172.5 / 66 496）。
★ `--audio off` ⇒ 解码在**主线程**（pump 模式），profiler 与 `native-ovr:` 都读得到；**thread 模式**下 feeder
用自己的表拷贝 ⇒ 这两个读数都看不到（收口读数按 audio mode 分开写）。

## 取证结论（已按 `.lst` 复核）

1. `0x4E82C0` = `__ftol2_sse`（28 B，`.lst:381874`）；`0x4E82F6` = **`__ftol2`**（117 B，`.lst:381885`）
   ⇒ **挂 `0x4E82F6` 会 hits=0 却全绿**（反面判据）。
2. 本线执行 x87 那条：`0x4E82DC cmp ds:dword_55F2E8,0 / jz __ftol2`；flag 唯一写者 `0x4F1117`，
   本线 `IsProcessorFeaturePresent` shim 恒 0 ⇒ **flag 置位的一侧必须 decline**（真机走 `cvttsd2si`，越界语义不同）。
3. 语义（由指令推出，非文档）：返回 `EDX:EAX` 的 64-bit 截断；NaN / ±Inf / |x|≥2^63 ⇒ `EAX=0, EDX=0x80000000`；
   2^31..2^63 ⇒ 低 32 位回绕（不 clamp）；`0x4E8301` 有一次 `fst dword`（32 B scratch）⇒ 会置 PE/OE。

## 落地（见 `docs/03-emulator/design-notes.md` 的 `L3-ftol-01`）

- `nativeFtol2Sse` 落 `src/runtime/native_crt.cpp`：`x87ToI64(x, trunc=true)` + **一次** `x87RegToMem(x, scratch, 4)`
  （同时给出 PE/UE/OE 与 guest `[E+18h,E+1Ch)` 那 4 字节）+ 弹 1 层 + tag/status（⛔ 不清 C0..C3）。
- **闸门四条**：`[0x55F2E8] == 0` 且已提交 · `empty(phys(0))`（不是物理槽 0）· `8-used ≥ 2`（空闲槽）· scratch 已提交。
- **第 5 条闸门（本单新增，已裁决保留）**：`x ∈ (2^63−1, 2^63)` ⇒ **decline**。
  理由：guest 的 `fistp` 只在**取整值**放不进 int64 时给不定值，而 trunc 的判据是**截断值**放不进；
  该区间唯一可表示点是 `2^63−0.5` ⇒ guest 给 `0/0x80000000` + IE，trunc 给 `0xFFFFFFFF/0x7FFFFFFF`。
  不闸就是已知不一致；闸上的代价只有 6 行 + 2 条用例（`ftol/boundary-2^63-minus-half` decline、
  `2^63−1` 必须接管）。
- 契约：`native_overrides.h` 改成"按 ABI 写返回值（`Cdecl` 下可写 `EDX:EAX`，且必须在所有闸门之后）"；
  已核全仓只有 `native_block.cpp` 写 EDX/ECX（段级、契约本来就要求写）。
- `kMax 15→16`。

## 已知不覆盖（登记，不修）

- `0x4B5545 jmp __ftol2_sse`（`sub_4B5500` 的 qsort 比较器，尾跳）：`machine.cpp` 只拦 call 目标 ⇒
  **既不 +hits 也不 +fallbacks**。⇒ "hits 与 385 站点吻合"只是**下界**（实为 386 个入口）。
- `0x4BC539` / `0x4BC680` 直连 `__ftol2`（真消费 `EDX:EAX`）：不经 `0x4E82C0`，天然不覆盖。

## 判据（红了意味着什么）

- **差分臂**（`tests/native_override_assets.cpp` 的 `ftol/*`）：解释器 vs native 逐值比 30 格边界表
  （`±1.5 / ±3.5 / ±2.7 / ±0.5 / ±0.6 / 1.3 / 1e39 / 2^31± / 3e9 / 2^53 / 2^62 / 2^63 / −2^63 / ±Inf / NaN`
  + 三格 10 字节 ext80）+ `fpu.status` 逐位 + scratch 4 B + `top≠0` + 入口 CC 保留。
- **pure**（`tests/native_override.cpp`）：`ftol/decline-no-write`（6 个 decline 桶的零写冻结快照）·
  `ftol/abi`（`EDX:EAX` + EIP/ESP）· `ftol/stack-balance` · known-table 断言。
- **隔离臂**：`tests/loader.cpp`（用已有 `LIFT_AGE_LST`）：`all` 装得进 `0x4E82C0`、`all,-__ftol2_sse` 排得掉
  且余项仍在。⚠ 它在 `loader` 这个 ctest 里，`-R "native"` 过滤不到它；要进过滤得给 `native-override*`
  加 `LIFT_AGE_LST`（会动 CMake）⇒ **本轮不动**，全量 `ctest` 已覆盖。
- **变异（6 条实测红，均已还原）**：截断→就近 · 漏 f32 · 忘弹栈 · 抄 `native_affine` 的 `syncX87Status`（清 C0..C3）·
  越界给 `0x80000000` · 名字挂成 `__ftol2`。

## 测不出来的（文档承诺，不假装有守卫）

- **CW/RC 用例**：修正环把 RC 中和 ⇒ 删 `x87ApplyControlWord` 红不了。
- **"宽档 vs 窄档"**：无观察面（本实现不做窄档）。
- **scratch 另两段** `[E,E+4)` / `[E+10h,E+18h)` 与已消费槽残留字节：**不复刻**，差分只比 `[E+18h,E+1Ch)`，
  并断言另两段在 native 臂仍是哨兵 ⇒ 登记为已知偏差。
- **`fpu.status` 的判据是"与解释器逐位一致"，不是与真机**（解释器 `fistp` 不置 PE、`fld/fild` 不置 C1）。
- **时钟 k 不补**：thread 模式 `clock->tick` 是 no-op；pump 模式设备与 guest 计时同源 ⇒ 跳 tick 是一致加速。
