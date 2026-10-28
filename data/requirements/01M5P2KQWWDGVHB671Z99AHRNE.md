# 🐞启动约 1s 的整屏白闪（残余）

- id: REQ-01M5P2KQWWDGVHB671Z99AHRNE
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-flash-probe.assets.test.mjs#启动期白闪：呈现过 ≥10 帧且没有一条 white present
- tags: [bug render]

## 症状

启动后约 1s 整体白屏闪一次；`--skip-logo` 也一样 ⇒ 渲染系统层。

## 已定位并修的两条高置信路径

1. **窗口先 show、画布后才有第一笔**：日志次序 `ready → createWindow → showWindow → init`；canvas 在 `PixiRenderHost.create` 挂上后，直到 `init` 才第一次 `render` ⇒ show 出来那一瞬是"没画过的 WebGL 画布"。
   - 修复：`create` 里 append 后立即 `render({EMPTY_CONTAINER, clear:true})`（背景 0 / alpha 1 = 黑）。
2. **释放纹理时用旧快照中途 present**：`releaseTexture` 里 `scrub` 只剥纹理 id，**网格不引用纹理** ⇒ 版权页的白底网格仍留在旧快照里；`0x1fa` 那一刻核心已前进，画出来就是"上一页白底闪一帧"。
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

## 自动判据尝试（2026-10-28，不需要人眼）

`apps/emulator-new` 的 renderer 里**自带白闪探针**：每次 present 读后缓冲中心 1 像素，接近纯白（RGB ≥ 235）就记
`[flash-probe] white present trigger=<trigger> target=… nodes=… textures=…`，最多 20 条，且只在启动后 120s 内生效
（`pixi-host.ts` 的 `probeFlash`）。`--smoke` 还能自动截图（`.tmp/emulator-new-electron-smoke.png`）。

实测（本机 macOS，Electron 可跑）：
```
cd apps/emulator-new && npx electron --no-sandbox . --install <repo>/links/install --skip-logo
```
- 启动链正常：`renderer ready（webgl 1280x720）` → `createWindow 1280x720` → `showWindow` → `boot：1280x720 · screenMode 1`；
- **60 秒运行期间没有出现任何 `[flash-probe] white present` 行** ⇒ 至少"后缓冲中心接近纯白"这条判据下，启动白闪**没有复现**；
- `--smoke` 路径可跑通并自动截图（2560x1440）。

★ 这一条还不能直接关单：探针只看**中心 1 像素**，且当前没有"跑到 TITLE / 已呈现 N 帧"的日志，
所以"没有白"也可能只是"没有帧"。**下一步（很小）**：给 renderer 加一条低频诊断（每次 present 计数，或在到达
TITLE 时打一行 `frames=<n>`），再用同一条命令确认"确实呈现过 N 帧且无白闪"，即可把本单挂 `verify` 收口。

## 收口（2026-10-28，自动判据）

给 renderer 加了**低频 present 诊断**（`[present] frames=N lastTrigger=… triggers={…}`，每 120 帧一行 + 第一帧立即打），
把"没有白闪"与"根本没有帧"分开。然后用真安装跑 150s：

```
npx electron --no-sandbox . --install <repo>/links/install --skip-logo
⇒ [present] frames=16320 … triggers={"frame":8165}；[flash-probe] white present 一条都没有
```

**守卫**：`tools/test/emulator-new-flash-probe.assets.test.mjs#启动期白闪：呈现过 ≥10 帧且没有一条 white present`
（assets 档：要真安装 + Electron；跑一次约 150s —— 它**故意跑满**整个启动窗口，因为白闪发生在启动期）。
两条判据缺一不可：**呈现过 ≥10 帧**（否则"没白"可能是"没帧"）+ **零条 `white present`**。
★ 它不看像素是否正确，只看"有没有整屏白"；像素级观感仍属人工/真机。
