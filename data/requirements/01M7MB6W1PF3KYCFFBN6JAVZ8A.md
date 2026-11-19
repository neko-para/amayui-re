# SC0000 曾出现持续黑屏（用户口径）——HEAD/mac 不复现；待 win32 呈现面读数

- id: REQ-01M7MB6W1PF3KYCFFBN6JAVZ8A
- type: bug
- status: done
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- repro: docs/03-emulator/scripts/SC0000.md#黑帧读数（2026-11-17 / macOS arm64 / HEAD 21c6bcd）
- severity: S2
- done_reason: 用户裁决（2026-11-19，本轮唯一裁决来源）：此单此前已修好、只是没关。证据：①P3 `c79da47` + P4 `f7f708d` 均已是 HEAD `d55b2c3` 的祖先；转场走 FX 路径（`apps/emulator-lift/src/runtime/d3d9.h:319-320` 报 PS 2.0 / VS 1.0；`apps/emulator-lift/src/shims/d3dx9_effect.cpp:836-837` 的 203 两条 technique 已进实现集；`apps/emulator-lift/src/runtime/d3d9_pipeline.cpp:4606` 未实现即 `pipelineTrap`），不再有「203 被拒 ⇒ 整段黑」这条路。②mac 三入口读数不复现持续黑：SAVE00=76/77 首帧 99.491% 非黑 / 204,220 与 204,138 色，黑只出现在 78 入口的 8 帧过渡；`--present window` 的窗口回读与 guest 后缓冲逐项相同（`docs/03-emulator/scripts/SC0000.md` 的「黑帧读数（2026-11-17 / macOS arm64 / HEAD 21c6bcd）」）。③`docs/03-emulator/d3dx-effect.md` §9/§10：FX 路径用户验收「与真机差不多了」，203 真画后转场由纯黑变为模糊的塔（`SN0000 pos=0xD190`：44 采样 1 个 hash ⇒ 25 采样 25 个 hash；nonblack 0% ⇒ uniq 3.2 万–28.9 万、亮度 244→102）。④`docs/03-emulator/audio.md` §5（win32）加载配方能到 SC0000、正常结局；文末「复测」（mac / HEAD `00af635`）缺省 caps 下黑区间 2.60 s（帧 1761–2671），不是持续黑。缺口：仍没有 win32 的「窗口回读 vs guest 后缓冲」像素读数（需 win32 装机，对应 `REQ-01M7JN5Z1GZ8BTB9FVY8AVY381`），故不挂 verify（游戏画面是产品观察，按 AGENTS §12.4 不进默认门禁）。保留 repro 作重开条件。
- tags: [emulator-lift, render, sc0000]

## 现象（用户口径，2026-11-17）

"之前能执行到 SC0000，但渲染出来一直是**黑色**的 ⇒ 部分 shim 也许实现了，但功能是错的。"

## HEAD 上的读数（macOS arm64 / 21c6bcd / `--game-dir links/raw`）

**不复现持续黑**（三个入口都测了，像素直读见 `docs/03-emulator/scripts/SC0000.md`「黑帧读数」）：

| 入口 | 首帧 | 画面 |
|---|---|---|
| `SAVE00=76`（第一个战斗前） | 99.491% 非黑 / 204,220 色 | 遗迹 CG + 名牌 + 对话框 |
| `SAVE00=77`（SC0000 入口） | 99.491% 非黑 / 204,138 色 | 同上 |
| `SAVE00=78`（SN0000 结尾→点击） | 100% 黑 **8 帧（≈0.3 s）** → 白 12 s → 淡入章节卡 | 正常过渡形态 |

> ★ 2026-11-18：这三行的样本（`corpus/fixtures/SAVE7{6,7,8}.*`）已按用户裁决删除 ⇒ **读数不可逐字节复现**；
>   重跑请用 `saves/` 里的槽（00–04），那是另一份数据，**只能重取“黑/不黑”这一类读数**，不能当成同一次观察。

黑帧那一拍的 trace：`SetRenderTarget(后缓冲) → Clear → 3.94e9 条指令零 shim 调用` ⇒ **`Clear` 之后没有
`Draw*`**，不是"画了但画成黑"；`D3DXLoadSurfaceFromSurface` 的未实现臂会 `d3dxTrap` **响亮停**、不会静默黑。

## 最可能的落点：**宿主呈现/光栅面**，不是 shim

mac 走 `sdl-gpu/metal/MSL`，win32 走 D3D12/DXBC —— 两条光栅后端不同。若用户是在 win32 上看到的持续黑，
那与本机读数不矛盾：guest 后缓冲可能是对的、**窗口/呈现**那条路是黑的。

## 要收的读数（在 win32 上）

1. 同一入口（appdata 的 `SAVE00 = saves/SAVE00.DAT` —— ★ 2026-11-18 起 `corpus/fixtures/` 的存档载荷已按
   用户裁决删除，规范样本在仓库根 `saves/`（用户自维护：**只读复制**，⛔ 不就地改、不新建 / 不删）；
   删 `SAVE.DAT/BAK/RT.DAT`）跑到 SC0000，
   `emu-ctl screenshot <bmp>`（⛔ 不要用 `paused_at=block` 的旧口径）⇒ 非黑比例/唯一色数；
2. `--frame-log` 的逐帧 hash 是否恒定（恒定 = 没画）与 AOT 摘要的 `present :` 行
   （`sinkFailures` / `freezeFailures` / `readback_ns`）；
3. `--present window` 与 `--present headless` 各一次，对比"窗口回读 vs guest 后缓冲"
   （mac 上两者逐项相同，见上表）。

## 判据

- 窗口回读非全黑且随帧变化 ⇒ 不是 shim，是呈现链；
- guest 后缓冲全黑而窗口也黑 ⇒ 回到 `Clear`/`Draw*`/`SetRenderTarget` 三者的入参读数（需 opt-in 只读行）。
- ⛔ 在拿到上面的读数之前，不要为"黑"去补 `GetRenderTarget`/`CreateRenderTarget`/`StretchRect`/`ColorFill`
  （本装 0 调用）。
