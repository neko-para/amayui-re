# emulator-cxx · ADV 抽干：按键 / 跳过时把剩余待画格一次画完（输入桥信号）

- id: REQ-01M61PZ9G8BH81A1P6BVNMAJXM
- type: req
- status: done
- parent: REQ-01M618MSP80J9FHXB4JS4VE1J5
- verify: apps/emulator-cxx/CMakeLists.txt#emulator-core-adv-drain

## 已落地（2026-10-29）

- **信号**：`Machine::set_input_pressed(bool)` —— 宿主每帧喂一次"有输入在按下"（键盘位图非 0 或鼠标左键按下）。
  它是主循环**被动观察**输入，⛔ 与脚本主动取键（`0xff` / `0x109`）不是一回事。
- **抽干**：`service_text_layers()` 在它为真时把**剩余待画格在同一帧内**画完（引擎 `0x4096e0`–`0x4096f4`
  的 do-while：每轮仍走"推进一格"那条路，循环到队列空）；为假时仍是一帧一格。
- **宿主接线**：窗口路径每帧算一次 `sample_key_mask() != 0 || 鼠标左键按下` 喂进去
  （`window.cpp` 的 per-frame 循环，与 `set_host_clock_ms` 同一处）。
- **守卫**：CTest `emulator-core-adv-drain`（`--only-drain`）两半判据：
  ① 无输入时一帧只多画一个字（待画格 3 → 2）；② 报"有输入"之后**一帧**抽干（待画格 → 0）。
  变异（把 `if (!drain) return;` 去掉 = 无条件抽干）⇒ `无输入时一帧画了不止一个` 当场红。

## 没做

- 脚本化输入（`--input-script`）路径没有喂这个信号：那里没有"当前按住"的概念（按时间线注入单次事件），
  要喂得先给 `ScriptedInput` 加"事件发生的那一帧"。CTest 用不到（守卫直接置标志），留给将来需要时再补。
