# S2a-2：pixi 执行画布 36/37 + emit/copy/composite

- id: REQ-01M5PQQ68ZJTYP0S3AVNYS9YT7
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-scene-command-run.test.mjs#★ 关闸（kinds 空）⇒ 零 op、零宿主调用；未启用的种类也不产 op
- tags: [emulator s2]

# S2a-2：pixi 执行画布 36/37 + emit/copy/composite
## 目标
把 `scene-command-exec.ts` 的 op 序列接进 `pixi-host.present()`：独立画布表（不进 `slots` / `reconcileTextures`）、按 op 切目标 / 清 / emit / copy / composite；`enabled=false` 产品零回退。
## 步骤
1. `PixiRenderHost` 加 `commandCanvases: Map<'canvasA'|'canvasB', RenderTexture>`（缺省按快照宽高建，尺寸变化重建）。
2. `buildScene` 的节点创建抽成 `emitEntries(entries, into)`（按 table 选 drawable + planDraw）；只发指定条目。
3. `present()` 内、收尾扫描前跑 `scene-command-exec`（仅 `enabled=true`）；copy = renderer.render 到 own；composite = 两画布作为 sprite 依次画到 own（先 A 后 B，进度 S2b 再接）。
4. present 次序：命令档 op → 收尾扫描 → backbuffer；`trigger=frame`。
5. 守卫：开闸才跑 / 关闸零 op / 画布尺寸重建 / copy 方向 / composite 源次序；不测像素。
## 风险
`buildScene` 一帧多次调用、快照目标、影片 RT 争用。
