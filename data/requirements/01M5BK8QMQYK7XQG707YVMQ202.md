# 当前实现总览与交接（会话 / 设备继续）

- id: REQ-01M5BK8QMQYK7XQG707YVMQ202
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 0
- tags: [handoff, engine spec]

## 一句话

2026-10-26 交接：`apps/emulator-cxx`（C++ 原生宿主）已按 spec 重写并在真实安装上跑到 TITLE；**过滤条件内（排除 audio / text-subsystem / live2d）的缺失指令 = 0**，门禁全绿。工作树有未提交改动（长期纪律：不 `git add` / `commit`）。

## 交付物在哪

- **规范**：`data/spec/`（真源）；工具 `pnpm tools spec validate|report|describe`。
- **C++ 实现线**：`apps/emulator-cxx/`（CMake preset `windows-x64`，SDL3 + SDL_GPU，clang-cl）。
- **参考实现（只作旁证）**：`apps/emulator-new/`。
- **审计记录**：`docs/02-engine/spec-audit-2026-10.md`（本阶段见 §64–§67）。
- **进度**：`pnpm tools requirements plan`（本文件只是交接摘要，不是进度真源）。
- **本线已覆盖的机制**（spec 机制表里属于 `emulator-cxx` 的部分）：`CORE-ISA`、`CORE-POOL`、`CORE-FRAME`、`CORE-VM`、`CORE-VM-LOOP`、`CORE-BOOT`、`CORE-CONFIG`（配置库：词表 + ALF 发行默认表 + `SYS4REG.INI` 覆盖 + 脚本键表）、`FMT-ALF`、`FMT-AGF`、`CORE-OPS-*`、`CORE-INPUT-*`、`CORE-TIMING-*`、`CORE-SCENE-*`、`SPC-POOL-MEMFLIP`。

## 最近完成（第三十一批 · 2026-10-26）

### ① 残差收口（subagent 取证 → spec → 独立复审 → 二次收口）
- **暂停 = 区间忙标记（同一格）**，条件是「忙 = 1 且**等待位 = 0**」（原正文写反）；`0x1f4` 置 / `0x1f5` 在嵌套计数归零时清。
- `0x20c` 的「别处驱动」读**同一格** ⇒ 区间内**不重盖**时钟对；不忙时它先覆盖再请求渲染步 ⇒ 它自己那次步读**覆盖后**的值。
- **本帧场景时刻 = 渲染步内直读的活格**（不是迭代头快照）；`0x23c` 是**无条件重盖**的第四写者（`sub_41A2C0`）。
- spec 5 文件（`70-vm.md` / `60-ops-render.md` / `60-ops-scene.md` / `bindings.json` / `audit.json`）：**删 6 条已答残差**、`RES-MOVIE-PAUSE` 收窄、新增 `RES-CLOCK-WRITERS` / `RES-VM-PAUSE-MOVIE-BIT`。
- 独立复审指出 6 条矛盾 + 1 个真缺口（暂停 × 已置影片位），已全部收口。

### ② 键盘 + 鼠标接入（不接摇杆）
- `window.cpp` 的 `kKeyBits` **宿主键位表**（规范明说物理键映射属平台边界 ⇒ 不进 spec）；SDL 键盘 → 输入字（`0xff` 采样）、鼠标 → 客户区光标（`0x109`）、按钮位图（`0x108`，位 0 左 / 位 1 右）、按钮按下/抬起与滚轮 → 事件队列（`0x2fc`）；`0xcc`/`0xcd` 回调按时间闸派发（`elapsed >= interval`）。
- 只在窗口路径生效；`--footprint` / `--trace` / `--render-*` 无窗口 ⇒ 无输入（`0x109` 写 −100000、`0x108` 写 0、`0x2fc` 恒"没有事件"）。
- **实测边界**（500k 步到 TITLE）：输入环在轮询鼠标 —— `0x109` 9133、`0x2fc` 9132、`0x108` 9132、`0x100` 9132 次；**不执行 `0xff` / `0xcc` / `0xcd`** ⇒ 键盘位在 TITLE 上不会写进输入字（`input_word_` 恒 0），键盘目前只是宿主侧接通、脚本未使用；**TITLE 的鼠标是轮询（不是回调）**。
- ⚠ **未解决**：实测在 TITLE 里 hover 无反应（预期 TITLE 脚本自己读光标做 hover 特效）——正在查是"光标源在窗口路径返回不可用 / 坐标空间不对"还是"脚本另有门"。

### ③ 过滤条件内缺失指令 → 0
- 对照 `apps/emulator-new` 的 `HANDLERS` 表机械核对：**202 条 JS handler ↔ 203 个 C++ case，missing 0**。
- 本阶段补的批次：17 条 TITLE 路径 → 24 条（帧/本地块/标志/数值/场景读回 + 暂停门 + `0x23c`）→ 3D setter 2 条 + 按住派发族 5 条 → **memflip `0x6d`（两组 bank）** + **存档族 7 条**（`SaveHost` 三态读宿主边界、`.STH` 的 BMP 容器）。
- 存档族的三条硬口径（照 spec，不照参考实现）：`0x19e` 成功 = **0**；`0x1a1` 打开成功后**无条件**重编码全局整型池且**成功不写 op1**；`0x1af` **打不开优先于表面门**。

## 未做 / 待核

- **摇杆**（范围外）；键盘在 TITLE 路径上未被脚本使用（见上）。
- `0x19e` 的写侧内容构造与 `0x1a1` 的整档读回 —— 属 `FMT-SAVE-CODEC` / `FMT-SAVE-ENVELOPE` 等 gap 单的 seam；没注入就**响亮失败**（`save-codec-missing` / `save-restore-unmodeled`）。
- 存档族的实现选择（已登记）：标题串来源（`set_save_identity`，空串 = 不校验）、写盘失败的 op1（用 1 + 诊断）、`0x1a1` 头坏时 op1 不写、整池重编码在 memflip 下按当前 bank、1/4/8/32bpp 缩略图解码无样本。
- spec 侧仍有 **24 条残差 + 5 条 `unhandled`**（`pnpm tools spec report`）。

## 当前门禁

```bash
ctest --test-dir apps/emulator-cxx/out/build/windows-x64 --output-on-failure  # 5/5
pnpm test                 # 1071 pass / 0 fail
pnpm test:mutation        # 363/363 按预期变红
pnpm tools spec validate  # 13/13 全绿，未登记 0
pnpm tools requirements validate # 6/6 全绿
```
