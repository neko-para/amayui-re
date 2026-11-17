# CTX · 交接（2026-11-17 晚 · **macOS 单树**）

> 这是**交接快照**，不是口径文档：口径看 `AGENTS.md`（§12 实现 · §15 派活）· `apps/emulator-lift/README.md` · `docs/03-emulator/`。
> **进度真源**仍是 `pnpm tools requirements plan` —— ⛔ 不在这里抄数字（本文件只写"现在在哪、要你决定什么、下一步从哪起"）。
> 设备范围：**macOS 只有一棵树**（`AGENTS.md` §14 的双 worktree 只在 win32 成立）。

---

## A. 一句话现状

**本轮三件 + GPU 路线 P0–P3 全部落地**（每件都有守卫或读数，工作树干净）：

1. **TITLE→Quit 不再停在 `0x513B90`**：根因是 D26 只扫 4 字节对齐 dword，漏了指令流里的**未对齐 imm32**
   （`0x412295 的 push offset SEH_412290`、下一跳 `0x413562 的 mov eax, imm32`）⇒ 补扫后 guest 自己走完
   `catch(Command_Exit_Exception) → SendMessageA(WM_CLOSE) → ExitProcess`，宿主 `exit 0`、`table misses=0`。
2. **AOT 音频从"恒 D 泵"改成真 feeder 线程**：`pushed - played` **115200 B（600.0 ms）→ 19200 B（100.0 ms）**（4/4 精确同值）；
   `calls=0`、`underruns=0`；TEB 自有（BUG-22 不复发）。
3. **SC0000 转场**：接口复审证明"不是接口问题"；真因 = **caps 分路**（`PixelShaderVersion` 决定引擎走软件模糊还是 FX）。
   GPU 路线 **P0–P3** 落地后，**用户实测：`LIFT_D3D9_PS_VERSION=0x200` 下与真机一致**，剩下的黑 = 202（Snow）未实现。

读数（macOS arm64 / `macos-arm64` = RelWithDebInfo）：`ctest` **107 项（103 passed + 4 平台 skip）** ·
`pnpm test` 574 · `pnpm verify` 502 · `requirements validate` 6/6 · `ledger validate` 8/8 · 工作树干净。

---

## B. 现在最要紧的一件事（用户口径，2026-11-17）

**不再考虑 `PixelShaderVersion = 0` 的配置。** ⇒ 下一单 **P4**（`REQ-01M7MQ…` 见 C）：
取消 `LIFT_D3D9_PS_VERSION` 开关，`D3DCAPS9.PixelShaderVersion` **固定报 `0xFFFF0200`**，
**删掉所有"psver=0 时怎样 / 改了之后 0x200 仍怎样"的测试与断言**（用户明确说这类测试没意义），
并落一条守卫：**caps 报 2.0 ⇒ 引擎会建/会用的 effect 程序必须全部在实现集里**（少一个 ⇒ 启动即红）。

⚠ **前置待你裁决**：**202（Snow，`vs_1_0`）**。caps 报 2.0 后引擎会建 202 并在雪景用它；今天它被具名拒绝（雪没有）。
两条路：① 按 ADR 预转写 —— 把 `vs_1_0` 转成 C++ 在 **CPU 顶点阶段**跑（保持一份顶点实现，改动小）；
② 先登记为已知缺失，P4 的守卫按白名单放行。**建议 ②**（先把 P4 做掉，雪景另开单）。

---

## C. 已收口的单（查证从这几条入手）

