# 帧步长 0x78 与帧装载器在"帧+0x78/+0x7C/+0x80"建三个数组互相冲突

- id: REQ-01M48E8HHKYM854RXN9ZG8DSH7
- type: bug
- status: done
- parent: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- order: 11
- verify: tools/test/engine-frame.test.mjs#★ 三个 per-cur dword 格
- repro: tools/test/engine-frame.test.mjs#★★ 负例：旧口径
- severity: S2
- tags: [engine, ledger]

## 结论（迭代点 ③ 裁决；2026-10 三次订正后重写）

★★ **本单第 2 条（"记录下界 ≥ `0x8C`"）已被取代** —— 落地在 `REQ-01M4H0Q2TQMKHQ35X69DG0FPGP`
（台账 `KN-01M4H0MZGD4K4P4F0J1E7E5G79`）：**记录基址 = `Engine+0x5D894`、记录大小 = 索引步长 = `0x78`**。
原来那个 `0x8C` 是**把记录基址取成 `0x5D880`**（= `cur` 那个 Engine 级标量的槽）算出来的**假偏移**：
`0x5D904/0x5D908` 相对真基址是 `+0x70/+0x74`，**都落在 `0x78` 之内** —— 所以"两个量"这件事**不成立**。

* **字段索引步长 = `0x78`**（成立）：记录内字段与 `0x5D8F8/0x5D8FC/0x5D900` 三格都用 `reg = 15·cur` 寻址
  （`*8` ⇒ 净步长 `120·cur`）；三格写作 `[reg+edx*4+5D8xxh]`，而 `edx = 2*(15*cur) = 30*cur` 是**索引的 dword 数**
  ⇒ 净字节步长**同为 0x78**（出处 `sub_405640`，`.text:004056BD..004056CB`）。⇒ 那三格是**记录内 `+0x64/+0x68/+0x6C`**。
* ~~**记录下界 ≥ `0x8C`**~~（**已否**）：`0x40F63B mov [esi+edx*8+5D904h],ecx` 与 `0x40F678 mov [esi+edx*8+5D908h],ecx`
  各写一次（`ecx` = `operator new(0x10)` 出来的容器对象）—— 这两条**字节事实不变**，变的是它们相对**谁**：
  真基址 `0x5D894` 之下是 `+0x70/+0x74` ⇒ **记录大小 = `0x74 + 4 = 0x78`**，与下一条记录的 `+0x00` 严丝合缝。
* ⛔ **不采信的读法**（本轮踩过并已撤回）：① ~~"三格是帧记录尾槽"~~（**现在成立**：`0x5D8F8 = 0x5D894+0x64`）；
  ② "`imul eax,84h` ⇒ 帧记录 = 0x84"（那一段落点是 `Engine+0x69334 + 0x84*cur`，**另一个 per-cur 状态块** —— 仍然不采信）。
* 另有 `lea ebx,[esi+5D8F8h]` + `mov edi,3`（`sub_40EA00`，**无 cur 参与**）⇒ "基址 + 3 项"的访问（释放路径）。

## 残留（不在本单范围）
三格的**用途**（装的是什么表、由谁读值）⇒ `REQ-01M48EVKSNX139B8TX0CYXZ0A1` 跟踪。
★ 三格的**归属**（记录内三格）与**记录的真实大小**（`0x78`）已由 `REQ-01M4H0Q2TQMKHQ35X69DG0FPGP` 收口。
