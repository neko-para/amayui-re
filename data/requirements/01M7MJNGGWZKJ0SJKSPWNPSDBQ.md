# 🐞S2 SN0000→SC0000 的「渐变到黑」被一段 39.5e9 指令的计算整段吃掉 ⇒ 渐变塌成一帧（疑似空转/等线程）

- id: REQ-01M7MJNGGWZKJ0SJKSPWNPSDBQ
- type: bug
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- repro: .tmp/sc0000-gfx/report.md#C1
- severity: S2
- tags: [emulator-lift, sc0000, script-wait]

## 一句话

SN0000 最后一句之后那段「横向模糊 + 渐变到黑」**不是没画**：渐变是**脚本层**做的
（`SN0000.BIN.txt` 第 2889 行 `label_0000b3b4` 起：`create-texture 1280×720` 抓屏 → `draw-texture`，
`label_0000b69c` 每步 `set-draw-color`（灰阶）+ `set-draw-color-alpha` + `i238 0x9C4` + `wait`），
**画与混合都通**（同一支 quad 在白闪段能从 α=0 爬到 255，100 帧、`fa=0→255`）。
真正的问题：**等待预算被一段超长计算整段吃掉** ⇒ 渐变余步在**一帧内**跑完 ⇒ 观感是"直接黑屏"。

## 读数（2026-11-17 / mac arm64 / `links/raw` / headless / RelWithDebInfo / 二进制 sha `8332003f…`）

- 画面序列：塔（被提亮，342,156 色 / 均值 145.7）恒定 **帧 570–1004** → 纯黑（1 色）**帧 1005–1612**
  → 白闪 1613–1918 → 章节卡淡入 1919+。
- 帧 1004→1005 之间 **846,141,019** 条 guest 指令；帧 1005→1010 之间 **39.49e9** 条（最大一段 **20.4e9**，
  边界全是 `HeapFree`→`HeapFree`，**0 次 D3D 调用**）。
- 黑段每帧仍有 16–17 笔 `ID3DXSprite::Draw` + 1 笔全屏 `DrawPrimitive`（FVF 0x244 / stride 36），
  全 run `[warn]` = 0 ⇒ **不是"Clear 之后没有 Draw"**。
- 预算判据：`wait` 的 handler（`.lst` 0x41A260）只把控制权交回主循环，**预算由主循环用 `timeGetTime` 判**
  （AOT 臂时钟 = wall clock，`aot_run.cpp:374`）⇒ 一段计算超预算之后，**后续每个 `wait` 立刻通过**（追赶语义）。
- 黑起点在 **SN0000 内部**（`sample` 读到 `script=SN0000.BIN pos=0xB7C4`）；SC0000 是"黑着被切进来"的。

## 要做什么（按证据顺序）

1. **先补读数**（低风险）：在 `i238` / `wait` 的 handler 之后打一行 `(pos, timeGetTime, budget)`
   （或挂进控制层 `sample`）；把 `LIFT_SLOW_DRAW` 的 `[draw]` 扩到 `ID3DXSprite::Draw`（color / texture / srcRect）。
2. **分诊那 20.4e9 / 39.5e9**：用 `--profile` + `--until` / `--max-insns` 二分到 1–2 个 EA。
   已知热点：`__ftol2_sse`(0x4E82C0)、fmul 循环(0x4A6808)。要回答的是"这是引擎真的在算（模糊/解码），
   还是我们在某处空转（等一个永远不会来的线程 / 轮询）"。
3. **只有第 2 步说是空转**才动实现；如果确实是引擎的模糊/解码计算，那 `wait` 的追赶语义就是**引擎行为**，
   ⛔ 不许改时钟或预算让它"看起来像"（先在需求单写裁决）。

## 判据（收口时）

- 那一段有**多级**灰阶帧（逐帧 `--frame-log` 的 hash 序列单调变化 ≥ N 帧），而不是一帧跳黑。
- 若是空转：给出"谁在等谁"的 EA 锚 + 一条能红的守卫（⛔ 不是"画面变好看了"）。
