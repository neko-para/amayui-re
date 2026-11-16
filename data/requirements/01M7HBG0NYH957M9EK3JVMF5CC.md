# 输入配方 DSL 收缩为三条指令（await script / wait / click）：去掉帧号门，改用脚本身份电平门

- id: REQ-01M7HBG0NYH957M9EK3JVMF5CC
- type: req
- status: done
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- verify: apps/emulator-lift/CMakeLists.txt#input-plan-read-points
- done_reason: 三条指令落地：await script（电平门，大小写不敏感、两侧剥 .bin）/ wait <ms>（steady_clock 非阻塞 deadline + 测试注入缝）/ click（1280×720 参考系，经 cursor_map 新增的唯一纯函数 referenceToClientPixel 缩放到 guest 客户区，move/down/up 共用同一映射）。presents/always/key/图语法 = 解析期硬错；key 死码已删。门禁：build exit 0；ctest 102 项 100% passed（6 skip 结构性）；pnpm test 552/552；pnpm verify 500/500；变异自检 2/2 红。★ 判据 2 经父级实测重写：只有 2a 负对照（NOSUCHSCRIPT ⇒ fired=0 + 基线 region）是确定性凭据；带 wait 的 region 级正例 12 跑 4 成 ⇒ 时间门不作判据，region 级正例等位置谓词 REQ-01M7HC1Z1RD4DNH25BN5ZE1HQK。★ 存疑：cli-aot-flags 整段在 _WIN32 内且 mac SKIP ⇒ 那块本机未编译未运行（CLI 硬错用真二进制手工补证）。
- tags: [emulator-lift, input, dsl, aot]

## 一句话

把 AOT 臂的输入配方（`--input-plan`）**收缩成三条指令**、删掉"依赖特定帧"的门：`await script TITLE`（电平门，忽略 `.bin`）·
`wait 500`（宿主单调时钟毫秒）· `click 1070 480 [left|right]`（1280×720 参考系，按钮词裸写、省略 = left）。

## 为什么要重做（用户口径，2026-11-16）

用户原话：**"不依赖特定 frame，这个本身人工都无法评估"**。

- 旧配方除 `await always` 就只剩 `await presents <n>` —— **`presents` 就是帧号**，与父节点正文、`input-plan.md` §2.5 冲突。
- 后果不是"不够精确"而是**表达不了**；且同一个 N 在两臂不对应同一 guest 时刻（BUG-26 候选①：时间步长差 ~15×）。
- 状态门的材料本来就在（`CondKind` 的 `Script`/`ScreenChanged`/`ScreenRect`/`ScreenHash`），而 `--input-plan` 全部拒收；
  本单只放行 `Script`。

## 语义（已裁决，⛔ 实现不得再改）

| 指令 | 语义 |
|---|---|
| `await script <NAME>` | **电平**门：当前身份 == NAME 即通过（不是边沿）。名字**大小写不敏感**，两侧都忽略 `.bin` |
| `wait <ms>` | 正整数毫秒；**宿主单调时钟**；从上一条动作生效起算。★ 只作粗推进：⛔ 不参与跨臂可比、⛔ 不当判据 |
| `click <x> <y> [left\|right]` | x/y = **1280×720 参考系**整数；按钮词**裸写**、省略 = left；⛔ 不收 `=left`（与 `--interaction-graph` 同一拼法，`interaction_graph.cpp:563-575`） |

- 按文件顺序执行；`click` 在同一读点施加（flush-before-read 口径不变）；`#` 注释、空行忽略。
- ⛔ 没有每步超时：卡住由 `--time-limit` / `--max-frames` 这层看门狗报。

**坐标合同**（用户 2026-11-16 追加）：
- (x,y) 属于**标准大小 1280×720** 参考系；**投递前必须按实际尺寸等比缩放**（窗口缩放 / 非 1280×720 适配器模式 / Retina）。
  ⛔ 不许原样投出去、⛔ 不许在 plan 里再写一份公式：唯一许可来源是 `src/runtime/cursor_map.h`（纯函数、SDL-free）。
- `mouseMove` 与 `mouseButton` 必须用**同一个**映射结果（现在各传一次坐标，各算一次就会静默漂移）。

## 同时收缩掉的（不是遗漏）

`await always` · `await presents <n>` · `key <VK>` · 图那套 node 语法 ⇒ plan 文件里一律**解析期硬错**：
`exit 2` + `文件:行号:` + 点名"未知指令"。★ `--interaction-graph` 自身**一个字不动**（推进点不同）。

## 不动的东西

- `atPoint()` 仍是唯一能施加 step 的入口 ⇒ `machine.cpp` / `aot_run.cpp` 的禁用词仍必须 0 命中；
  身份采样**只读 guest 内存**（`runtime/engine_marker.h`），⛔ 不许为它加"块边界 pump"。

## 判据（收口时补实，逐条要能红）

1. `await presents 120` ⇒ `exit 2` + 点名行号。
2. 门真的按身份裁决（2026-11-16 实测后重写：票面原写 `right` + 两行配方错，且 `wait` 门不稳定）：
   - 2a 负对照（确定性）：`await script NOSUCHSCRIPT` + 同一 click ⇒ `fired=0` + 基线 `region=108595152/21/6674`；
   - 2b 正例**只到 `fired=1`**：同一份 `await script TITLE` + `wait 2000` + `click 1070 480 left`，12 跑只有 4 跑
     得到 `117490928/25/6821`、其余是无输入基线 ⇒ **时间门不能当 region 判据**（`wait` 是墙钟、guest 推进不是）；
     region 级正例等位置谓词（`REQ-01M7HC1Z1RD4DNH25BN5ZE1HQK`）。`heap_blocks` 长跑 ±1（6821/6821/6822，原因未查）。
   - 2c 已知退化：`--skip-logo` 下第一个读点即 `TITLE.BIN` ⇒ 身份门表达不了"菜单就绪"。
3. `wait <ms>` 真的等（单测时钟注入，或加/不加 `wait` 的动作次数差异）。
4. 未知指令 / 缺参数 / 负毫秒 ⇒ `exit 2` + `文件:行号:`。
5. `click` 的抬起语义（`released`/`held`）在删掉 `key` 后仍成立。
6. **坐标缩放**：单测钉"1280×720 → 实际尺寸"（至少两组尺寸、一组非 1:1），且 move 与 button 是同一映射结果。

## 已知退化（不遮掩，2026-11-16 实测）

- **身份门是电平判据，不保证该脚本已进入它的输入循环**：`--skip-logo` 下启动后第一个 guest 读点身份就已经是
  `TITLE.BIN` ⇒ `await script TITLE` 立刻满足，点击会打在 boot 上（读数 = 无输入基线）。
  ★ 且 `wait <ms>` **本身不稳定**（墙钟 ≠ guest 推进）：12 跑 4 成，所以它连"粗推进"都只能算尽力而为；
  **根治 = 位置谓词**（`sample` 的 `pos`，见 `REQ-01M7HC1Z1RD4DNH25BN5ZE1HQK`）。`right` 对该 UI 无效（left 才开）。

## 未做（边界外）

- **解释器不接 `--input-plan`**（`setInputPlan()` 只有 `aot_main.cpp:484-485` 两处调用；给它是 `未知参数`+exit 2）
  ⇒ "两臂同一份配方"要等解释器接线（另开单）；BUG-26 判据因此**当前不可执行**，那条单要跟着改。
- `ScreenChanged` / `ScreenRect` / `click center` 不做；键盘（`key`）本单删除，需要时再加。
