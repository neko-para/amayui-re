# 台账 bin 锚的站点口径与复核机制：锚 EA 与 claim 引用的地址错位（0x1000 级）无判据可发现

- id: REQ-01M4N3QC4SYG4C36QDBQCFRMSW
- type: bug
- status: done
- parent: REQ-01M3TD4N309J3Y6YMWWR2GTQAA
- verify: tools/test/ledger.anchor-lst.assets.test.mjs#★ 站点口径：在场 reference bin 锚必须落 `.lst` 行首真指令（A）或折叠/孤立站点（S）；落指令中间的必须在 allowlist 里
- repro: data/ledger/observation/2026-10.jsonl#tool/ledger/pe-offset-and-anchor-alignment
- severity: S2

# 台账 bin 锚的站点口径与复核机制：锚 EA 与 claim 引用的地址错位（0x1000 级）无判据可发现

- id: REQ-01M4N3QC4SYG4C36QDBQCFRMSW
- type: bug
- status: done
- parent: REQ-01M3TD4N309J3Y6YMWWR2GTQAA
- repro: data/ledger/observation/2026-10.jsonl#tool/ledger/pe-offset-and-anchor-alignment
- severity: S2
- verify: tools/test/ledger.anchor-lst.assets.test.mjs#★ 站点口径：在场 reference bin 锚必须落 `.lst` 行首真指令（A）或折叠/孤立站点（S）；落指令中间的必须在 allowlist 里

## 口径（2026-10-11 用户批准；上一版"站点还须被 claim 逐字引用"**作废**）
`bin` 锚**默认只许落站点起点** = `.lst` 里**行首那条真指令**的地址（"折叠 / 孤立 CHUNK 站点"——该 EA 处只有 `;` 注释行 —— 同算站点起点）。**落在指令中间**的例外必须（a）claim 说清"是哪条指令的第几个**字节**"、（b）带 `len`+`sha256`，并逐条登记进 `tools/lib/anchor-lst.mjs` 的 `AUDIT_ALLOWLIST`（**B 类不在表里 ⇒ 红**）。逐字引用是**人读的旁证**，不是判据 —— 它判不了"这个 EA 在语料里是什么"。

## 机械普查（本轮自算；在场 182 条记录 / 345 个 reference bin 锚）
**A=344**（行首真指令）· **S=1**（折叠站点）· **B=D=E=0**。判据 `cls = 不在语料?'D' : 是行首真指令?'A' : 有 listing 行但无真指令?'S' : 落前驱真指令的字节区间内?'B' : 'E'`，`len = nextRealInsn(ea) − ea`。★ 三个坑**照写进守卫**：**同一 EA 多行合并**（实测 `sub_48F000` 那个 EA 上叠着 proc near + 4 条栈变量定义 + 指令共 9 行）· **排假指令行**（`proc near` / `endp` / `名字= dword ptr 8` / 指示符 / 纯标签）· **折叠站点算真站点**（不排掉前两类，"锚落指令中间"整类会静默消失）。

## 25 个"待裁决"锚的裁决（**15 条订正 + 10 条保留 + 0 条 allowlist**）
机械做法：顺 `replaces` 链找**在做字节级取证的原始 claim** → 逐字点名字节 ⇒ 保留 EA + 登记 allowlist；否则 **吸附到所在指令的行首**；同记录多锚合并进**一条**订正版。31 个"曾经落指令中间"的锚，逐条读原始 claim，**没有一条**点名字节 ⇒ **allowlist 为空**（这是判据跑完的结果，不是"没做"）。

