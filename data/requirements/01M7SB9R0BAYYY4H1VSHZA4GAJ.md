# 输入注入：shim 侧虚拟鼠标（一台设备 / 四个驱动者 / SDL 只作可选驱动）

- id: REQ-01M7SB9R0BAYYY4H1VSHZA4GAJ
- type: req
- status: doing
- parent: REQ-01M7JHSFB2SSBBNSJ82YDED8M1
- tags: [emulator-lift, input]

## 一句话

把输入注入做成 **shim 侧的一台虚拟鼠标**：设备只有一份状态（guest 屏幕坐标 + 电平 + 来源 + 单一租约），
SDL / 配方 / 控制层 / 机器内驱动器都只是**驱动**它；shim 的读点只读设备，**不要求 SDL 窗口或事件泵存在**。
用户口径（2026-11-19）：“shim 层需要一个‘虚拟鼠标’，而 input-plan 和 SDL 的输入都只是控制鼠标移动，
而不是两遍分别组合。需要确保 shim 看到的状态是唯一的，不依赖 SDL。”
## 现状审计（2026-11-19 / macOS arm64 / 只读，无构建）

- **已满足**：guest 可见输入的**事件写入路径只有一条** —— 18 个 `input::*` 落点 + 5 个会话写，
  全部最终写 `WindowManager` 的那一份状态；读点只读它。`--present headless` 仍建隐藏 SDL 窗口 + GPU，
  但缺的只是 sink 与事件泵 ⇒ 读点路径本身不要求 SDL。
- **未满足**：设备不是“一台设备”——`WindowManager` 里 6 组字段 + **3 套所有权**
  （`key_owner_[256]` / `button_owner_[3]` / `cursor_script_owned_`）+ 1 个零调用者的 `capture_`；
  SDL 层还持有第二份“鼠标在哪 / 点了没”（overlay hover 读 `SDL_GetMouseState`、`click_router`、
  `warp_echo` 回灌期望）。
- **“两遍分别组合”的 5 个形状**：①同一个 `click` 由 plan 与 `InputScript` 两套 hold 策略组合；
  ②overlay 是 OS 路径上的第二道闸门；③`--cursor-warp` 要独立回灌抑制；④`SetCursorPos` 是隐藏推进点；
  ⑤`MSG.time` 有三个来源。
## 设计（v2，待裁决；稿在 `.tmp/input-virtual-mouse/`）

- 新件 `runtime/input_device.{h,cpp}`（`InputDevice`），由 `WindowManager` **以成员持有**（同线程 / 同生命周期）；
  设备只存 **guest 屏幕坐标 + 256 键电平 + 逐电平来源 + 单一 `Driver` 租约**，**无时钟 / 无队列 / 无窗口**。
- `runtime/input.{h,cpp}` 是“**设备写 + 消息投影**”的唯一组合点：消息字段**写后从设备回读**，
  且**只在电平转移（0→1 / 1→0）时 post**。
- 四个驱动者（SDL / 配方 / 控制层 / `InteractionGraph`）都只调 `input::*`；SDL 适配器只做
  “窗口像素 → guest 屏幕坐标”的换算，不持有状态。
- 设计稿 / 独立复审 / 修订记录：`.tmp/input-virtual-mouse/{design,review,changes-v2}.md`（未入库，裁决后落 docs）。
## 切片（行为标记）

| 片 | 主题 | 行为 | 前置 |
|---|---|---|---|
| S1 | 设备实体（状态搬家） | 逐字节不变 | 无 |
| S2 | 消息 = 设备的投影（写后回读） | 逐字节不变 | S1 |
| S3 | 按钮位图 = VK 电平的投影 | 逐字节不变 | S1 |
| S4 | 单一租约 + `Driver::Guest` + 转移投影 | **变**（guest 可见） | Q1 / Q7 |
| S5 | overlay 指针改读设备 | **变**（诊断/产品面） | 可先做纯映射；注入路由见 Q6 |
| S6 | 配方 `mouse_move` / `mouse_down` / `mouse_up` | 增量（旧配方不变） | Q3 |
| S7 | 一个读点最多一条写动作 + 连点串行化 | **变**（guest 时序） | Q2 / Q8 |
| S8 | 文档切片（`MSG.time` 等） | 无代码 | Q5-B 已定 |
## 裁决（2026-11-19，用户：“无问题，按这个策略处理即可”）

- **Q1** = 推荐：1→0 且来源是 OS 才 post；Program-owned 不动；失焦 / 暂停 post up。
- **Q2** = 推荐：接受“一个读点最多施加一条写动作”（含 `drainImmediate`）。
- **Q3** = 推荐：`mouse_move` + `mouse_down/up`；down 轻闸门、up 直写、滚轮不被光标写失败丢弃。
- **Q6** = 推荐 A：overlay 注入点击过 router，并实现 `--ui-input`（新切片 S9）。
- **Q8** = 推荐 A：删除连点强制抬起，连点串行化。
- Q4 取消（解释器 target 已退役）；Q5 选 B（`MSG.time` 只写文档）；Q7 设计已定（`Driver::Guest`）。
## 实施顺序（按依赖）

1. **S1 → S2 → S3**：逐字节不变，一次批（设备实体 / 消息投影 / 位图派生）。
2. **S4**：单一租约 + `Driver::Guest` + 转移投影（Q1 / Q7；改 guest 可见语义）。
3. **S7**：一个读点一条写动作 + 连点串行化（Q2 / Q8）。
4. **S6**：配方 `mouse_move` / `mouse_down` / `mouse_up`（Q3；S7 之后再落，拖拽自动串行）。
5. **S5**：overlay 指针改读设备。
6. **S9**：overlay 注入点击过 router + `--ui-input`（Q6）。
7. **S8**：文档切片（`MSG.time` / 过期判据 / 文档债）。
## 判据（实施时）

- 逐字节不变的片：`ctest` 与 `pnpm test`/`pnpm verify` 全绿，现有 `input-plan*` / `messagebox` / `cursor-warp` 用例逐项不变。
- 语义片：新用例的断言形状见设计 §4（真实 CTest 名实施时定，⛔ 不挂假锚）。
- 收口时把设计稿落 `docs/03-emulator/input-device.md`，并更新 `docs/03-emulator/input-plan.md` 的落点表。
## 边界

- ⛔ 不恢复 `--input-script` / 图语法；⛔ 不引入第二份状态；⛔ 不把 SDL 事件直接当 guest 状态。
- ⛔ 不改 guest 可见语义的片必须与“改语义”的片分开验收（S4 / S5 / S7 要显式裁决）。
- 与 `REQ-01M7S5N4VG3GZXDQH1VY8FMS9Y`（配方缺 move/drag）同一边界：本单的 S6 是它的实施面。
