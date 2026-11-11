# 音频解码吞吐：ADV 每句 ~1.7 s（本机）/ ~0.75 s（参考机）的三个杠杆（Huffman/码本 · 起播重建解码器 · __ftol2_sse）

- id: REQ-01M75PTEGCR1Y91FKKJ1CWPNH2
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift audio performance decode]

## 目标

降低 ADV 推进下一句的墙钟，并抬高音频 feeder 的占空余量
（一刀 fill 产出 0.5 s 音频、实测 381–462 ms ⇒ **占空 76–92%**；两条 Ogg 流同时跑就 >100% ⇒ 落后 ⇒
听感上的重复/错位）。

## 基线（本机，`graphs/sn0000.graph` click1/click2，`--native-overrides all --audio off`）

| 段 | click1 / click2 |
|---|---|
| `sub_420F70 -> sub_4BB840`（整条 VOICE） | 1728.4 / 1807.9 ms |
| ├ `-> sub_4B6570`（起播重建解码器 + DS 缓冲） | 485.6 / 587.2 |
| └ `-> sub_4B6020`（起播预填） | 1241.3 / 1219.4 |
| 　└ `sub_4B6C30 -> sub_48D610`（**3 刀 fill 的聚合**，146/145 包） | 1237.0 / 1215.1 |
| 　　├ `sub_4D90C0 -> sub_4E2A30` | 823.1 / 805.9 |
| 　　│　├ `-> sub_4E4370 -> sub_4E3EE0`（Huffman/码本） | **600.7 / 587.3** |
| 　　│　└ `-> sub_4E0B40`（MDCT，已 override） | 2.2 / 2.4 |
| 　　└ `-> __ftol2_sse`（66 176 次） | 176.4 / 172.5 |

⇒ 单刀 fill ≈ 412 ms（1237/3）。**比例口径**：Huffman 48.6%、位读 28%、ftol 14%。
（参考机同形、整体慢 ~2.3×，读数见 `REQ-01M75CWE5A5MV5847K2WHSFB17`。）

## 结论（取证后，三个候选各自的去向）

| 杠杆 | 去向 | 证据 |
|---|---|---|
| ② 起播重建解码器（485–587 ms） | **不修：这是引擎语义（b 类）** | 非幂等：`0x4B661B` 取插槽 → `0x4B6626/28` 取 `[ecx]`/`[[ecx]]` → `0x4B662A push 1; 0x4B662C call eax`（对**上一任**发 scalar-deleting dtor）；`0x4B667F` 无条件覆盖插槽。重开：`0x4B6774 call eax`（槽 `[vtable+4]`；vtable `0x52917C` `??_7OggVorbis@@6B@`，`[1]=sub_48CF70` `[2]=sub_48D610`），而 `sub_48CF70` 在 `0x48CFB9` 用 `SetFilePointer(0,0,FILE_CURRENT)` 把**当时**的文件位置记成数据起点 ⇒ 复用 = 改引擎语义。**且它不在 fill 预算内** ⇒ 对占空零贡献 |
| ① Huffman/码本（600 ms） | **收窄成取证**：入口要避开 `sub_4E3EE0` | `sub_4E3EE0` 在 codec arena 里分配（`0x4E3F4B call sub_4D7A60`，每包 `0x4D90E5 call sub_4D7AD0` reset），arena 偏移会进 guest 指针 ⇒ override 必须复刻"同 size 同序"。而**位读占这 600 ms 的 57%（345 ms）**，只换码表查找约 160 ms |
| ③ `__ftol2_sse`（176 ms） | **另单**：`REQ-01M75PW4MXS06MN6FKGXB36ETH` | 锚 `0x4E82C0`（**不是** `0x4E82F6`＝`__ftol2`）；本线 shim 恒 0 ⇒ 执行 x87 那条体 |

## 下一步（只有一条）

取证 `sub_513220` / `sub_5132D0`（`0x4E1586` 的 look、`0x4E15C2` 的 adv）：是不是 leaf、
状态是否只落在 guest 数据里。若是 ⇒ 它是最优入口（不用复刻 arena 分配、调用面窄、可 bit-exact 判）。
**收益口径**：只换位读 ⇒ 占空 **76–92% → 55–66%**；"39–48%"需要连 arena 分配一起换（本次不做）。

## 不做

- 通知位置 4→8：4 个 offset 是 **guest 传进来的**，feeder 的 `WaitForMultipleObjects` 与
  `sub_4B6C30` 的 `(a2%4+3)%4` 都按 4 建索引 ⇒ 改 8 是改契约，不是改性能。
- `sub_4D81B0`（42 ms，跨包窗口）/ `sub_48D610` 的 Ogg 分页（~196 ms，含 `ReadFile` 与 corrupt 恢复）/
  `sub_4E2A30`、`sub_4D90C0`（各 62 ms self）。
- 预热 / 早启动 feeder：占空 76–92%，没有空闲 CPU。
- ② 的 instrumented 归因：只服务一个已结案的杠杆。

## 判据

- 取证阶段（本单的交付物）：`sub_513220`/`sub_5132D0` 的 EA + 调用边 + 状态落点，
  "能不能做 leaf override"一句话可判。
- 若真做实施：两臂差分（`hits>0 && fallbacks==0`，oracle = 解释器臂）+ 同窗口
  `sub_420F70 -> sub_4BB840` 与一刀 fill 的墙钟同时下降；比例读数进文档，不进 test。

## 未做

- 参考机 219/251 vs 本机 486/600 的 2.3× 是否全为解释器速度（未拆）。
- 22 个"直接进地址/尺寸"的 `__ftol2_sse` 站点最终消费者未追到底（记在 ③ 那张单里）。