| 主题 | 单 |
|---|---|
| Quit 退出路径 | `REQ-01M7HR6N2D971KV88J1JR6TPG7`（`verify` = `lift-closure-imm32`；`repro` = `aot-quit-exit`，**LIFT_STRESS 档**） |
| D26 未对齐立即数 | `REQ-01M7MGH13Y2JZ1ABT867VQGYF8` |
| AOT 音频真 feeder | `REQ-01M7MHEZRQJSMAAWPESVW44DSK`（`s0-thread-mode-truth-table`） |
| GPU 路线父单 | `REQ-01M7MNPGETP2CQJ3ACMCGEHKF5`（P0/P1/P2/P3 各有子单） |
| 转场"渐变到黑" | `REQ-01M7MJNGGWZKJ0SJKSPWNPSDBQ`（caps=0 现象；探针读数已记） |
| 转场另两条观察 | `REQ-01M7MHF67562WXHSRTEX3EQQG2`（(c) 待现场） |
| ADR | `REQ-01M7MNP9K8T81SP2HNXQ9TTXPG`（目标 = 真 GPU 渲染）· `REQ-01M7MNYPDXM2NEQR77DXYZM3CJ`（预转写） |
| 工具缺口 | `REQ-01M7MW746DSB2G7NCBSJC6XV92`（`--profile` 按帧区间归因） |

---

## D. 需要你定的（agent 不能替）

1. **202（Snow）**：实现 vs 登记缺失（见 B，建议先登记）。
2. **(c) 黑白混合**：本机不复现（章节卡蓝向 62.9% / 64.1%，暗灰 1.4% / 2.2%）⇒ 需要你那次的**机器 + 命令 + 截图**。
3. **真人听感**：`--audio sdl --sound` 下 hover SE 是否明显变短（本机只有 `pushed-played = 19200 B` 的设备读数）。

---

## E. 下一步（按顺序）

1. **P4**：取消 opt-in + caps 固定 2.0 + 删 psver=0 的测试面 + "声明⇔实现"守卫（依赖 D1 的裁决）。
2. **202**（若裁决=实现）：`vs_1_0` 预转写 → CPU 顶点阶段。
3. `REQ-01M7MW746DSB2G7NCBSJC6XV92`（profile 按帧区间）—— 只在需要解释"某段每帧 1.3e8 条"时才做。

---

## F. 踩过的坑（⛔ 别再踩）

- **caps=0 是历史配置**：转场黑 17.8 s（**6 帧吃掉 99.5% 指令**），⛔ 不要再去测它、不要再为它写断言。
- **顶点 T&L 在 CPU**（`design-notes.md` 的 `L3-tl-01`）：设备 vtable 没有 `Create/SetVertexShader` ⇒ 任何 `vs_*` 撞到就是 `comUnsupported`。
- `--raster gpu` 开关**早已删除**（GPU 是唯一光栅）。
- `--until <EA>` **必须带 `0x`**，否则不停、跑到 time-limit。
- `--profile` 只有**整跑聚合**；转场窗口的 hash 是 **8 帧周期循环**（不是单调灰阶）⇒ 别拿"多级灰阶"当该窗口判据。
- 换 FX 后章节卡 3.03→6.03 s、白闪 1.01→1.52 s（追赶语义）⇒ ⛔ 不要拿秒表逐点对数当缺陷。
- `effectResourcePins()` 的 `kPins[]` 必须带**终止项**（P2 修过一次越界读）。
- `Machine::gpuRaster()` 生产路径是 `SdlWindow` 转发包装 ⇒ 对包装 `dynamic_cast` 永远失败；effect 走 `d3d9_effect_gpu.h` 的交付入口。
- 直接写 guest 纹理像素**不推进内容版本** ⇒ GPU 上传缓存看不到。

---

## G. 怎么跑（本轮用过、可复制）

```bash
cd apps/emulator-lift
cmake --build --preset macos-arm64
ctest --test-dir out --output-on-failure                     # 107 项，103 passed + 4 平台 skip

# 转场 E2E（P4 之前：FX 路要显式开）
LIFT_D3D9_PS_VERSION=0x200 ./out/emulator-lift-aot --present headless --skip-logo --audio off \
  --game-dir ../../links/raw --appdata ../../.tmp/<副本> \
  --input-plan ../../.tmp/sc0000/black/prefix27.plan --time-limit 120

# 音频（设备读数）
./out/emulator-lift-aot --audio sdl --sound --log-level info ...   # 看 pushed-played = 19200 B

cd ../.. && pnpm test && pnpm verify && node tools/requirements.mjs --validate && node tools/ledger.mjs --validate
```
