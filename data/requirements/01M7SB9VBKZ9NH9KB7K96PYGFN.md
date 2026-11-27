# 同一读点连写两条 click ⇒ down,down,up,up（armClickAt 只清 pending_，不清 armed_）

- id: REQ-01M7SB9VBKZ9NH9KB7K96PYGFN
- type: bug
- status: done
- parent: REQ-01M7JHSFB2SSBBNSJ82YDED8M1
- repro: apps/emulator-lift/src/runtime/pointer_core.cpp#armClickAt
- severity: S3
- done_reason: S7（0a720cf）删掉 armClickAt 的强制抬起，改成“一个读点最多一条写动作 + 同按钮 pending_ 时 click/mouse_down 不消费”；两条连续 click 现在逐条串行、消息序列为 DOWN,UP,DOWN,UP（测试 input-plan/two-clicks-serialized 实测 seq=DUDU，⛔ 不是 DDUU）。repro 保留为重开条件。
- tags: [emulator-lift, input-plan]

## 现象（2026-11-19 / macOS arm64 / 只读代码 + 纯夹具）

同一读点连写两条 `click`（配方里两条 `click` 之间没有门；`applyStep` 在同一读点的 flush 循环里
把它们都施加）⇒ guest 收到的消息序列是 **down, down, up, up**，两次点击在 guest 眼里合并成
“一次长按 + 一次释放”（而不是两次完整的 down→up）。

## 根因（代码）

- `src/runtime/input_plan.cpp` 的 `armClickAt`：只遍历 `pending_`（**已经按下、尚未抬起**）里同按钮的项
  并 `emitRelease`；**`armed_`（已 arm、还没 press）里同按钮的项没处理**。
- `pressDue`：在同一个读点 `for` 遍历 `armed_`，把两条都按下 ⇒ 同一读点出现两条 down。
- 注释声称“同一个按钮上一次按下还没抬起 ⇒ 在这里先抬起它”，但只覆盖了 `pending_` 这一半。

## 复现（机械面）

- 配方两条连续 `click`（同按钮），`atPoint` 一次 flush 施加两条 ⇒ 读 `PeekMessage` 侧的序列。
- 或纯夹具：在 `tests/input_plan.cpp` 的既有读点循环里构造“同一读点两条 click”，断言
  `down, up, down, up`（当前会是 `down, down, up, up`）。★ 用例名实施时定。

## 修法（由重设计 S7 覆盖）

不在旧模型上打补丁：按输入重设计的 S7 做“一个读点最多一条写动作 + 连点串行化”
（`armClickAt` 的强制抬起一并删除或改为排队）。
见 `.tmp/input-virtual-mouse/design.md` §4.7 / §6 Q8-A。

## 边界

- ⛔ 不要把“两条 click 在同一读点合并”当成合法语义写进文档；用户要的是每次 click 完整。
- 与 `REQ-01M7HBG0NYH957M9EK3JVMF5CC`（click 的三段闸门）不冲突：改的是“同读点多条写动作”的串行化。
