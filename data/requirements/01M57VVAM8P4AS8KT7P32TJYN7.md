# 批 S1n · 补载体后暴露的成员表缺口：7 条里 6 条已修好加回，第 7 条（G1×G2 夹死）交给 S1q

- id: REQ-01M57VVAM8P4AS8KT7P32TJYN7
- type: req
- status: done
- parent: REQ-01M52F3ASMGWQXX64JSA18MP2N
- order: 55
- verify: tools/test/shape-claims-g4-init.test.mjs#★g4init-01
- tags: [engine shape]

## 背景

S1k 用 `propagate` 的机械推断批量补了 289 条 `arg` 载体（每条 witness 都落在 `call <fn>` 上）。
载体一补，`claims` 就把**这些函数自己也纳入判定** ⇒ 暴露 7 个函数的成员表缺口。
**6 条已修好并加回**；1 条卡在一个判据口径（G2 遇"整片 init 写"，见下）。

## 已修好并加回（每条都跑过 `claims` 全绿）

| 载体 | 函数 / 推断 | 修法（证据） |
|---|---|---|
| `s1k-49` | `sub_40BE10` / `T_Scene*` | `0x40be25 mov eax,[esi+0C610h]` → `0x40be2b/34/3d` 在 `+408/+40c/+410` 解引用 ⇒ `T_Scene+0xc610` = `kind:"ptr"`、`type:null`（不透明桶，**不是**红） |
| `s1k-145` | `sub_4B5090` / `T_SE*` | `[esi+edi*4+4BCh]`，`edi` 由 `cmp edi,0Ah` 界住 ⇒ `T_SE+0x4bc` = `T_el32 × 10`（G4：末尾 `0x4e4` **25 处引用**）；窗口内 9 个逐槽标量删除 |
| `s1k-171` | `sub_4BB840` / `T_Voice*` | 四处 `[esi+ebx*4+K]`（`K=418/460/46c/478`，`0x4bb938` 是之前漏看的第四张表）⇒ **四张并行表**各 `T_el32 × 3`（`+0x478` 末尾 `0x484 == size`）；窗口内逐槽标量删除 |
| `s1k-144` / `s1k-178` | `sub_4A1A60` / `sub_4B0360` / `T_Scene*` | `T_Scene+0xda04` = `T_el32 × 10`（`+0xda2c == size` ⇒ G4 第三档）；窗口内 9 个逐槽标量删除 |
| `s1k-206` | `sub_40A4C0` / `T_Command*` | `T_Command+0x77bec` = `T_el32 × 512`：构造函数 `0x415dca lea edi,[esi+77BECh]` + `mov ecx,200h` + `rep stosd` **直接观察**条数 ⇒ G4 新开"初始化写循环"档（见下） |

★ 共同病根：**"逐槽写死"盖住了数组**（G15 存在的理由）。修法一律是"给数组声明 + 删窗口内逐槽标量"。

## 顺带落成的一条判据档（G4 · 初始化写循环）

`T_Command+0x77bec` 的条数**有直接观察**（`rep stosd` 的 `ecx` 是立即数），但末尾 `0x783ec` 零引用 ——
旧 G4 只有"末尾被引用 / 判不了 / 末尾==size"三档，于是只能"只声明首槽"，索引访问随即变成假红。
⇒ `initStompCounts()`（`shape-claims.mjs`）扫 `lea edi,[reg+OFFh]` + `mov ecx,N` + `rep stosd`
（中间允许 `xor eax,eax` / `mov eax,…`；**被打断不算**），G4 认"N == 声明条数"。
守卫 `tools/test/shape-claims-g4-init.test.mjs`（★g4init-01..05）· 变异清单一条（拆掉这条证据 ⇒ 当场红）。

## 还差一条：`s1k-284` `sub_43AA80` / `T_Sys3Draw*`（witness `0x44a819`）

`0x43ab0d mov [esi+608h],eax` 起是一串 **dword** 清零；而 `+0x608/+0x609/+0x60a` 是**真的字节成员**
（`0x43b3b2/0x43b3f2` 有字节写，`0x43b7b6/0x43c264` 有 `movzx` 字节读）。
* 宽度改 4 ⇒ **G1 红**（与 `+0x609/+0x60a` 重叠）；不改 ⇒ **G2 红**（写宽 4 ≠ 声明 1）。
* 候选解法：给 G2 开一档"**整片 init 写**穿过字节字段"（理由同 G4 那条：写是对象初始化，不表达语义字段粒度），
  或证明那一带其实是 dword（要先解释那四处字节读写）。**未裁决 ⇒ 载体暂不加回。**
  ⇒ 已把它**移交 `S1q`**（"G2 第三档：整片 init 写 / 同一 dword 与其内字节字段"），本单不为它编声明。

## 判据（逐条）

每条只有两种结局：**改成员表**（数组 / `kind:"ptr"` / 宽度，按观察 + witness）或**撤回推断**。
⛔ 不许把宽度改成"碰巧等于访问宽度"、⛔ 不许把 `count` 写成 1 把索引访问藏起来。加回后 `claims` exit 0、`calls` 无 `mismatch`。

## 怎么查

```bash
pnpm tools shape claims data/shape/carriers.json   # 加回一条就跑一次；红的位置点名载体 id 与 EA
pnpm tools shape access sub_4BB840                 # 逐站点（带 stride / indexSym / problems）
```
