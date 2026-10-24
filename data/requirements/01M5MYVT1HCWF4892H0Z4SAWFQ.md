# 残差第六批：进入本地块 · 种类 2 全字段 · 日志路径

- id: REQ-01M5MYVT1HCWF4892H0Z4SAWFQ
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-ops-100-200.test.mjs#★★ 真跑 `0x199` → `0x7c`：不再抛深度异常（身份来自建帧路径，⛔ 不手工赋 localDepth）
- tags: [spec residual]

# 残差第六批：进入本地块 · 种类 2 全字段 · 日志路径

## 一句话

收口交接单里可静态取证的一批：未成篇的 `0x199`（进入本地块）、种类 2 的完整字段/校验/出口、遮罩算术、图形剩余三条窄残差（日志路径 / 诊断格式 / 旧式降级实参）、`[20,30)` 命名。audio / text-system / Live2D 仍跳过。

## 已闭合 / 移出（4 条）

`RES-SCENE-COMMAND-KIND2-LAYOUT` · `RES-BOOT-GFX-RESOLUTION-DIAG-FORMAT` · `-LEGACY-DOWNGRADE-EFFECT` · `-LOGSINK-PATH`；新增 `RES-LOCAL-ENTER-DEPTH-NAME` / `RES-SCENE-COMMAND-KIND2-RECT-OFFSET` / `-KIND0-OUTPUT` / `RES-BOOT-GFX-LOGSINK-PATH-DATA`；`KIND1-FLAG` / `TEX-BELOW-RANGE` 收窄。

## 合同变化

- 新规则 `CORE-OPS-STATE-199`：推进格数 1、无条件清输入字；存档字第 26 位两形态（首次进入写返回点 + 入块深度 + 状态字整字搬走；已在块内读-改-写、不写返回点/深度）。
- `0x7c` 深度判据改为「本帧身份编号 vs 入块深度」，写者 = 0x199。
- 种类 2：10 操作数全映射；类型判据 **[0,11] 含负数**（纠错）；分割幅<1 夹 1 / 类型 −1 / 延迟持续清 0、跳过类型校验照写；两条校验后才发建/换请求；两段各喂一块画布、每帧都画；抹除矩形来自绘制项表同键条目、缺它 ⇒ 清持续时间并中止整趟；类型<0 ⇒ 场景「强制完成」。
- 遮罩：只改源像素最高字节、RGB 逐位不变、255 不是恒等；每帧尾段混合（源=画布、目标=影片纹理）。
- `70-vm` 新增「强制完成」档（本帧有效、不跨帧）。
- 日志路径全量（分隔符只在结果非空时补、256 字节、探存在性+建目录、恰为 1 ⇒ 本地应用数据、缺键弹框属表现层、打开时机）。
- 旧式降级五条请求 + 标志只在第二条件置位且无读点。

## emulator-new 落地

- `0x199` handler；`freshFrame(id)` 写 `localDepth`（修掉 0x7c 真跑必抛）；种类 2 handler 全字段 + 校验 + 请求。
- `joinEnginePath` / `ErrorLogPathSpec` / `openErrorLog` / `appDataDir`；`resolveLogSink` 后移；`config.has`；诊断②带 format。
- 守卫：`spec-sites-batch6.assets`（0x199）· `emulator-new-ops-100-200`（0x199 两形态 + 真跑 0x199→0x7c）· `-scene-commands`（种类 2 四条）· `-graphics-params`（路径 7 例 + 落点/时机/格式）；变异 +6。

## 仍开着

`RES-SCENE-COMMAND-KIND0-OUTPUT` · `-KIND2-RECT-OFFSET` · `KIND1-FLAG`（画面极性）· `RES-LOCAL-ENTER-DEPTH-NAME` · `RES-BOOT-GFX-LOGSINK-PATH-DATA` · `-LOGSINK-SINK2` · `RES-TEX-...-OOB` / `-SELFTARGET` / `-BELOW-RANGE`。命令档执行（两画布/两区间/12 图案/缺项中止）仍未建模；`0x104` 与它的 0x102/0x103/0x105 仍未成篇；显示适配未接 renderer。

## 判据

`pnpm test` 930/0 · `pnpm test:assets` 见收口记录 · `pnpm test:mutation` 307/307 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。复审：只读 `data/spec` 两轮，S1 5 → 0。

## 怎么查

`pnpm tools spec show CORE-OPS-STATE-199` · `docs/02-engine/spec-audit-2026-10.md` §23 · `tools/test/emulator-new-ops-100-200.test.mjs`。
