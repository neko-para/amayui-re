# D3D9 剩余缺口索引（7 条待拆单；含已否证清单）

- id: REQ-01M7485DCS1RBQSDDH6F68HS1J
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- done_reason: 路由完成：7 行全部拆单或作废（#2/#3/#5/#6 已各自 done 并带守卫；#1 并入 RTT 单已 done；#4 拆到 REQ-01M7A2F1CCEPWNF12Z7NSN7GQ7；#7 作废）。本表已空，规则（只列未拆行）满足。
- tags: [emulator-lift d3d9 index]

## 一句话
本单只做**路由**：剩余未拆单的 D3D9 缺口逐条列在这里。**每行必须先单独拆单**（本仓"一张单只收一类失效"）才动代码；
拆单后把该行搬进新单、并从本表删除，保持本表**只列"还没拆的"**。

## 表
**本表已空**：所有行都已拆单或作废（见下）。

## 已拆单（本表不再列，⛔ 不要塞回来）

- #1 sprite 合成目标恒为后缓冲 ⇒ 随 CPU sprite 路径退休一并处理；GPU sprite 画进 RT 归 **`REQ-01M79MSMWGFEKF8J7C7VYWS4YP`**（RTT，显式写"本单不做 sprite→RT"）。
- #2 caps 声明与实现脱钩 ⇒ **`REQ-01M79PP97ZD65BJG9GMAJ08MJ3`**（进行中）。
- #3 `ValidateDevice` 恒 `S_OK,1` ⇒ **`REQ-01M79PP9H5XZRWFJD4BGS19S54`**（本轮拒绝实现 + 防漂移守卫）。
- #5 `MinIndex` / 负 `BaseVertexIndex` ⇒ **`REQ-01M79PP9S4BFBPKDPBQBDN5GGR`**（✅ done，`d3d9p/indexed-range`）。
- #6 `SetRenderState` 的 `default:` 静默接受 ⇒ **`REQ-01M79PPA10JCKG4C9HY6QMV6PT`**（✅ done，`d3d9p/render-state-policy`；bit26/27/28 已具名）。
- #7 `fragments` 实为顶点数 ⇒ **作废**（"应有像素"只能来自 CPU oracle，而 CPU 光栅已退休）。

## 已被否证、别再查（负结果索引）
纹理副本陈旧 · guest 覆盖镜像擦批 · 深度清值 · 视口二次映射 · scissor · 没进 render pass · mip 回归 · `LIFT_NO_MIP` 对 GPU 无效 ·
FF 管线本身 · 顶点链 · 三张 2048² 图集的内容（level 0 哈希 == guest）· mip 选级公式（实测返回 2）· `ID3DXSprite::Begin` 无 ALPHABLEND（1013/1013 带 `0x10`）。
★ 详细证据在各单正文与 `.tmp/d3d9/a*.md`。

## 未做
- 不在这里写实现细节。
- 被裁"边界外"的东西仍写进对应单的 `## 未做`，不塞回本表。

## 收口（2026-11-13）

本单只做路由。7 行全部处置完：#1 并入 RTT（`REQ-01M79MSMWGFEKF8J7C7VYWS4YP`，已 done）、#2 已 done（`d3d9/caps-claims`）、
#3 已 done（`d3d9p/validate-device-one-pass`，防漂移）、#4 拆到 `REQ-01M7A2F1CCEPWNF12Z7NSN7GQ7`（接口保真，低优先）、
#5 已 done（`d3d9p/indexed-range`）、#6 已 done（`d3d9p/render-state-policy`）、#7 作废（CPU oracle 退休）。
