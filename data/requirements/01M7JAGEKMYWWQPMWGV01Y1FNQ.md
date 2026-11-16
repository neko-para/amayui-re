# 🐞S2 screenshot 全黑：dumpLastFrame 保留的最后一张 surface 没有像素（同帧 --dump-frame 正常）

- id: REQ-01M7JAGEKMYWWQPMWGV01Y1FNQ
- type: bug
- status: done
- parent: REQ-01M7HC1WJ9X0QCQ51Q9F01988W
- verify: apps/emulator-lift/tests/control_snapshot.cpp#screenshot-freezes-frame
- repro: apps/emulator-lift/src/runtime/present.cpp#dumpLastFrame
- severity: S2
- tags: [emulator-lift, control-plane, present]

## 现象

`emu-ctl screenshot`（服务端走 `Presenter::dumpLastFrame`）写出来的 BMP **全黑**，而**同一帧**用 `--dump-frame`
（`SurfaceArena::dumpBmp`，在 present 汇点内当场写）却是正常画面。

## 根因（2026-11-16 取证；原「猜测」已被确认并修正措辞）

1. `present()` 只留**描述符**（`last_arena_` / `last_surface_` / `last_surface_hash_`），**不拷像素**；
   `dumpLastFrame` 在**暂停那一刻**才去读 guest 像素。
2. 那块 guest 后缓冲**只在 Present 那一刻是完整一帧**：GPU 开着时 D3D9 绘制只写 GPU target，guest 只在 `present` 里回读；
   而帧首 `Clear` 会**逐像素直写 guest**（清成 0）⇒ 从 Clear 到下次回读之间，guest 里就是清除色。
3. `--dump-frame` 恰写在回读之后、`present` 之内 ⇒ 它永远对；`dumpLastFrame` 写在暂停时刻 ⇒
   取决于暂停落在哪：落在 **Present 闸门**＝有图，落在**块边界**＝黑。

### 实测（win32，2026-11-16，`--present headless`，连续 `resume --for-ms 40` + `screenshot`）

| 暂停落点 | 读数 |
|---|---|
| `paused_at=block` frame=542 | 非黑 **0/921600 (0.000%)** · `hash_still_matches=false` |
| `paused_at=present` frame=547 | 非黑 **918642/921600 (99.679%)** · `hash_still_matches=true` |

⇒ **推翻了本单原写的"headless 结构性黑"**：headless 也能有画面；决定因素是**暂停落点**（机制见上）。

## 修法（2026-11-16 落地，最小改法）

`present()` 在 `last_surface_hash_` 定稿之后、交给 sink **之前**，把该帧像素（**连描述符一起**）冻结成一份副本；
`dumpLastFrame` 冻结有效就写副本、否则**退回写现场**（= 今天的行为）。**只在 `--control-root` 时拷**（缺省零成本）。
`surface` 抽出 `dumpBmpRaw(desc, src, …)` 从副本写盘；`hash_still_matches` 改成在**实际写出的那份字节**上算。

## 判据（能红）

1. `--present headless` 跑到 ≥30 帧后 `screenshot` ⇒ 非黑像素 > 50%（与同帧 `--dump-frame` 对照）。
   **修后读数**：6 次 resume+screenshot **全部 99.679%**，含 `paused_at=block` 的那几次
   （独立复审自建目录复跑：`paused_at=block` frame=98 → 99.679%）。
2. `--present window` 同样 —— **<未取证>**（本轮只跑 headless）。
3. pure 守卫 `apps/emulator-lift/tests/control_snapshot.cpp#screenshot-freezes-frame`：
   present（同时写一份参照 BMP）→ 把**现场**像素 `memset` 成 0 → `dumpLastFrame` 写出的 BMP 必须与参照**逐字节相同**。
   **修前红**（`shot==ref=0`，`90 checks, 2 failures`），修后绿（`90 checks, 0 failures`）。
   ★ 它不是自证：参照走 `arena.dumpBmp`（现场）、清的是现场、截图读的是 `frozen_pixels_`。
4. 变异清单一条：删掉冻结那一行 ⇒ 上面那条用例当场红（`ctest` exit 8）。**实测红过**。

## 收口（2026-11-16）

- `verify: apps/emulator-lift/tests/control_snapshot.cpp#screenshot-freezes-frame`
- 独立复审裁决 **approve-with-fixes**（`.tmp/shot-defect/change-review.md`）：修法 / 冻结点正确、
  守卫不是自证、实测能红、端到端 3 次 ≥99% 非黑。三处文本修补（失败计数可见、合同注释、客户端帮助）同轮带走。
- **契约口径已改**：`hash_still_matches` 在冻结路径下**恒 true**，只回答"冻结有没有生效"（删掉冻结会变 false）；
  ⛔ 不再声称它验内容 ⇒ M0 判据 5 因此补了"BMP **非全黑**"（`REQ-01M7HX0CJ202MJ4PZXW80VFA0J`）。

## 残余（⛔ 不在本单）

- 冻结**分配失败**时退回写现场（极端情况仍可能写出一张全黑图），只 warn 一次 + 计数（`freezeFailures=` 进 AOT 摘要）。
- window 档端到端、16bpp 运行时、DDraw Flip 路径未实测。
