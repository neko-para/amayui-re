# 残差第七批：按住派发按键族 · 种类 0 出口 · 深度 = 统一文件 id

- id: REQ-01M5N11MSKNRP0FZJXAHTKFX5J
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-input-hold.test.mjs#★ 0x104：反向取最高位、上限 0 走无按键编号
- tags: [spec residual]

# 残差第七批：按住派发按键族 · 种类 0 出口 · 深度 = 统一文件 id

## 一句话

把 `0x102` / `0x103` / `0x104` / `0x105` / `0xfc` 这一组未成篇的输入表指令一次写成规则并落地；顺带收口种类 0 出口、种类 2 矩形偏移、`0x7c` 深度身份三条窄残差。audio / text-system / Live2D 仍跳过。

## 新规则（5 条）

- `CORE-INPUT-102-SLOT`：键 0..31 / 槽 0..9 / 目标参数写入「每（帧 × 键）10 个槽」表；越界先抛、一槽不写；目标原样、−1 = 取消；与 `0xfb` 的按键回调表不是同一张。
- `CORE-INPUT-103-MODULUS`：模数原样（不检查、含 0/负数）；只清当前帧按住游标；模数初值 0（启动复位本体与 `0xfc` 都写）。
- `CORE-OPS-INPUT-104`：反向扫最高置位；上下文闸；掩码命中 ⇒ 返回点 = 本条自身 + 按键扫描上限夹到该键号（查表前、不撤销）；未登记 ⇒ 撤销返回点、推进 1；每条出口 `(游标+1) mod 模数`；模数 0 = 整数除零 UB（不兜底）。
- `CORE-INPUT-105-MASK`：32 位掩码原样写；唯一读者 = `0x104`；初值 0；不按帧。
- `CORE-INPUT-RESET-FC`：清两张回调表（全部帧）+ 模数 ← 0；不碰掩码/游标/上限/输入字。

## 闭合 / 移出（3 条）

`RES-SCENE-COMMAND-KIND0-OUTPUT` · `RES-SCENE-COMMAND-KIND2-RECT-OFFSET` · `RES-LOCAL-ENTER-DEPTH-NAME`；新增 `RES-SCENE-COMMAND-KIND0-ALPHA` / `RES-INPUT-103-NEGATIVE-MODULUS` / `RES-INPUT-104-CURSOR-INIT`；`RES-SCENE-DISPLAY-FIT-MATRIX` 扩写。

## 纠错 / 补强

- 种类 0 也有"命令自己的纹理"通道：区间 A → 画布 36、区间 B → 画布 37，再合成到记录纹理（进度 0..1；Fade / 默认两笔）；能力档 < 2 时每块先多一趟"区间之下"。
- 种类 2 抹除矩形：窗口态偏移 0、占满屏态非 0（视口原点 + 内容缩放；减位置、不减四边；组合次序见残差）。
- `0x7c` 的"深度" = 统一文件 id（区号 + 条目下标；帧装载抄入；`0x3` 第 1 操作数）；异常消息解析成名字。
- `0x105` falsifier 修正（原"先 7 后 3"在 7 仍按住时不可能）。

## emulator-new 落地

- `HoldState` + `Frame.holdSlots` / `holdCursor: number|null`（不填 0）。
- 五个 handler；0x104 模数 0 与未初始化游标显式抛；0xfc 清两张表 + 模数 0。
- 守卫：`emulator-new-input-hold`（9 条）· `spec-sites-batch7.assets`（62 站点）；变异 +4。

## 仍开着

`RES-SCENE-COMMAND-KIND0-ALPHA`（画面极性）· `RES-INPUT-103-NEGATIVE-MODULUS` · `RES-INPUT-104-CURSOR-INIT` · `RES-SCENE-DISPLAY-FIT-MATRIX` · 命令档执行仍未建模。

## 判据

`pnpm test` 939/0 · `pnpm test:assets` 见收口记录 · `pnpm test:mutation` 311/311 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。复审：只读 `data/spec` 两轮，S1 8 → 待确认（已逐条修订）。

## 怎么查

`pnpm tools spec show CORE-OPS-INPUT-104` · `docs/02-engine/spec-audit-2026-10.md` §24 · `tools/test/emulator-new-input-hold.test.mjs`。
