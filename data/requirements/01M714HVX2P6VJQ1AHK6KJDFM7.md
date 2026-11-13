# 工具缺口 · 折叠区间没有取字节的路（只有外部 objdump）+ 折叠让 1519 条库函数不在 item 目录 ⇒ 调用边静默消失

- id: REQ-01M714HVX2P6VJQ1AHK6KJDFM7
- type: req
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- verify: tools/test/disasm.test.mjs#折叠块可辨认
- tags: [tooling, disasm, corpus]

## 判据（存在性 + 观察）

两条**工具层**的缺口，都是 `REQ-01M713MSM6WKDDTMHCXR6FJX5Q`（补字节）实测撞出来的：

| # | 缺口 | 观察（可复现） |
|---|---|---|
| 1 | **没有任何工具能取"折叠区间"的字节** | `pnpm tools disasm at --ea 0x4E8A6E --lines 40` 只把折叠行打回来；`span` 要 `--match`、与字节无关；`disasm verify` 的口径是**文本↔字节的 CP932 转写保真**（不是指令级）；`packages/age-format` 的反汇编器是 **AGE 脚本字节码**；`disasm-fn show 0x4E8A6E` 报"不落在任何函数里" ⇒ 想读那 6 段只能靠外部 `llvm-objdump` + 一次性脚本 |
| 2 | **折叠让调用边静默消失** | 折叠行替掉了 `proc near … endp`，而 `disasm-fn` 的函数身份正是从它来的（`tools/lib/catalog.mjs`）⇒ 这 **1519 条不在 item 目录里**。铁证：`.lst` 里 `0x5130B4`（`j_RtlUnwind`）**只出现 1 次**，二进制里却有 **3 处** `call 0x5130b4`（`0x4E7066` / `0x506071` / `0x5068E3`，全在被折的函数里）⇒ `callers` / `callees` / `impact` 在库函数上是"间接调用 + 折叠"的**双重下界，且不报错** |

- 判据 #1：给一条**不依赖外部工具**的取字节路径（EA → PE 段表 → 文件偏移 → 字节），并对至少一处折叠区间给出"字节 == 二进制"的自校验。
  最小可复用件 = 「PE 段表 → EA→文件偏移 → 取字节」纯 JS 段（零依赖），落点建议 `tools/lib/pe.mjs` + `disasm-at --bytes`；解码器选型另裁。
- 判据 #2：让"依赖了目录里不存在的函数"这件事**响亮**，而不是静默给下界。候选：`callers/callees` 输出里显式标 `folded`（或给"已知折叠覆盖基线"的计数器），
  ⛔ 不是把折叠区强行解出函数（那要改语料或另一套规则）。

## 为什么现在不做（严重度）

- 两条都**不改变已有结论**（`REQ-01M713MSM6WKDDTMHCXR6FJX5Q` 已经用外部工具把该拿的字节拿到了），只是"下一次还会撞"。
- #2 属于"静默少报"这一类 —— 按本仓一贯口径是要响亮的；但它影响的是**分析工具的可信度**，不是引擎语义，取 S3。

## 修法边界

- ⛔ 不许去"展开语料"（语料是只读真源，改动它等于换真源）。
- ⛔ 不许把外部工具（`llvm-objdump`）设成门禁依赖：本仓是"纯文本依赖"口径，`tools/*` 必须能只靠 Node 跑。
- #2 的"响亮"要么进输出行、要么进 `--stats` 的汇总；⛔ 不要只写进 README（README 不写状态）。
- 守卫：`tools/test/disasm.test.mjs` 已有"截断必须明说"一类判据，照它的形态加一条"折叠覆盖必须可见"。

## 进度（2026-11-10）：**#1 已修**（取字节的路通了），#2 仍未做

- **#1 已修**：`disasm-at at --bytes <n>`（域是 **`disasm-at`**，不是 `disasm` —— `disasm` 是"语料保真"域；本单早期正文把域写成 `disasm` 是错的）
  现在**纯 JS** 取折叠区间字节。★ 按仓内既有不变量（`tools/test/ledger.test.mjs` 的"工具侧不许第二处 PE 节表解析"）**没有新模块**：
  能力加进 `tools/lib/ledger.mjs`（`PeError` / `readPeSections` / `eaToFileOffset` / `readBytesAt`，块首注释写明"为什么归这里"与"两套契约"：
  锚点解析仍返 `null`、取字节必须**抛**）；既有 `peMap` / `peOffsetOf` **一字未动**。
- CLI 侧先核对二进制 sha256 == `.lst` 头部的 `Input SHA256`（不符即停，⛔ 不硬读）；`rva = ea − ImageBase`，段界 `max(VirtualSize, SizeOfRawData)`；
  失败全抛 `PeError(code)`：`below-image-base`（点名"你可能传了 RVA"）/`beyond-image`/`unmapped`（列出已映射段）/`no-raw-bytes`/`range-exceeds-raw`/`file-truncated`/`bad-length`（`n` 必须正整数）。⛔ 无 `null`、无空 Buffer。
- 实测：`disasm-at at --ea 0x4E8A6E --bytes 76` ⇒ 正好整段（首 5 字节 `8B FF 55 8B EC` = MSVC 序言），并标注 `.lst` 折叠行声明的 76 字节；
  不带 `--bytes` 时输出与改动前**逐字相同**（两版 SHA256 一致）。
- 守卫：`tools/test/pe.test.mjs`（**手写**合成 PE32，7 例：命中 / 未映射 / 越界 / 段内超 `SizeOfRawData` / 跨节 / 文件截断 / 头部拒绝）
  + `tools/test/pe-corpus.assets.test.mjs`（真实语料抽样 3 例：`.lst` 头段表 vs PE 节表**两源算同一偏移**、折叠长度对账、CLI 端到端；`corpus/` 缺席则 `t.skip`）。
  修前红：把节边界判断只留上界 ⇒ "未映射"那条 `Missing expected exception`。
- **#2 仍未做**（折叠让 1519 条库函数不在 `disasm-fn` 的 item 目录 ⇒ `callers/callees/impact` 在库函数上是**双重下界且不报错**）：
  候选 = 输出里显式标 `folded`，或在 `--stats` 给"已知折叠覆盖"计数器。⛔ 不是把折叠区强行解出函数（那要改语料或另一套规则）。
- 顺带发现（已另开单）：`tools/lib/anchor-lst.mjs` 的 `COLLAPSED_RE` 漏了 `COLLAPSED CHUNK OF FUNCTION` 形态。

## 与别的单的关系

- 发现于 `REQ-01M713MSM6WKDDTMHCXR6FJX5Q`；直接影响 `REQ-01M708DF24KY0QZNEHY66S0HYW` 之后的任何"读库函数"工作。
- 折叠的**成因**（导出视图状态、1805/1805 全是库名、跨模块复现）已写在补字节单的正文里，本单不复述。
