# ⬜ 那三个"按 cur 索引"的 dword 数组（+0x78 区）用途未查明

- id: REQ-01M48EVKSNX139B8TX0CYXZ0A1
- type: req
- status: open
- parent: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- order: 12
- tags: [engine, ledger]

# ⬜ 那三个"按 cur 索引"的 dword 数组（0x5D8F8/0x5D8FC/0x5D900）用途未查明

- id: REQ-01M48F0000000000000000001
- type: req
- status: open
- parent: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- order: 12
- tags: [engine, ledger]

## 已知（机械可复核）
* 初始化点 `sub_405640`（语料 `.text:004056BD..004056CB`）把三者都写成 0：
  `mov [ecx+edx*4+5D8F8h], esi` / `…+5D8FCh` / `…+5D900h`，`edx = 2*(15*cur)`，`esi = 0`。
* 另一处 `sub_40EB74` 附近：`lea ebx,[esi+5D8F8h]` + `mov edi,3`（**无 cur 参与**）⇒ "基址 + 3 项"的访问。
* 一处 `memset` 调用前 `push 0FFh`（Val）且源指针取自 `[esi+eax*4+5D8F8h]`（语料 `.text:0040F5EC`）。
* 语料里 `5D8FCh` 与 `5D900h` 各只出现 **1 次**（就是那三行初始化）⇒ 其余用法要么在别处用基址+偏移算，
  要么这三项只在初始化时被碰。

## 待做
1. 三条数组各自的**读点**在哪（用基址 + 索引的形态找，不要只搜绝对地址）。
2. 它们与帧的生命周期关系（是否随 loadScriptFrame 重建、是否被存档覆盖）。
3. 索引变量 `edx = 2*(15*cur)` 的量纲：为什么是 `30*cur` 而不是 `cur`？是"每帧 2 项"还是算错的口径？

## 判据
结论按准入门登记进 `data/ledger/`（锚到语料 EA / 守卫），并把本单 `verify` 指到对应守卫。
