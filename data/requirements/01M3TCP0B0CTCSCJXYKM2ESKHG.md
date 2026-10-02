# 批 M2 · 格式层（AGE 容器：ALF / AGF / ASM / uimap）

- id: REQ-01M3TCP0B0CTCSCJXYKM2ESKHG
- type: req
- status: done
- parent: REQ-01M3TC9660ZG1YMB7KAKE4W94B
- order: 40
- verify: packages/age-format/test/alf.test.mjs#索引文件读进来再写出去，逐字节相同

## 范围
`packages/age-format`：**ALF**（归档）/ **AGF**（图像）/ **ASM**（脚本）三套容器的解析与组装。
~~uimap~~ **不在范围内**（用户口径：**uimap 只是一个工具**，不是数据格式 —— 那是"扫 PNG 里的块"，
属工具层，不进格式层）。旧仓 `scripts/{asm,alf,agf}` 只作**参考**（按新结构重写，不是逐文件搬运）。

## 判据（已完成）
1. **解包 → 重打包逐字节相同**（唯一口径）：三套格式各有守卫，且都能在**真实件**上成立。
2. **改内容之后不是只会原样抄回去**：ALF 换条目载荷、AGF 改像素各有一条用例；
   负例也都有（ALF 归档留空洞、AGF 8bpp 索引越界、ASM 头签名不对）⇒ 守卫**红得有意义**。
3. **样本取自 corpus（不是伪样本）**：样本是**原始游戏文件**，按用户口径**不入库** ——
   只在清单登记来源路径 + sha256（`assets/samples-{alf,agf,asm}`），守卫 #4 对它们现算现比；
   样本缺席时用例**跳过**（fresh clone 不该红），而不是假装通过。

## 非目标 / 有意剔除
* 旧仓 `scripts/uimap/`（`scan_blocks.py` / `clean_fill.py`，Python）—— **只是个工具**，不进格式层；
* PNG 编解码（AGF 只承诺"容器 + 像素排布"，解出的是 `w*h*4` 的 top-down RGBA）；
* ★ **指令表里的引擎结论**：旧表 `scripts/asm/opcodes.json` 含 `handler`（opcode → 引擎函数，574 条 /
  543 个不同函数）与 `status`（`已核对`/`仅映射` 的人工自述标签）—— 两者都是**结论**、属知识层，
  K3 通过前不得进新仓。格式层只保留四列，由 `pnpm tools opcodes derive` **机械派生**。
* ★ 但**观测必须可追溯**：那份旧表已登记为 `knowledge/opcode-table-source`（`external-only` + sha256 +
  `blocks: [K1,K2,K3]`），派生器**按登记解析来源、不硬编码旧仓路径** ⇒ 旧仓移除也不会断链
  （缺来源时工具与守卫都**报错**，不是跳过）。

## 凭据
* `packages/age-format/test/{alf,agf,asm}.test.mjs`：**22 条**全绿（ALF 7 / AGF 9 / ASM 6）。
* `node packages/age-format/cli.mjs verify`：**7 项核对 / 0 失败 / 0 跳过**（判据的一条命令版）。
* `pnpm tools opcodes report|derive` + `tools/test/opcodes.test.mjs`（**8 条**）：派生只吐四列、
  丢弃可点名、非法输入不留残file、**来源登记在清单里**（删掉登记 ⇒ 工具报错且 2 条守卫红）。
* `pnpm test`：**109/109**（全仓；`package.json` 的 glob 已扩到 `packages/*/test/**`）。
* 实测数字（本机 + 旧仓只读）：
  * ALF：索引 `APPEND02.AAI` 5 839 B、数据体 `APPEND02.ALF` 51 814 238 B ⇒ 两者**逐字节相同**；
  * AGF：`MI042.AGF` 173 706 B（meta 1080→159、body 775680→140160、alpha 258560→33315）、
    `MI040.AGF` 86 419 B（meta **原样**、body 258560→51772）、`SO002.AGF` 328 832 B（8bpp、256 色）⇒ 3/3 相同；
  * ASM：4 个样本（284 / 440 / 21 472 / 52 492 B）⇒ 反汇编→重汇编逐字节相同；
    验证面另扩到**全量**：游戏目录 106 个 `*.BIN` ⇒ equal 104 / 不等 0 / 报错 2
    （两个都不是 AGE 脚本：`AGE.exe__userdata.bin`、`SYS4INI.BIN`，旧仓 CLI 同样拒绝）；
  * 与旧仓基线 `data/SC0000.txt`：**31130 行行数一致**，仅 10 行不同，全是同一 opcode `0x20d` 的
    助记符演进（本表 `set-render-target` / 基线 `i20d`）⇒ 差异来自表自身演进，不是移植误差；
  * 压缩方向互逆：ALF 目录区 LZSS 在 3 个真实索引上重压逐字节相同（含 44.6 万字节的 `SYS4INI.BIN`）；
    AGF 族 LZSS 在 44 个真实 AGF / 132 段上"压得小就压、否则原样"与盘上**0 处不符**。

## 已知偏差（有意，不粉饰）
1. **v5 头字段**：旧仓把 13 个字段整块后移 8 字节（真实语料全是 v4，故一直没暴露）；
   本包按字段顺序修正，只用**合成 v5 脚本**验过往返相同 —— 没有真实 v5 样本可验。
2. **CP932 编码方向**照抄旧仓的 `encodeSkipVals=[0xED40,0xF940]` 优先级（不照抄则 `STINIT2.BIN` 差 1 字节）；
   **但**外字区 `0xF040–0xF9FC` 保留可编码（iconv 会写坏成 `?`）⇒ 可编码集合是 iconv 的超集。
3. **不编造**：AGF 取不到调色板、8bpp 像素索引越界、ASM 字符串读越界，一律**显式报错**
   （旧仓分别是造灰度表 / 静默越界 / 静默截断）。
