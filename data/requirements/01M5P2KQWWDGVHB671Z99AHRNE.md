# 🐞启动约 1s 的整屏白闪（残余）

- id: REQ-01M5P2KQWWDGVHB671Z99AHRNE
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- done_reason: 用户决定移除 emulator-new 的 Electron 白闪探针测试项（assets 档、依赖真窗口），后续不再维护；白闪残余按用户决定收口，不再有自动判据。
- tags: [bug render]

## 症状

启动后约 1s 整体白屏闪一次；`--skip-logo` 也一样 ⇒ 渲染系统层。

## 已定位并修的两条高置信路径

1. **窗口先 show、画布后才有第一笔**：日志次序 `ready → createWindow → showWindow → init`；canvas 在 `PixiRenderHost.create` 挂上后，直到 `init` 才第一次 `render` ⇒ show 出来那一瞬是“没画过的 WebGL 画布”。
   - 修复：`create` 里 append 后立即 `render({EMPTY_CONTAINER, clear:true})`（背景 0 / alpha 1 = 黑）。
2. **释放纹理时用旧快照中途 present**：`releaseTexture` 里 `scrub` 只剥纹理 id，**网格不引用纹理** ⇒ 版权页的白底网格仍留在旧快照里；`0x1fa` 那一刻核心已前进，画出来就是“上一页白底闪一帧”。
   - 修复：`releaseTexture` **不 present**；`session.ts` 置 `lastScenePushAt=0`，让下一次帧边界推**新快照**再画；`present` 加 `trigger(frame|movie|release)` 诊断（出现 `release` = 有人又加回去了）。

## 辅助（不是根因）

- 空白槽（`0x1f8` 建好、未写像素）在绘制项里**不画**（`blankSlots` + `blank-slot-skipped` 诊断）；未写过的 RT 是未定义像素，画出去会让前景随机。
- 影片覆盖层在快照里无引用时只记 `movie-overlay-unreferenced`，不擅自跳过（有影片本来就不在绘制项表里）。

## 验证

- `pnpm test` 1009/0 · `pnpm typecheck` 六工程绿。
- **必须 `vite build` 后复测**；若白闪仍在，看白闪前一瞬日志：
  - 第一条 `uploadTexture` 之前 ⇒ 路径 1；
  - 紧跟 `releaseTexture(…)：本帧不呈现` ⇒ 路径 2；
  - 紧跟影片首帧 / `movie-overlay-unreferenced` ⇒ 影片覆盖层。

## 怎么查

`tools/test/emulator-new-scene-order.test.mjs`（剥离只删纹理引用、网格保留）· `apps/emulator-new/src/platform/electron/renderer/{pixi-host,session,scene-order}.ts`。

## 自动判据（已按用户决定移除）

原 `tools/test/emulator-new-flash-probe.assets.test.mjs`（Electron 真窗口 + `links/install`，跑 150s）
在 2026-10-28 被用户要求移除；原因：它会打开 emulator-new 窗口，且后续不再维护。
⇒ 本单改为 `done` + `done_reason`，不再挂 `verify`。白闪残余仍可按上面“怎么查”人工/真机目视。
