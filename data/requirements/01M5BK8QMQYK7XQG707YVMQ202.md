# 当前实现总览与交接（会话 / 设备继续）

- id: REQ-01M5BK8QMQYK7XQG707YVMQ202
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 0
- tags: [handoff, engine spec]

## 一句话

本节点**保持 open**，作为「当前实现总览与交接」持续更新，不按收口处理（用户口径 2026-10-29）。
2026-10-29：设置界面（右列点击 / 悬停 + 各页滚动）与 ADV 文本层残差两条线已收口；文本记录生产链已落地。
当前剩余集中在「文本记录账 / 门位与计时」与「未成篇 opcode」。

## 交付物在哪

- **规范**：`repo:data/spec`（真源）· 工具 `pnpm tools spec validate|report|describe`。
- **C++ 实现线**：`repo:apps/emulator-cxx`（preset `windows-x64`，SDL3 + SDL_GPU + FreeType；`out/` 是 binaryDir）。
- **参考实现（只作旁证）**：`repo:apps/emulator-new`。
- **取证记录**（锚 EA、含不确定项，⛔ 不是 spec）：`repo:docs/02-engine/ops-forensics-2026-10-title-option.md`。
- **本线需求单**：`REQ-01M5WD5Q5HDE246W3T97F3NA8C`。

## 宿主观测面（不是引擎语义）

- `--input-script` / `--play-headless` / `--tick-ms` / `--anchor-script` / `--expect-script` / `--require-script` / `--bmp`。
- 诊断口：`--watch-locals` · `--trace-script` / `--trace-opcodes` · `--dump-scene-at N` ·
  `--dump-texture <id> --dump-texture-out <bmp>`；收口时打印引擎错误通道。

## 当前需要处理（2026-10-29）

1. `REQ-01M630YKF34EVZPMMBAJVMDDQW`：状态字第 29 位 / 文本计时器 / 文本门的服务者。
   静态线索：`0x406F79` 主循环在该位置起时清位并循环 `sub_45BE20`；`0x4096A9` 是每帧推进点；
   两套入口（每帧一格 / 置位后抽干）的关系要写进合同，不能停在"每帧一格"。
2. `REQ-01M633QTR9VXK1CA1AYJCJPNN8`：记录标志位 2（另一支字号）。
   已定：`0x4666B6` 的 `test [record+0x28],4`；`0x82` 按它选字体。未定：谁置位（排版层只 `|1` / gate / `|8`）。
3. `0x205` / `0x20E` 仍无 `core_selftest` 断言。
4. **取证齐未实现**：`0x82` 的绘制（只记请求）、`0xC2` 音乐转移、`0x2CF` 画面模式（不做真全屏）、`0x1D0` 步进算法。
5. **静态出现未走到**：`0x1BA`、`0xBA`/`0xB6`/`0xB7`/`0xC4`/`0x194`/`0x1B5`/`0x1B8`/`0x1B9`/`0x2F5`/`0x2FF`/`0x307`/`0xB8`/`0x215`。
6. **spec 条目**：上述实现多数只有取证记录，`repo:data/spec` 未成篇（准入门下一层）。

## 本轮已收口（2026-10-29）

- 设置界面单 `REQ-01M5Z5D786WF65YWHFK7K8Z7Q8` → done。
- ADV 文本层残差单 `REQ-01M618MSP80J9FHXB4JS4VE1J5` → done。
- 文本记录生产链 `REQ-01M630YH3RVEJJEGYKZJW2VQE7` → done（`0x196` 追加 72 字节记录；`0x82` 下标空间对齐；`core_selftest` 守卫）。
- `config-toggle` 环境依赖 bug `REQ-01M631SB4CRGK48ZXC34XC0SR4` → done（`--config` 种子）。

## 已打通（实测）

TITLE：`0x12e` 命中框表 → `0xa1/a2/a3` 菜单分派 → key 3 → `0x3 call-script 52` ⇒ CONFIG.BIN → `0x3 call-script 20940`。
点击在**抬起**时生效；左键 = 引擎位 0。实现指令：`0xC0`/`0x142`/`0x1BB`/`0x1A6`/`0x1D2`/`0x1D3`/`0x300`/`0x301`/`0x6E`/`0x6F`/`0x196`/`0x217`/`0x12F`/`0x2CC`/`0x2CD`/`0x2CE`/`0x2E6`/`0x2E7`/`0x2E8`/`0x2EA`/`0x10D`/`0x205`/`0x20E`/`0xC5`/`0xC6`/`0xC7`/`0x306`。

## 当前门禁（实测）

`ctest --test-dir apps/emulator-cxx/out` 21/21 · `pnpm test` 1103 通过 · `pnpm tools spec validate` 13/13 ·
`pnpm tools requirements validate` 6/6 · `pnpm tools corpus validate` 8 通过 / 0 失败 / 1 警告。

## 工作口径（2026-10-29）

- 本节点**不关闭**：它是"当前实现总览与交接"，随工作更新；进度仍以 `pnpm tools requirements plan` 为准。
- **spec 完整性判据**：所有影响可观察行为的字段 / 触发 / 转换都写全，独立实现者**只凭 spec**能写出唯一实现。
  ⇒ 需要回看 `.lst` 才能补的字段 / 触发 = **spec 缺口**（进正文或 residual/gap），不是"原则上定不了"。
- **静态优先**：单线程确定性程序，分支 / 触发都可静态枚举；"需要真机采样"只有静态读完后仍有两个在 spec tick 边界上
  不可区分的模型时才成立，并要写明采什么。先把 `0x4096A9` / `0x406F90` / `sub_45BE20` 读完。
- **spec vs emulator**：spec 写领域状态机（字段 / 触发 / 请求），机器布局走 `storage` 指针；emulator 是示范与第二实现证据。
- 详细口径：`repo:docs/02-engine/spec-proposal.md` §9；技能 `amayui-spec` §0.5。
