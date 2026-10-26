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
- `window.cpp` 的 `kKeyBits` **宿主键位表**（规范明说物理键映射属平台边界 ⇒ 不进 spec）；SDL 键盘 → 输入字（`0xff` 采样）、鼠标 → 客户区光标（`0x109`）、按钮位图（`0x108`，SDL 左右键归约到引擎位 0/1）、`0xcc`/`0xcd` 回调按时间闸派发（`elapsed >= interval`）。
- **实测边界（600k 步到 TITLE）**：`0xcc` 1 次、`0xcd` 11405 次、`0x109` 11406 次、`0x2fc` 11405 次、`0x108` 11405 次、`0xff`/`0x100` 各 11405 次 ⇒ **回调链是活的**；headless 的 `0x12e` 为 0 只是因为没有光标源、坐标没变化，不是“脚本没走回调”。
- **hover 已修**（第三十二批）：`0x12e` 命中查找已写 spec 并在 C++ 实现；`0xcc/0xcd` 身份、`0x109` 哨兵、`0x108` 右键位同批订正。窗口路径的“光标在客户区内”仍需真机点一次确认（代码路径已改）。

### ③ 过滤条件内缺失指令 → 0
- 对照 `apps/emulator-new` 的 `HANDLERS` 表机械核对：**202 条 JS handler ↔ 203 个 C++ case，missing 0**。
- 本阶段补的批次：17 条 TITLE 路径 → 24 条（帧/本地块/标志/数值/场景读回 + 暂停门 + `0x23c`）→ 3D setter 2 条 + 按住派发族 5 条 → **memflip `0x6d`（两组 bank）** + **存档族 7 条**（`SaveHost` 三态读宿主边界、`.STH` 的 BMP 容器）。
- 存档族的三条硬口径（照 spec，不照参考实现）：`0x19e` 成功 = **0**；`0x1a1` 打开成功后**无条件**重编码全局整型池且**成功不写 op1**；`0x1af` **打不开优先于表面门**。

## 第三十二批 · 2026-10-26（hover / 输入契约）

- **spec**：新增 `CORE-INPUT-HIT-TEST-12E`；订正 `CORE-INPUT-CALLBACK-REGISTER` / `-PUMP`（脚本身份，不是深度）、`-MOUSE-POS`（哨兵只在输入停用；出窗写真实坐标）、`-MOUSE-BUTTON`（引擎位 0/1 固定，交换只换内容）；`GAP-OPS-0100-01FF` 移出 `0x12e`。
- **C++**：`op_12e` 实现；`mouse_owner_` 改 `frame.local_depth`；窗口光标不再把“出客户区”当哨兵；SDL 右键重映射到引擎位 1；selftest 覆盖命中 / 未命中 / `start+1` / `count=0` / 同址跳过（同址门已做变异验证）。
- **emulator-new**：同步 `0x12e` 与 owner 语义（参考实现不再断言 stub）。
- **需求**：`REQ-01M5VBGREC7T3CC7H3QCTV2877`（hover，已收口）、`REQ-01M5VBGG3YTDGKBD339YD6HF17`（P1，已收口）、`REQ-01M5VBVGWE7ZQN0K1EK936M5TM`（P2，open：`0x2fc` 触点 / 键总数 7 / `0x101` 采样 / `0x202-0x203` 归一化）。

## 第三十三批 · 2026-10-26（启动 / 退出 + 输入 P2）

- **`--skip-logo`**：跑根脚本前把引擎 ShowLogo 格置 0（`0x130` 分支），不是宿主捷径。`--footprint --skip-logo` 的 script-chain 不再含 21090、`0x20F` 不出现，仍到 21092；`--skip-logo --play-frames 1` 正常退出。
- **退出路径**：补 `0x23d` / `0x32b` handler（影片槽 42..999 清理 / 网格槽 0..999 清空），根帧 `exit` ⇒ `Halted`；窗口宿主遇 `halted()` 立即关窗返回。spec 规则 `CORE-OPS-MOVIE-CLEAR-23D` / `CORE-OPS-SCENE-MESH-CLEAR-32B` 已成篇并移出 gaps。守卫 `check_exit_cleanup` + `exit-cleanup-contract`。
- **P2 输入**：`0x2fc` 改为触点查询（桌面鼠标-only 恒无触点 ⇒ 结果 0，TITLE 回退 `0x109`/`0x108`）；键总数初值 7；`0x101` 先采样再丢弃；`0x202/0x203/0x322/0x323` 负值归一化 + `0x203` op2 落记录；emulator-new 同步。
- **需求**：`REQ-01M5VBVGWE7ZQN0K1EK936M5TM`（P2，done）、`REQ-01M5VV4Z8ZTARZFV9RD8NE8VGP`（退出，done）、`REQ-01M5VV4ZSGQ8PGNYDBAA47VBTP`（skip-logo，done）、`REQ-01M5VV50B4PJYC3QWK466H0CZ6`（0x23d/32b spec，done）。

## 未做 / 待核

- **摇杆**（范围外）；TITLE 的键盘路径已采样并会派发“无按键”表项，实际按键按下路径未做真机验证。
- `0x19e` 的写侧内容构造与 `0x1a1` 的整档读回 —— 属 `FMT-SAVE-CODEC` / `FMT-SAVE-ENVELOPE` 等 gap 单的 seam；没注入就**响亮失败**（`save-codec-missing` / `save-restore-unmodeled`）。
- 存档族的实现选择（已登记）：标题串来源（`set_save_identity`，空串 = 不校验）、写盘失败的 op1（用 1 + 诊断）、`0x1a1` 头坏时 op1 不写、整池重编码在 memflip 下按当前 bank、1/4/8/32bpp 缩略图解码无样本。
- spec 侧的残差 / 未成篇计数以 `pnpm tools spec report` 为准；P2 输入契约仍是 open。

## 当前门禁

```bash
ctest --test-dir apps/emulator-cxx/out --output-on-failure  # macOS + links/install: 6/6（含 boot / skip-logo / logo）
pnpm test                 # 1075 pass / 0 fail
pnpm typecheck            # 六个工程全绿
pnpm tools spec validate  # 13/13 全绿，未登记 0
pnpm tools requirements validate # 6/6 全绿
pnpm tools ledger validate # 8/8 全绿
```
