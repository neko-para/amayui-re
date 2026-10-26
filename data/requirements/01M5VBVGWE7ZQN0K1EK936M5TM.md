# 输入契约第二批：0x2fc 触点 · 键总数初值 · 0x101 采样 · 0x202/0x203 归一化

- id: REQ-01M5VBVGWE7ZQN0K1EK936M5TM
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- done_reason: 0x2fc 改为触点查询（C++/emulator-new/spec）；键总数初值 7；0x101 补平台采样；0x202/0x203/0x322/0x323 负值归一化与 0x203 op2 落记录。C++ selftest check_input_dispatch / check_color_normalization / check_mouse_and_hit_test；pnpm test / typecheck / ctest 全绿。
- tags: [input, spec]

## 范围

0x12e 与 P1 输入契约（0xcc/0xcd / 0x109 / 0x108）之外的四处偏差：

1. `0x2fc` 是**触点查询**，不是鼠标事件队列：引擎扫 `Input` 的触点缓冲（40B/项）；无触点只写结果 0 后返回；有触点输出**虚拟屏坐标** / flags / touch id，不 pop 队列。spec 现在写成“取一条鼠标事件 / 事件被消费 / 客户区坐标”，C++ 宿主还在按钮/滚轮时合成事件。
2. 键总数初值：引擎 Input 构造写 `Input[259] = 7`（= `Engine[517]`），spec 现在说“引擎不建立初值、唯一写点是 0xfe”。
3. `0x101` 缺平台采样：spec 要求“先采样、再清第 27 位、清输入字、写 A/B”，C++ 没调用采样源。
4. `0x202` / `0x203` 的负值归一化与 `0x203` 第 2 操作数：引擎把 alpha>255 夹 255、alpha/color<0 取现有值，并把第 2 个操作数写进记录（`sub_4ACF60` 的 a3）；C++ 直接 `(alpha<<24)|(color&0xffffff)` 并忽略 op2。

## 判据

- `0x2fc` 无触点：只写结果 0，op2..op5 不变；同一触点连续查询返回同一点，不是“下一条”。
- 未执行 `0xfe` 的脚本，`0x100` 的“无按键”表项下标必须是 7。
- `0x101` 必须调用一次采样源，随后输入字为 0。
- `0x203` 传 alpha=−1 / color=−1 时必须保留现有颜色；op2 必须写进记录。

## 备注

先做 0x12e + P1；本单是同一回调路径的剩余合同偏差，不阻塞 hover 的最小闭环。
