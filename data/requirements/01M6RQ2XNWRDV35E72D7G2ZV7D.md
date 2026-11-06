# emulator-lift · TITLE 残余：sprite 合批 + 按需回读

- id: REQ-01M6RQ2XNWRDV35E72D7G2ZV7D
- type: req
- status: open
- parent: REQ-01M6RH9CDPM8PG2SFEG0PYT46T
- tags: [emulator-lift, gpu, title, performance, sprite]

## 现状（2026-11-06 直出后实测）
TITLE direct GPU present 已成立（`present=direct/immediate`，Present median 7.5 ms），但 TITLE wall 仍 median **58.5 ms**（约 17 fps），未达 33 ms。
| 项 | 直出后 TITLE median |
|---|---:|
| wall | 58.5 ms |
| Present | 7.5 ms（readback 1.7 / hash 4.8 / sink 1.0 / stats 0） |
| ID3DXSprite::End | 18.4 ms |
| DrawPrimitive | 7.9 ms |
| guest | ~11–12 ms |
## 残余根因
1. **Sprite 合批没做**：每个 sprite 条目一次 `drawBatch`（独立 transfer buffer + command buffer + submit），TITLE 每帧约多次提交，`End` 18.4 ms。
2. **按需回读没做**：`d3d9::Present` 仍每帧 GPU→guest 回读 1.7 ms + guest hash 4.8 ms（hash 是 `lastHash` / dump / A4 语义的一部分）；约 6.5 ms。
3. 剩余是 guest ~11–12 ms + 多 shim/调度开销；目标 33 ms 需要把 sprite 合批、并只在 dump/screenshot/真 guest 读时同步。
## 验收
1. `--raster gpu --real-time --present window`，TITLE wall ≤33 ms（≥30 fps），`--gpu-strict` 仍 exit 0、fallback=0。
2. Sprite 合批：同一帧/同一状态的条目合并提交，提交次数可见下降；像素与逐条路径 ≤1 LSB，且无整层缺失/黑块。
3. 回读：`d3d9::Present` 不再无条件回读；`--dump-frame` / `--present-screenshot` / guest 真读后缓冲时按需同步，行为与现在一致。
4. `lastHash`/frameCount/`--gpu-strict` 语义不变；CPU 参考路径逐字节不变；LOGO 不退化。
5. 守卫：TITLE 帧墙钟/提交次数/像素的机械判据，不能只靠人看。
## 非目标
- 不改变 GPU 光栅准入/像素语义；不为快而跳过 guest 语义。
- 不在本单扩多 stage / 16bpp / alpha test / depth 镜像。
