# AOT · 配方层没有鼠标移动/拖拽原语：场景 1 的"向左移动"与场景 2 的"上下拖拽"无法复现

- id: REQ-01M7S5N4VG3GZXDQH1VY8FMS9Y
- type: req
- status: done
- parent: REQ-01M7JHSFB2SSBBNSJ82YDED8M1
- verify: apps/emulator-lift/tests/input_inject_visible.cpp#inject/drag
- done_reason: S6（0a720cf）给 --input-plan 加了鼠标原语：mouse_move <x> <y>（1280×720 缩放同 click）、mouse_down/up [left|right]（用设备当前位置、up 不回跳），--help 同表更新；拖拽由 inject/drag 覆盖（down A → move B → up B，up 的 lParam 是 B、只有一对 down/up），解析负例（缺参数 / 无前置 move / 未知按钮 / 挂 timeout）在 input-plan-read-points。判据 4（resume --for-ms 到点重停）已在 tools/emu-ctl.mjs:55/112 文档化。
- tags: [emulator-lift, input-plan, control, tooling]

## 现象（2026-11-19 / macOS arm64）

`--input-plan` 是 AOT 臂**唯一**的配方入口（`--help` 自称"⛔ 没有'本入口不认'的指令"），但它**没有鼠标移动原语**：
只认 `await script` / `await position` / `wait` / `click` / `profile_begin|end` / `quit`。
写 `mouse_move 440 360` ⇒ **exit 2**（`未知指令`）。

后果：**"鼠标连续移动 / 拖拽"这类用户操作无法用配方复现**。用户报的两个卡顿场景都卡在这里：

- 场景 1（SAVE05 → 点击 440,360 → **向左移动**）：只能点、不能移；`click 440 360` 的实际效果是关掉教程叠加层，
  与"移动过程中和移动完成后都卡"不是同一段输入。
- 场景 2（OPTION **上下拖拽滚动条**）：`click` 只能点轨道，拖拽（down→move→up）根本表达不了。

控制层 `emu-ctl move` 是替代品，但 `ControlOp::Move` **只接受暂停态入队**（`not_paused`）⇒ 每步要
`pause → move → resume`。而 `resume --for-ms N` 到点会**重新暂停**：一串循环的最后一拍会把 guest 留在暂停态
（实测：整个游戏停住；那一版 profile 的 `wall` 109 s / 273 帧里大部分是暂停时间）。收尾必须发
**不带 `--for-ms`** 的 `resume` 才是持续运行 —— 这条只在源码里，文档没写。

## 判据（实施时写 verify / 手动验收）

1. 给 `--input-plan` 加**运行态**的鼠标移动原语（至少 `move <x> <y>`；若要拖拽再加 `mouse_down` / `mouse_up`），
   坐标口径与 `click` 一致（1280×720 参考系、投递前按客户区缩放），并更新 `--help` 的同一张表。
   ⛔ 不要恢复已退役的 `--input-script` / 图语法；⛔ 不做"help 有、解析器没有"的第二种语法。
2. 或者：明确**不做**，把 `--help` 里"唯一一份配方语法"的说法收窄，并在配方错误文本里点名
   "移动/拖拽走控制层 `emu-ctl move`（只在暂停态入队）" + 文档写清 `resume --for-ms` 的收尾语义。
3. 验收（无论选 1 还是 2）：一条能覆盖"移动/拖拽"的配方（或控制层脚本）能重复跑出与用户相同的输入序列，
   且 `input-plan` 的 `pending=0` / 门的 timeout 不误报。⛔ 不以"画面看起来动了"为判据。
4. 同一次修复里把 `resume --for-ms N` 的"到点重新暂停"写进 `--help` 或 `tools/emu-ctl.mjs` 的用法说明
   （今天只在 `control_ws.cpp` 的实现里）。

## 边界

- 只动输入控制面（`src/runtime/input_plan.*` / `recipe.*` / `aot_main.cpp` 的 usage / `tools/emu-ctl.mjs` 的文档）；
  ⛔ 不碰 guest 输入语义（`runtime/input.h` 仍是唯一施加路径）。
- ⛔ 不为复现某个场景改模拟器行为；⛔ 不把 `move` 做成"绕过暂停的直接写内存"。
- 与"`--present-hidden` 的 Present 病理值"不是同一件事（那条只有一次观测，尚未做 visible/hidden A/B，未立项）。
