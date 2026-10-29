# emulator-cxx · ADV typewriter：绘制侧每帧推进一格（追加时一次性写布局格）

- id: REQ-01M61PMX8K8K7ZPD50WZSMZ97P
- type: req
- status: done
- parent: REQ-01M618MSP80J9FHXB4JS4VE1J5
- verify: apps/emulator-cxx/CMakeLists.txt#emulator-core-adv-typewriter

## 已落地（2026-10-29）

- spec `62-adv-text.md` 的 `ADV-TEXT-APPEND` 已写入：追加时一次性写布局格与文本记录、**绘制侧每帧推进一格**、
  帧间按 `message:MessageSpeed` 睡眠、按键 / 跳过时抽干。
- 实现：`Machine::pending_glyphs_`（跨帧队列）+ `service_text_layers()`（每次 `step()` 画出一格，
  **不受** `paused` 影响 —— 等待输入时字还在打），`0x71` 清画布时同时丢掉该层的待画格。
  布局（量度 + 折行 + 逐字符条目）仍在追加时一次算完。
- 守卫：CTest `emulator-core-adv-typewriter`（`--only-typewriter`）三半判据：
  ① 追加那一步之后待画格数 = 字符数、**画布全透明**、三个布局条目**已排好**；
  ② 每走一步待画格 −1 且画布出现墨；③ 走满后队列空。
  变异（把"追加即全画"写回去）⇒ `追加那一步就落了像素 —— 引擎是每帧只推进一格` 当场红。

## 剩（新单 `REQ-01M61QK1…`）

- **抽干**：引擎在有按键 / 跳过时把剩余格一次画完（`0x4096e0` 起的 do-while）。我们的输入桥还没有
  "本帧有按键"这个信号暴露给文本层 ⇒ 单开一条。
- 帧间睡眠 `message:MessageSpeed` 是宿主时钟（`--tick-ms`），不是像素判据。
