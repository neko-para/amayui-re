# anchor-script 只能锚 boot 期脚本：0 点 pending 期数值触发全抑制 ⇒ 无法用输入推到后面再锚（需 anchor 前输入 / visit 计数）

- id: REQ-01M710E6PV4PPHECPPCCTRF5RA
- type: req
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- verify: apps/emulator-lift/tests/input_anchor.cpp#anchor/absolute-segment-fires-before-latch
- done_reason: before-anchor / after-anchor 段标记落地（必须成对、无隐式边界）：绝对段触发不参与重定基且闩锁前也执行，相对段语义不变；解析期错误 4+1 类。守卫 input-anchor 19 例/279 断言（手撕抑制豁免 12 红、重定基豁免 7 红），变异 1 条实测红。真机验收：--anchor-script 0x34（CONFIG.BIN，点 OPTION 后同帧进入）⇒ 两遍 origin.frame=100 逐字相同，相对动作精确落 +5/+10，exit 0；反例 0xDEADBEEF ⇒ exit 5 + origin-missed 且绝对段输入照跑。残差：绝对段须用帧锚才可复现（insn 锚两遍 101/99，real-time 漂移）。
- tags: [emulator-lift, input-script, anchor]

## 判据（存在性 + 观察）

`REQ-01M6WS9G1EQRZ0EYM21V0DPZVY`（`--anchor-script`）收口时的实测限制：三组 anchor 的 `origin.frame` 全是 0
⇒ **只能锚 boot 期脚本**；想"先用输入把游戏推到某屏、再锚那一刻的脚本"做不到，因为 0 点 pending 期间所有数值触发被抑制。

**修好后的判据**（本轮全部实测过）：
1. 输入脚本里能用**绝对**触发把游戏推到后面，再锚那一刻首次进入的脚本 ⇒ **`origin.frame > 0`**。
2. 0 点**之后**的**相对**触发精确落在 `origin` 基准上（不是绝对时间）。
3. 同一脚本 + 同一 anchor ⇒ 两遍 `(脚本 id, frame)` **逐字相同**。
4. 没进入目标脚本 ⇒ 仍然 `origin-missed:` + **exit 5**，且绝对段的输入**确实执行过**（⛔ 不静默按 0 点跑）。

## 已实现（2026-11-10）：`before-anchor` / `after-anchor` 两个段标记

- **语法**：两行各**独占一行**、**必须成对**（缺 `after-anchor` ⇒ 解析期错误，⛔ 没有任何"隐式边界"语义）；必须配 `--anchor-script`。
  `before-anchor` 之后 = **绝对段**（按声明值执行、不参与 `+= origin`、闩锁前也执行）；`after-anchor` 之后 = **相对段**（今天的语义）。
  其它解析期错误：重复 `before-anchor` · `after-anchor` 在 `before-anchor` 之前 · 段内出现 `profile_begin/end`（窗口 profiler 的 0 点语义与锚点冲突）· 标记行带参数。
- **实现**：`input_script.{h,cpp}` 的 `Action::absolute` 打标 + 抑制/重定基两处豁免（`if (origin_pending && !a.absolute) continue;`、
  `rebasePendingTriggers()` 跳过 absolute）；`main.cpp` 把 0 点配置移到脚本解析**之前**（段标记的解析期判据要用它）。`machine.cpp` **不用改**。
- **守卫**：`tests/input_anchor.cpp` 从 11 例 / 187 断言 → **19 例 / 279 断言**，新增 `absolute-segment-fires-before-latch` ·
  `absolute-segment-not-rebased` · `absolute-frame-and-insn` · `rejects-before-anchor-without-after` · `rejects-duplicate-before-anchor` ·
  `rejects-after-anchor-without-before` · `rejects-segment-without-anchor` · `rejects-profile-in-before-segment`。
  变异 1 条（去掉抑制豁免）实测红；两次手撕（抑制豁免 / 重定基豁免）分别 12 红 / 7 红，恢复后 `ctest` **73/73**。
- **真机验收**（window/GPU 档 400 帧、`--skip-logo --audio off`，exe sha256 `8654F0CC…85405B72`）：`before-anchor` 里放
  `at frame 96/97` 的 `mouse_move` + `at frame 98` 的 `click 812 622 hold 2`，`after-anchor` 里放 `at frame 5 mouse_move 1100 300` / `at frame 10 quit`，
  `--anchor-script 0x34` ⇒ **两遍都是 `origin : script=0x00000034 frame=100`**，相对动作精确落在 `frame=105` / `frame=110`，
  `finished：5 条动作全部应用`、`token violations=0`、exit 0。★ discovery：点 OPTION 后**同一帧**先 `CONFIG.BIN=0x34(cur=2)`、紧跟
  `CONFIG1.BIN=0x51CC(cur=3)`（与 `data/routes/screens.json` 的 `option.arrive=52` 及 cxx 脚本注释的 `CONFIG1.BIN 20940` 两处独立对上）。
- **反例**：`--anchor-script 0xDEADBEEF` ⇒ `origin-missed: … last=CONFIG1.BIN frames=400` + **exit 5** + 摘要无 `origin` 行、相对段**一条没跑**；
  而绝对段照跑（日志有 frame 96/97/98 的 move + 按下）⇒ "没进入目标脚本"没有被静默成 0 点。

## 已知取舍 / 残差

- ⚠ **绝对段要用帧锚才可复现**：把绝对段改成 `insn` 锚时同 id 的 `origin.frame` 两遍是 101 / 99（Δ2）—— real-time 档帧号↔指令数的漂移
  （与本仓既有口径一致），**不是 0 点机制问题**；所以"位置可复现"要求绝对段按**帧**给时刻，`insns` 只记不判。
- 未取证：0 点**重进不定基**在真机上没有一次"两次进 OPTION"的运行（没有 `origin 重进` 行）；`0x51CC`（`CONFIG1.BIN`）没单独跑配对。
- 段内禁用 `profile_*` 是**解析期错误** ⇒ "在绝对段里量窗口"做不到，要量只能等 0 点之后。

## 与别的单的关系

- 直接承接 `REQ-01M6WS9G1EQRZ0EYM21V0DPZVY`（收口时的实测限制，不是遗漏）。
- 同族：`REQ-01M6XVZ4ANQRC215ACG90A35NR`（脚本身份 marker —— 绝对段复用它那条采样源，但**不改**它的边沿语义）。
