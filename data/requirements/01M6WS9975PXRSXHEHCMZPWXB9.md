# 输入脚本语法统一（父级已裁暂不统一）：lift 的 at frame + Win32 VK vs cxx/new 的毫秒 + 引擎键号 0..31

- id: REQ-01M6WS9975PXRSXHEHCMZPWXB9
- type: req
- status: open
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- tags: [emulator-lift input script]

## 现状：两套语法，互不可读

| 线 | 时间锚 | 动作与编号 | 落点 |
|---|---|---|---|
| emulator-lift | `at frame N` / `at guest_ms T`（`after` / `wait`） | `mouse_move X Y` / `click` / `key_down VK`（**Win32 VK**） | `repo:apps/emulator-lift/src/runtime/input_script.h` |
| emulator-cxx | 行首**仿真毫秒**，无帧触发 | `move x y` / `down N` / `up N`（鼠标按钮 N）/ `key N down\|up`（输入位 N）；N = **引擎编号 0..31** | `repo:apps/emulator-cxx/src/input_script.h` |
| emulator-new | 同上 | 同上（`parseInputScript` / `ScriptedInputHost`） | `repo:apps/emulator-new/src/platform/node/input.ts` |

差异两层，不只是字段名不同：
1. **抽象层**：lift 给平台级（VK），由 guest 里的引擎按映射表自己归约出 32 位输入字；cxx/new 直接给**引擎侧的编号**（输入位 / 鼠标按钮位），绕过引擎的映射表。两者不是同一层的同义词。
2. **时间锚**：lift 有帧触发（确定性）；cxx/new 只有毫秒（real-time 时钟下不可复现）。

## 父级裁决（2026-11-08）：**暂不统一**

`apps/emulator-cxx` / `apps/emulator-new` 是冻结件（`AGENTS.md` §11）：⛔ 不为它们改解析器、⛔ 不迁证据。统一 = 让它们能读 lift 的时间线并反过来，属于解冻，须单独裁决。

## 若将来统一：谁是真源

- **语义真源 = spec 的输入归约**：`repo:data/spec/chapters/60-ops-input.md`（平台原始状态 → 映射表 → 32 位输入字 + 光标 + 按钮位图 + 触点）。⛔ 不把任一实现的解析器当规范。
- **语法真源 = `repo:apps/emulator-lift/src/runtime/input_script.cpp`**（唯一在维护的线）。统一时由它定义语法；cxx/new 的“引擎编号”写法只能作为**兼容读入面**，⛔ 不反向定义语法。
- 因此统一的形状是 **时间锚用帧 + 动作用平台级（VK）**，而不是把 lift 改成引擎位号。

## 判据（若开做）

- 一条“点 OPTION”的时间线在三条线上给出同一个 guest 可观察结果（消息序 + 采样电平 + 光标位置）。
- cxx/new 解冻后的既有守卫全绿（不允许借“统一”顺手改它们的行为断言）。
- 若兼容层是新增而非改冻结件，要有守卫证明“旧脚本仍能读”。

## 现在绕到了哪里

三条线各写各的脚本；仓库里有三份“点一下”的语法，新 agent 会把 cxx/new 的格式当 lift 的照抄。
跨线复用时间线要手工改时间单位与编号，解析失败也只报自己那套的行号。
