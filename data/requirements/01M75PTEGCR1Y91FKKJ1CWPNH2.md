# 音频解码吞吐：ADV 每句 ~1.7 s（本机）/ ~0.75 s（参考机）的三个杠杆（Huffman/码本 · 起播重建解码器 · __ftol2_sse）

- id: REQ-01M75PTEGCR1Y91FKKJ1CWPNH2
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift audio performance decode]

# 音频解码吞吐：ADV 每句 ~1.7 s（本机）/ ~0.75 s（参考机）的两个杠杆

## 目标

降低 ADV 推进下一句的墙钟，同时抬高音频 feeder 的占空比余量
（0.5 s 预算 / 一刀 fill 380–460 ms = **76–92%**；两条 Ogg 流就超 100% ⇒ 落后 ⇒ 听感上的重复/错位）。

## 基线 A：参考机（macOS arm64，`REQ-01M75CWE5A5MV5847K2WHSFB17` 的 `click1` 3 s 窗口）

| 段 | 读数 |
|---|---|
| `sub_420F70 -> sub_4BB840`（整条 VOICE） | 746.2 ms |
| ├ `-> sub_4B6570`（重建解码器 + 建 DS 缓冲） | 219.7 ms |
| └ `-> sub_4B6020`（起播预填 3 刀） | 515.6 ms |
| 　└ `sub_4B6C30 -> sub_48D610` | 676.3 ms / 146 包 |
| 　　└ `sub_4D90C0 -> sub_4E2A30` | 499.5 ms |
| 　　　├ `-> sub_4E4370`（"Huffman/码本"边） | 251.7 ms |
| 　　　└ `-> sub_4E0B40`（MDCT，已 override） | 163.3 ms |

## 基线 B：本机（Windows x64，`graphs/sn0000.graph`，`--native-overrides all --audio off --present window --raster gpu`）

两次点击窗口同形（不是噪声）：

| 段 | click1 | click2 |
|---|---|---|
| `sub_420F70 -> sub_4BB840` | **1728.4 ms** | **1807.9 ms** |
| ├ `-> sub_4B6570` | **485.6 ms** | **587.2 ms** |
| └ `-> sub_4B6020` | 1241.3 ms | 1219.4 ms |
| 　└ `sub_4B73E0 -> sub_4B6C30` ×3 | 1241.2 | 1219.4 |
| 　　└ `sub_4B6C30 -> sub_48D610` | 1237.0 / 146 | 1215.1 / 145 |
| 　　　├ `sub_4D90C0 -> sub_4E2A30` | 823.1 | 805.9 |
| 　　　│　├ `-> sub_4E4370 -> sub_4E3EE0` | **600.7** | **587.3** |
| 　　　│　├ `-> sub_4E6480` / `-> sub_4E61E0` | 84.6 / 72.9 | 83.8 / 69.1 |
| 　　　│　└ `-> sub_4E0B40` | **2.2** | **2.4** |
| 　　　└ `-> __ftol2_sse`（66 176 次） | **176.4** | **172.5** |
| 　　　└ `-> sub_4D81B0` | 42.0 | 40.8 |

⇒ 形状与参考机一致、整体慢 ~2.3×。**两个杠杆没变**：重建解码器 **28%** + Huffman 树 **~48%**。

## 三个杠杆（按"归因是否已清"排）

1. **Huffman / 码本**（本机 ~600 ms/句）：入口**不是** `sub_4E4370`（只有 57 行、`__cdecl`、直调
   `sub_4E3EE0`）；inclusive 全落在 **`sub_4E3EE0`**（599/585 ms）。同窗口同链上还有
   `sub_4E1570`（self 93 ms / incl 438 ms / 38 482 次）与 `sub_4E1950`（incl 533 ms / 3 222 次），
   大概率是 `sub_4E3EE0` 的下钻 ⇒ 下一步先用**调用边**把它们钉进同一棵子树再谈 override 入口。
2. **每次起播重建解码器**（本机 ~486–587 ms/句）：不是单函数，是 `sub_4B6570`；先归因（码本初始化 /
   堆分配 / `CreateSoundBuffer` 各占多少），再判"能不能缓存 / 能不能 native"。
   ⛔ 不要预设能缓存：每条语音是不同文件。
3. **`__ftol2_sse`**（本机 ~176 ms/句、66 k 次）：CRT 的 float→int 辅助，在 `sub_48D610` 底下。
   语义面比 Huffman 小得多，可能是一刀就能拿掉的（先核取整语义与舍入模式、以及 x87 控制字）。

## 判据

- 归因阶段：同一窗口（`graphs/sn0000.graph` 的 `click1`/`click2`）里目标函数的调用边 + inclusive 时间，
  以及"无 override vs 有 override"的两臂差分（`hits>0 && fallbacks==0`，oracle = 解释器臂）。
- 收口读数：同一窗口的 `sub_420F70 -> sub_4BB840` 与"一刀 fill"的墙钟同时下降。

## 未做

- 参考机的 219/251 与本机 486/600 之间差的 2.3× 是否全是解释器速度（未拆）。
- 第 2、3 个杠杆都还只是**候选**（未取证到可实现的入口）。
