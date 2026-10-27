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
- **`0x204` 对位规则已定案（2026-10-27）**：`y` = **字形格顶**；两条绘制入口在落字前 `+ 本次ascent` ⇒ 用基线。
  模式 1 的基线 = `y + 缓存ascent`（本次被抵消）；缓存 = 字体重建时对**参考字体**（重建入口常量 "ＭＳ ゴシック"，
  不是对象当前字面名）量到的 `tmAscent`（`GetTextMetricsA` 的**出参**写 `Font+0x313EC`，`+0x313F0` = tmAscent）。
  取证：`repo:docs/02-engine/ops-forensics-2026-10-title-option.md` §26 / §28。
- **已修：文字被割裂到两行之间 / 整体偏一个 ascent**。`0x2db` 实测在根脚本 `ip=788` 就是 1；真因是缓存钉成 0。
  守卫：`core_selftest` 的「0x204 纵向对位」（改回 0 ⇒ 当场红）。
- **度量：本 emulator 用 Amayui CN（渲染字体）量缓存**（用户口径：引擎量常量 MS Gothic 是偷懒假设）。
  `cached_ascent = current_ascent` ⇒ 模式 1 与模式 0 基线相同；引擎的参考字体事实仍记在 spec 的
  `RES-RENDER-TEXT-ALIGN-REFERENCE`（不是本实现的取值来源）。守卫：同一守卫断言两模式墨迹顶差 ≤1px。
  spec 复审记录：`repo:docs/02-engine/spec-audit-2026-10.md` §七十二。
- **已修：滚动时文字不刷新**。真因是 GPU 纹理缓存按 revision 命中、`0x1f8` 又把 revision 重置成 1
  （每页固定 14 条 0x204 ⇒ 终值恒 15）；去掉重置后每轮重画版本号必前进。守卫：`core_selftest` 的两轮
  「重建 + 同条数 0x204」（旧行为 2 ⇒ 红）。⚠ 无头 `--bmp` 每帧新建 cache，**看不到**这类 bug；窗口路径才命中。
  若"底部残留五行"仍在，再按 §27 的边界查 0x1f7/0x1fb 的键区间。
- **重影的根因 = 灰度 AA + alpha-over**：真机是 1-bit GGO_BITMAP + **max/覆写**合成（`sub_46D9F0`），
  副色趟只是硬掩码几何膨胀（加粗）；我们的 alpha-over 在缝处相加 → 软边/粘连。已改 1-bit + 引擎合成，
  守卫 alpha∈{0,255}/幂等/覆写；"同色跳过"已撤回。机械判据 `.tmp/gap-scan.py`；取证 §31/§32。

## 欠账

- `0x205` / `0x20E` **没有** `core_selftest` 断言（`0x204` 对位规则已有，且做过变异验证；`0xC5`/`0xC6`/`0xC7`/`0x306` 已有）。
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
