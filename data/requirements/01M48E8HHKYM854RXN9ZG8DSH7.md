# 帧步长 0x78 与帧装载器在"帧+0x78/+0x7C/+0x80"建三个数组互相冲突

- id: REQ-01M48E8HHKYM854RXN9ZG8DSH7
- type: bug
- status: done
- parent: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- order: 11
- verify: tools/test/engine-frame.test.mjs#★ 那三个"帧尾数组"是**按 cur 索引、步长 4** 的独立数组
- repro: tools/test/engine-frame.test.mjs#★ 帧的矛盾仍在（旧口径说法 vs 新语料机械普查）
- severity: S2
- tags: [engine, ledger]

# 🐞S2 帧步长 0x78 与帧装载器在"帧+0x78/+0x7C/+0x80"建三个数组互相冲突

- id: REQ-01M48E8HHKYM854RXN9ZG8DSH7
- type: bug
- status: done
- parent: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- order: 11
- tags: [engine, ledger]
- severity: S2
- repro: tools/test/engine-frame.test.mjs#★ 帧的矛盾仍在（旧口径说法 vs 新语料机械普查）
- verify: tools/test/engine-frame.test.mjs#★ 那三个"帧尾数组"是**按 cur 索引、步长 4** 的独立数组
- done_reason: 分歧的理由是**读数口径错**（不是帧布局错），已按机械普查裁决并登记更正记录

## 结论（迭代点 ③ 裁决）
**帧步长 0x78 成立；旧读数把"按 cur 索引的独立数组"错当成了帧内字段。**

* 旧口径：把 `0x5D8F8 / 0x5D8FC / 0x5D900` 直接减帧基址 383104 ⇒ 读成"帧+0x78 / +0x7C / +0x80"。
* 新语料实测（出处 `sub_405640`，`.text:004056BD..004056CB`）：
  `mov [ecx+edx*4+5D8F8h], esi` / `…+5D8FCh` / `…+5D900h`，其中 `edx = 2*(15*cur)`
  ⇒ 这三个是**步长 4、按帧号 cur 索引**的 dword 数组，`ecx` 是对象基址（**不是**帧指针）。
* 另有 `lea ebx,[esi+5D8F8h]` + `mov edi,3`（`sub_40EB74` 附近）⇒ 存在"基址 + 3 项"的另一种访问。
* 帧内字段一律以 `15·cur` 为索引（`[reg+reg*8+5D8xxh]`）；**全语料 530296 行普查**：帧相对偏移
  **≥ 0x78 的槽只有 1 个**（就是那次 `lea`）⇒ 与"帧 = 0x78 字节"完全相容。

## 残留（不在本单范围）
那三个 cur 索引数组的**用途**未查明，另开单跟踪。
