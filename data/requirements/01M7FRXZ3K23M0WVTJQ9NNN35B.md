# M-lift-3 · 图形 shim 实现效率：端到端墙钟的 78% 在这里（DrawIndexedPrimitive 175 µs/次 · Clear 5 ms/次 · Present 8.4 ms/次）

- id: REQ-01M7FRXZ3K23M0WVTJQ9NNN35B
- type: req
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- tags: [emulator-lift, d3d9, efficiency]

## 为什么单独立这一节点

`M-lift-2` 已经把"翻译"做到 native 量级（`OBS-20261115-06`）：**929M 条 guest 指令 / 1.6 s = 1.7 ns/条**。
但端到端墙钟（300 帧 **8.14 s**）已经由**图形 shim** 决定 —— 同一份 profile 里：

| 项 | 采样折算 | 占墙钟 |
|---|---:|---:|
| `IDirect3DDevice9::DrawIndexedPrimitive` | 18,000 次 × **174.9 µs** = 3,148 ms | |
| `IDirect3DDevice9::Present` | 300 次 × **8.43 ms** = 2,528 ms | |
| `IDirect3DDevice9::Clear` | 301 次 × **5.04 ms** = 1,516 ms | |
| `IDirect3DDevice9::DrawPrimitive` | 323 次 × **2.28 ms** = 737 ms | |
| `SetRenderState` + `SetTextureStageStage` | 370,850 次 × 1.18 µs = 438 ms | |
| **图形 shim 合计** | **≈8.62 s** | **≈78%** |

⇒ 每个都**比"它该有的成本"高 1–3 个数量级**。★ 早先（`OBS-20261115-04` 那次 50 s 的运行）shim 只占 13%（约 6.6 s）：
**shim 的绝对值没变，是别的东西变快了** ⇒ 这条线现在是端到端唯一的量级杠杆。

## 待查的假设（⛔ 先测再改，不要按假设动手）

- `DrawIndexedPrimitive` 175 µs/次 —— 疑似**每次 draw 重建 D3D12 管线/根签名/描述符**（18,000 次/300 帧 = 60 次/帧）；
  也可能是每次重建顶点缓冲映射 / 状态块。
- `Clear` 5.04 ms/次 —— 清一块 1280×720 是不是走了 CPU 或逐行 `memcpy`（正确实现应是几十 µs 或一次 GPU clear）。
- `Present` 8.43 ms/次 —— 3.7 MB 的呈现路径是不是做了格式转换 / 逐像素 / 多次拷贝。
- `SetRenderState` 1.18 µs/次 × 260,450 次 —— 单次不慢，但**次数太多**：是 guest 真的每帧设 868 次，
  还是 shim 内部有 O(状态数) 的重算（例如每次重算整条固定功能流水线的哈希）。

## 判据

1. **尺度**：300 帧墙钟（release，交错 A/B ≥3 对取中位）相对 `8.14 s` 下降；目标先定"图形 shim 合计 ≤ 2.5 s"（≈ −70%）。
2. **语义不变量**：`exit` / `frames` / `stop @ <EA>` / arena 三元组 / `table misses` **逐位相同**；
   ⛔ 不改 guest 可见语义（这条线是宿主实现效率，不是 D3D 合同变更）。
3. ⛔ **不得**用"跳过某个 draw / 少画一帧 / 少一次 Present"换速度 —— 那会改可见行为。
4. 每条热点 shim 的改法必须给**前后单次成本**（`--profile` 的 `shim:` 行前后对照），不只是总量。

## 边界

- 这是**图形实现效率**，不是翻译质量；⛔ 不要把它记进 `M-lift-2` 的账。
- 真实渲染后端的取舍（GPU 是唯一光栅实现的既有裁决、CPU 兜底已禁）在
  `docs/00-origin/decisions.md` §8.1 与 `apps/emulator-lift/README.md` §4.2b，⛔ 不重开那条。
