# 输入注入：shim 侧虚拟鼠标（一台设备 / 四个驱动者 / SDL 只作可选驱动）

- id: REQ-01M7SB9R0BAYYY4H1VSHZA4GAJ
- type: req
- status: done
- parent: REQ-01M7JHSFB2SSBBNSJ82YDED8M1
- done_reason: 用户裁决 2026-11-19（Q1/Q2/Q3/Q6/Q8 按推荐）；S1–S9 全部落地（S1–S3 逐字节不变、S4 单一租约 + Driver::Guest + 转移投影（含 Script up 不先移动的 follow-up）、S5 overlay hover 读设备、S6 配方 mouse_move/down/up、S7 一读点一条写动作 + 连点串行化、S8 文档、S9 注入 click 过 UiClickRouter + --ui-input=consume|pass）。判据：全量 ctest 128/128（4 skip）；input-injection 441 checks / input-plan-read-points 743 / control-endpoint 530，0 failures；语义片各有新子用例。残留（文档已写清）：mouse_move/down/up 原语不过 router；控制层 consumed_by_ui 字段；window.key/mouse/cursor 站点名无断言；--ui-watch/--ui-strict 未实现。
- tags: [emulator-lift, input]

## 一句话

把输入注入做成 **shim 侧的一台虚拟鼠标**：设备只有一份状态（guest 屏幕坐标 + 电平 + 来源 + 单一租约），
SDL / 配方 / 控制层 / 机器内驱动器都只是**驱动**它；shim 的读点只读设备，**不要求 SDL 窗口或事件泵存在**。
用户口径（2026-11-19）：“shim 层需要一个‘虚拟鼠标’，而 input-plan 和 SDL 的输入都只是控制鼠标移动，
而不是两遍分别组合。需要确保 shim 看到的状态是唯一的，不依赖 SDL。”

## 现状审计（2026-11-19 立单时）

- **已满足**：guest 可见输入的事件写入路径只有一条（当时 18 个 `input::*` 落点 + 5 个会话写；S4 后是 17 + 5，
  见 `input-plan.md` §2.3）；读点只读那一份状态；headless 仍建隐藏 SDL 窗口 + GPU，缺的只是 sink 与事件泵。
- **未满足**：设备不是“一台设备”——6 组字段 + **3 套所有权**（`key_owner_` / `button_owner_` /
  `cursor_script_owned_`）+ 零调用者的 `capture_`；SDL 层还持有第二份“鼠标在哪 / 点了没”
  （overlay hover 读 `SDL_GetMouseState`、`click_router`、`warp_echo`）。
- **“两遍分别组合”的 5 个形状**：①同一个 `click` 由 plan 与 `InputScript` 两套 hold 策略组合；
  ②overlay 是 OS 路径上的第二道闸门；③`--cursor-warp` 要独立回灌抑制；④`SetCursorPos` 是隐藏推进点；
  ⑤`MSG.time` 有三个来源。

## 设计（已实施；合同 = `docs/03-emulator/input-device.md`）

- `runtime/input_device.{h,cpp}`（`InputDevice`）由 `WindowManager` **以成员持有**；设备只存 guest 屏幕坐标 +
  256 键电平 + 逐电平来源 + 单一 `Driver` 租约，**无时钟 / 无队列 / 无窗口**；写 API private + `friend`。
- `runtime/input.{h,cpp}` 是“设备写 + 消息投影”的唯一组合点：消息字段**写后从设备回读**，**只在电平转移时 post**。
- 四个驱动者（SDL / 配方 / 控制层 / `InteractionGraph`）都只调 `input::*`；SDL 适配器只做坐标换算，不持有状态。
- S9：注入 `click` 的三段都在 `input::*` **之前**问 `UiInputRouter`（与 OS 同一个 `UiClickRouter`）；
  `--ui-input=consume|pass`（缺省 consume；窗口参数）。

## 切片（全部已落）

| 片 | 主题 | 行为 |
|---|---|---|
| S1 | 设备实体（状态搬家） | 逐字节不变 |
| S2 | 消息 = 设备的投影（写后回读） | 逐字节不变 |
| S3 | 按钮位图 = VK 电平的投影 | 逐字节不变 |
| S4 | 单一租约 + `Driver::Guest` + 转移投影 | **变**（guest 可见） |
| S5 | overlay 指针改读设备 | **变**（诊断/产品面） |
| S6 | 配方 `mouse_move` / `mouse_down` / `mouse_up` | 增量（旧配方不变） |
| S7 | 一读点一条写动作 + 连点串行化 | **变**（guest 时序） |
| S8 | 文档（`input-device.md` / `MSG.time` / 过期判据） | 无代码 |
| S9 | 注入 click 过 router + `--ui-input` | **变**（HUD 命中时 guest 收不到点击） |

## 裁决（2026-11-19，用户：“无问题，按这个策略处理即可”）

- **Q1**：1→0 且来源是 OS 才 post；Program-owned 不动；失焦 / 暂停 post up。
- **Q2**：接受“一个读点最多一条写动作”（含 `drainImmediate`）。
- **Q3**：`mouse_move` + `mouse_down/up`；down 轻闸门、up 直写、滚轮不被光标写失败丢弃。
- **Q6**：A —— 注入点击过 router，并实现 `--ui-input`。
- **Q8**：A —— 删除连点强制抬起，连点串行化。
- Q4 取消；Q5 选 B（`MSG.time` 只写文档）；Q7 = `Driver::Guest`。

## 收口凭据

- `verify` 锚 S9 的 `input-plan/click-consumed-by-ui`；`done_reason` 列全 S1–S9 与其余新子用例。
- 合同文档：`docs/03-emulator/input-device.md`；配方 / 投递：`input-plan.md`（九条指令、一读点一写动作）。
- 门禁：全量 ctest 128/128（4 skip）· `pnpm test` 590 pass · `pnpm verify` 500 pass / 2 skip。

## 残留（文档已写清，⛔ 不静默）

- `mouse_move` / `mouse_down` / `mouse_up` **原语不过 router**（S9 只接 `click`）。
- 控制层 `consumed_by_ui` 字段未做；`InputPlan::uiConsumed()` + 摘要 `ui_consumed=N` 已做。
- `window.key` / `window.mouse` / `window.cursor` 的 off-thread 站点名仍无断言（只钉了 `find` / `post`）。
- `--ui-watch` / `--ui-strict` 仍未实现（既有文档债）。

## 边界

- ⛔ 不恢复 `--input-script` / 图语法；⛔ 不引入第二份状态；⛔ 不把 SDL 事件直接当 guest 状态。
- 语义片（S4 / S5 / S7 / S9）都单独验收并留了“变”的读数；⛔ 不要把它们混进“逐字节不变”的片。
- 与 `REQ-01M7S5N4VG3GZXDQH1VY8FMS9Y` 同一边界：本单的 S6 是它的实施面。
