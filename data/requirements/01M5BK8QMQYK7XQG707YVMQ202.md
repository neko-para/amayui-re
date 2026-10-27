# 当前实现总览与交接（会话 / 设备继续）

- id: REQ-01M5BK8QMQYK7XQG707YVMQ202
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 0
- tags: [handoff, engine spec]

## 一句话

2026-10-27：`apps/emulator-cxx` 已能从 **TITLE 点 Option** 进设置界面（`CONFIG.BIN` 52 → `CONFIG1.BIN` 20940 / 声牌子脚本 20939），
**左列六个 tab 全部可打开**（六条 CTest 钉住），并有**宿主脚本化输入 + 无窗口播放**这条无人值守观测面；`ctest` 16/16 绿。

## 交付物在哪

- **规范**：`data/spec/`（真源）· 工具 `pnpm tools spec validate|report|describe`。
- **C++ 实现线**：`apps/emulator-cxx/`（preset `windows-x64`，SDL3 + SDL_GPU + FreeType；`out/` 是 binaryDir）。
- **参考实现（只作旁证）**：`apps/emulator-new/`。
- **本轮取证记录**（锚 EA、含不确定项，⛔ 不是 spec）：`repo:docs/02-engine/ops-forensics-2026-10-title-option.md`。
- **本线需求单**：`REQ-01M5WD5Q5HDE246W3T97F3NA8C`（TITLE→Option→CONFIG 路径与未实现指令）。

## 宿主观测面（不是引擎语义）

- `--input-script <file>`：指令式时间线 `at move/down/up/key/wheel` → 输入字 / 光标 / 按钮位图 / 触点 / 滚轮；
  与 `apps/emulator-new` 的 `parseInputScript` 同语法。落点 `repo:apps/emulator-cxx/src/input_script.h`。
- `--play-headless` + `--tick-ms` + `--anchor-script`（时间线 0 点 = 某脚本首次进入）+ `--expect-script` /
  `--require-script` + `--bmp`。落点 `repo:apps/emulator-cxx/src/play_headless.cpp`。
- 诊断口：`--watch-locals` · `--trace-script`/`--trace-opcodes` · `--dump-scene-at N` · `--dump-texture <id> --dump-texture-out <bmp>`；
  收口时打印引擎错误通道。

## 已打通（实测）

TITLE：`0x12e` 命中框表（局部 205；基准 5/105）→ `0xa1/0xa2/0xa3` 菜单分派 → key 3 → `0x3 call-script 52` ⇒ CONFIG.BIN → `0x3 call-script 20940`（或按配置键 20939）。
点击在**抬起**时生效；左键 = 引擎位 0。

## 本轮实现的指令（取证锚见取证记录）

`0xC0` · `0x142` · `0x1BB` · `0x1A6` · `0x1D2`/`0x1D3` · `0x300`/`0x301` · `0x6E`/`0x6F`/`0x196` · `0x217`（spec 成篇）· `0x12F` ·
`0x2CC`/`0x2CD`/`0x2CE` · `0x2E6`/`0x2E7`/`0x2E8`/`0x2EA` · `0x10D` · `0x205` · `0x20E` · `0xC5`/`0xC6`/`0xC7` · `0x306`。
★ `0x74/0x7f` 参数格初值 = 配置 `message:MessageSpeed`（写点 `0x417E98`/`0x417EAB`）。

## 修复 / 未定

- **已修：滚动条中段断开**。真因是绘制项的**物体空间矩阵**（`0x1FD` 的 `sy`）被忽略；`gpu_scene.cpp` 与
  `scene_raster.cpp` 都按 `scene_record(key)->scale` 放大四边形（真机截图对照一致）。
- **已补 `0x204` 对位规则**：`y` = 格顶，基线 = y + 本次 ascent；模式 1 时 `y ← y + 缓存ascent − 本次ascent`
  （缓存格 `Font+0x313F0` 全语料**无写者** ⇒ 恒 0；`cached_text_ascent()` 恒 0）。
- **未定：文字整体偏低**。下一轮先量 `0x2db`（对位模式）在该屏的实参；若模式 0，则真因是 ascent 来源
  （引擎用 DC 字体的 `tmAscent`，我们用 FreeType ascender）。
- **未定：滚动时底部残留五行 / 两份文字**。取证已证 `0x204` 链**从不清区域**、引擎无"行数变化才刷新"规则；
  要么补 `0x20E` 的真实渲染目标模型，要么是脚本侧行为。

## 欠账

- `0x205` / `0x20E` / `0x204` 对位规则**没有** `core_selftest` 断言（`0xC5`/`0xC6`/`0xC7`/`0x306` 已有且做过变异验证）。
- **取证齐未实现**：`0x82`（文本排版）、`0xC2`（音乐转移）、`0x2CF`（画面模式 apply；按用户口径**不做真全屏**）、`0x1D0`（步进算法未定）。
- **静态出现未走到**：`0x1BA`（取证齐）、`0xBA`/`0xB6`/`0xB7`/`0xC4`/`0x194`/`0x1B5`/`0x1B8`/`0x1B9`/`0x2F5`/`0x2FF`/`0x307`/`0xB8`/`0x215`。
- **spec 条目**：上面所有实现目前只有取证记录，`data/spec` 未成篇（准入门下一层工作）。

## 当前门禁（实测）

```bash
ctest --test-dir apps/emulator-cxx/out --output-on-failure   # 16/16
pnpm test                                                    # exit 0（0 fail）
pnpm tools requirements validate                             # 6/6
pnpm tools corpus validate                                   # exit 0（8 通过 / 1 警告）
```
