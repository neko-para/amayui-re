# Huffman 子树函数级 override：0x4E1570 + 0x4E1950（每刀 200→~30 ms，拿走 86%）

- id: REQ-01M75WED0VFTEYXX43KGHNPPP0
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/native_override_assets.cpp#huff/bsearch-left
- tags: [emulator-lift audio performance native-override]

## 目标

把 Huffman 子树的两片叶子做成**函数级** native override：`0x4E1570`（look/首表/二分/adv）与 `0x4E1950`（码本向量）。
预期每刀 200 ms → **28–31 ms**（本机口径），即拿掉这棵子树的 **86%**。**不做**整块换 `sub_4E3EE0`。

## 现状拆分（每刀，本机 profile；`1.957 = (guest+interp)/guest` 摊回）

| 组成 | EA | ms | % |
|---|---|---|---|
| 位读 look / adv / 位反转 | `0x513220` / `0x5132D0` / `0x4E1510` | 43.0 / 17.1 / 16.8 | 38.5% |
| 首表 / 二分 / 降位重试（本体） | `0x4E1570` | 60.8 | 30.4% |
| ⇒ **`0x4E1570` 整棵** | | **137.7** | **68.9%** |
| 向量重建（`fld/fadd/fstp`，dim 路） | `0x4E1950` | 33.9 | 17.0% |
| 窗口/声道循环 + 调用 + arena 残差 | `sub_4E3EE0`… | 28.1 | 14.1% |

★ **arena 不是性能问题**（`0x4D7A60`+`0x4D7AD0` ≈ 0.3 ms/刀），只是**正确性约束**；
只 override 叶子可以**结构性绕开**它（分配次数/顺序/尺寸全不变）。
★ 口径：`412 ms/刀` 是本机 profile，`200–250 ms/刀` 是用户机 trace 的 `total` —— **两台机器差 1.7×，比例可搬、绝对值不可搬**。

## 已裁决（⛔ 实现者不要重新裁决；发现硬反证就停下报告）

1. **只做 S2**：`0x4E1570` + `0x4E1950` 两片，函数级。⛔ 不做整块换（风险高一个量级、且非必要）。
2. **粒度选 `0x4E1570` 整棵**，⛔ 不要只换 `513220`/`5132D0`（纯位提取只有 38.5%，会被整棵吸收）。
3. `kMax` **16 → 18**（`static_assert` 会先红，是好事）。
4. **`entry == 0` 必须 decline**（否则 guest 会读 `codelengths[-1]`）。
5. 读数要求：真线程模式**只能**用 `LIFT_AUDIO_TRACE=1` 的 `[audio] feeder fill … guest=… ov=N`
   （`ov` 不涨 = 没生效；feeder 有独立 override 副本、计数器分开）；pump 模式才用窗口 profiler。

## 最大风险（写进实现注释）

`0x4E40A9 xor eax,eax` 让**解码失败与成功都返回 0** ⇒ **差 1 bit 不会报错**，只会让这一段残差静默失真
（听得出、引擎零信号）。所以守卫必须：
- **真字节两臂**（解释器 vs native，逐字节比输出 PCM）；
- **位读三个 dword** 逐值比（look 之后的状态）；
- 一条变异：把 adv 的字节数改 1 ⇒ 必须红。

实现时这三处细节必须照 `.lst` 复刻，别按"标准 Huffman"想当然：
- `0x4E1666 adv(codelengths[lo])` 与 `0x4E167C adv(read)` **两条分支的 adv 字节数不同**；
- `0x4E15A5` 是 **`sar` 不是 `shr`**（符号性）；
- 位读状态在 **guest 内存**（不是宿主），override 必须读写同一份。

## 判据

- 差分臂（`tests/native_override_assets.cpp`）：两臂逐字节 + 位读三 dword + 边界（`entry==0`、最短/最长码、单符号码本、`dim>8` 走跳表）。
- 隔离臂：`all,-<名字>` 各自回到解释器。
- 变异：adv 字节数 ±1 / `sar`→`shr` / `entry==0` 不 decline ⇒ 分别必须红。
- 性能读数（只进文档）：pump 模式 `sub_4E2A30->sub_4E4370 incl` 下降；thread 模式 `guest=` 下降且 `ov=` 增长。

## 依赖 / 排序

- 实现要碰 `native_overrides.h`(kMax) / `native_overrides.cpp`(kKnown) / `tests/native_override*.cpp` / `tools/mutate-check.mjs`
  ⇒ **与 memcpy 重叠单串行**（它也在改这几个文件）。
- 与令牌尖峰（`REQ-01M75VZXV4SDMVVM1Z72G009XJ`）正交；与 PCM 转换段（`REQ-01M75W6BB3PY3MF42ZTS23KTBS`）互补：
  Huffman 拿走本块 86%，PCM 转换再拿 32.6 ms/刀，两者合起来才把 fill 压到 ~100 ms。

## 修订 2026-11-12（父级裁决，覆盖上面"判据"里的变异清单）

- **撤销 `sar`→`shr` 变异**：`0x4E15A5 sar esi,0Fh` 之后紧跟 `0x4E15AE and esi,7FFFh`，
  掩码只保留原始 bit15..29；算术右移与逻辑右移的差别只在**被符号扩展填的 bit15..31**，而它们被掩码清掉
  ⇒ **任何输入下同值**。这是**恒真项**，⛔ 不要再加（代数证明见实现者报告）。
- **替代变异**：二分里的 `cmp eax, dec_index[mid]` 是**无符号**比较（`sbb`），改成**有符号**比较 ⇒ 必须红。
- 保留：`adv` 字节数 ±1（`0x4E1666` 与 `0x4E167C` 两条分支不同）、`entry==0` 不 decline。
- **门禁污染的处置**：实现期间工作树被 rep 快路径的在制品污染
  （`exec_string.cpp` / `x86.h` / `native_crt.cpp` / `guest_copy.h` / `rep_bulk.h`），
  `memcpy-overlap/*` 的 6 条红属于**那边的 WIP**，不是本单失败；合并后由父级建干净树复验。
