# emulator-lift · TITLE 残余：sprite 合批 + 按需回读

- id: REQ-01M6RQ2XNWRDV35E72D7G2ZV7D
- type: req
- status: open
- parent: REQ-01M6RH9CDPM8PG2SFEG0PYT46T
- tags: [emulator-lift, gpu, title, performance, sprite]

## 现状（2026-11-06 直出后实测；★ 2026-11-13 复核：下表已过时）
★ 2026-11-13 复核：**sprite 合批已落地**（2026-11-08 `4f03ff0`，`d3dx9.cpp` 的 `rasterQueue` +
`gpu-batch/order` 守卫），下表 `End 18.4 ms` 与"残余根因 1"不再是现状；**此后没有任何 TITLE 墙钟读数**
⇒ 本单第一步是**重新量**（见验收 1），不是按旧表继续优化。
TITLE direct GPU present 已成立（`present=direct/immediate`，Present median 7.5 ms），但 TITLE wall 仍 median **58.5 ms**（约 17 fps），未达 33 ms。
| 项 | 直出后 TITLE median |
|---|---:|
| wall | 58.5 ms |
| Present | 7.5 ms（readback 1.7 / hash 4.8 / sink 1.0 / stats 0） |
| ID3DXSprite::End | 18.4 ms |
| DrawPrimitive | 7.9 ms |
| guest | ~11–12 ms |
## 残余根因
1. ~~**Sprite 合批没做**~~ → 2026-11-08 已落地（`4f03ff0`）；读数待重测。
2. **按需回读没做**：`d3d9::Present` 仍每帧 GPU→guest 回读 1.7 ms + guest hash 4.8 ms（hash 是 `lastHash` / dump / A4 语义的一部分）；约 6.5 ms。★ 注意它不只是性能：`present.cpp` 每帧 hash 读 guest、喂交互图 `screen`，且"`!dirty` ⇒ guest 最新"今天是被当成公理的 ⇒ 去掉无条件回读前必须先补 stale 标记，否则静默上传上一帧。
3. 剩余是 guest ~11–12 ms + 多 shim/调度开销。
## 验收（2026-11-13 按"GPU + 实时、不做像素一致"改）
1. `--raster gpu --real-time --present window`，TITLE wall ≤33 ms（≥30 fps），`--gpu-strict` 仍 exit 0、fallback=0（**先测再判**：若已达标，直接写读数收口）。
2. ~~像素与逐条路径 ≤1 LSB~~（作废：不做像素一致）；合批保留的要求只剩"无整层缺失/黑块"（人眼 + 截图 artifact）。
3. 回读：`d3d9::Present` 不再无条件回读；`--dump-frame` / `--present-screenshot` / guest 真读后缓冲时按需同步，行为与现在一致。
4. ~~CPU 参考路径逐字节不变~~（作废：CPU 光栅整体退休）；`lastHash`/frameCount/`--gpu-strict` 语义不变，LOGO 不退化。
5. 守卫：提交次数（结构性）的机械判据 + 按需回读的 stale 标记用例；墙钟读数进 `observations.md`，⛔ 不写进 test。
## 非目标
- 不改变 GPU 光栅准入/像素语义；不为快而跳过 guest 语义。
- 不在本单扩多 stage / 16bpp / alpha test / depth 镜像。