| 记录（在场 tip） | 现 EA | 旧 EA | 裁决 |
|---|---|---|---|
| `01M4NEX2AM1H3Q4P6P5D5V4J12` | `0x43485D` | `0x434860` | **订正** |
| `01M4NEXBT9105R475V0B7S3Z5X` | `0x4205FE` | `0x420600` | **订正** |
| `01M4NEXJV8687M1B4N6C2R1Q5G` | `0x417756` | `0x417759` | **订正** |
| `01M4NEYKXH362H4F5A550J1H4J` | `0x418A39` | `0x418A3A` | **订正** |
| `01M4NEZEX95R36543X1J4C5A0R` | `0x4910F0` | `0x4910F2` | **订正** |
| `01M4NEZEX95R36543X1J4C5A0R` | `0x49113D` | `0x491140` | **订正** |
| `01M4NEZEX95R36543X1J4C5A0R` | `0x491161` | `0x491165` | **订正** |
| `01M4NEZEX95R36543X1J4C5A0R` | `0x491167` | `0x491168` | **订正** |
| `01M4NEZEX95R36543X1J4C5A0R` | `0x418A3D` | `0x418A3E` | **订正** |
| `01M4NEZEX95R36543X1J4C5A0R` | `0x43505D` | `0x435060` | **订正** |
| `01M4NEZEX95R36543X1J4C5A0R` | `0x43506F` | `0x435070` | **订正** |
| `01M4NEZEX95R36543X1J4C5A0R` | `0x42CFDE` | `0x42CFE0` | **订正** |
| `01M4NEZQ5Q1J262S7044034S2P` | `0x407EFF` | `0x407F00` | **订正** |
| `01M4NEZQ5Q1J262S7044034S2P` | `0x407EF9` | `0x407EFA` | **订正** |
| `01M4NEZZRE732Q5N0D4874693E` | `0x4154BF` | `0x4154C0` | **订正** |
| `01M4NEXT8Z7Y2E3G2P2B160B0M` | `0x48F032` | （同） | **保留** |
| `01M4NEXT8Z7Y2E3G2P2B160B0M` | `0x48F03D` | （同） | **保留** |
| `01M4NEZ2XH7P0H3V5R436P5F5N` | `0x45D284` | （同） | **保留** |
| `01M4NEZ2XH7P0H3V5R436P5F5N` | `0x45D2C1` | （同） | **保留** |
| `01M4NEZEX95R36543X1J4C5A0R` | `0x49115B` | （同） | **保留** |
| `01M4NEZEX95R36543X1J4C5A0R` | `0x434F60` | （同） | **保留** |
| `01M4NEZEX95R36543X1J4C5A0R` | `0x42CFFD` | （同） | **保留** |
| `01M4NEZQ5Q1J262S7044034S2P` | `0x407FBD` | （同） | **保留** |
| `01M4NEZQ5Q1J262S7044034S2P` | `0x407FC0` | （同） | **保留** |
| `01M4NF775K281W2T4M5M5N0X6C` | `0x40EF56` | （同） | **保留** |

★ 10 条"保留"的 EA **本来就是** `.lst` 行首真指令（可再校验的站点），旧口径判它"待裁决"只因它没被 claim 逐字引用 —— 新口径**不要求**这个 ⇒ 不订正，只**具名裁决**。逐条理由（含"与 claim 的哪句话相关"）见 `tools/lib/anchor-lst.mjs` 的 `AUDIT_SITE_EXCEPTIONS`（守卫逐条复核：仍在此处 · 仍是站点 · 订正过的那 15 条旧 EA **仍**判成"落该 EA 内"）。

## C 类（0x1000 级）：判据收紧 + 2 条具名例外
第一版"差在一页到两页之间"在真数据上**误报 118 条**（1.1 MB 的 `.text` + 一条 claim 常点名十几个地址）。收紧成"**恰好**差一页（`|ea − x| === 0x1000`）**且**锚 EA 不在 `x` 所在函数体内" ⇒ 实测 **2** 条，均已裁决：`01M4N3PG90…`（**正文就是在讲那两个 EA 差一页**）· `01M4NEZEX9…`（与点名的 `sub_4900F0` 恰好差一页的是**另一个函数** `sub_491060`；同记录的锚 `0x418A30` 被逐字引用 ⇒ 无整体平移，平移读法也不自洽）。

## 摘要棘轮
`REFERENCE_BIN_DIGEST_DEBT_MAX`：**338 → 282 → 276**。本轮只追加一条**带齐摘要**的观察、不动任何欠账记录 ⇒ 现算 **276 = 上限、余量 0**（棘轮不瞎）。逐条回填见 `REQ-01M4NF3QG2V6RKM3ZFDDNTZVJK`。

## 判据（已挂）
* **口径本身**：在场 345 个锚必须全落站点（A/S）或进 allowlist —— 守卫第 1 条（下一轮有人把锚改歪就红）；
* **25 条裁决可复核**（EA / 旧 EA / 裁决 / 理由）—— 守卫第 4 条；
* **0x1000 级**：实测候选集合必须**等于**例外表 —— 守卫第 5 条；
* **缺件不许假绿**：`.lst` 缺 ⇒ `t.skip` 写明"缺了它连行首都判不了"；参考件缺 ⇒ 只跑"分类 + allowlist EA 成员"的**降级分支**（用例名里明说降级）。

## 不在本单范围
PE 映射本身（`peOffsetOf`）· "锚与 claim 的**语义**是否一致"（准入/复核的事）· `repo` 枚举缺"游戏安装根"档（另开单 `REQ-01M4NG7X2RXAJB4HEFR0EHMZ5V`）· 存量摘要逐条回填（`REQ-01M4NF3QG2V6RKM3ZFDDNTZVJK`）。
