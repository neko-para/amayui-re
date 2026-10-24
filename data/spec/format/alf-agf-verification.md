# ALF / AGF 核验台账（内部）

> 不是分发的规范正文；它是 `chapters/20-alf.md` / `chapters/30-agf.md` 的核验记录。
> 正文只写状态词（口径见下）；EA / 台账 id / 守卫在这里。

## 状态口径（闭集）

本台账与正文共用同一组状态词；正文不写别的状态词。判据与 `format/verification.md` 的**协议核验状态**同一条：**verified = 读取点逐条对上 + 关联列给可再校验的锚**（EA / accepted 台账 / 守卫用例）。

| 本台账 | 正文 | 判据 |
|---|---|---|
| `verified` | **已核验** | 该条声明的每个读取点都在引擎里逐条对上；`关联` 列给出可再校验的锚（EA / accepted 台账 / 守卫用例） |
| `partial` | **部分** | 只有一部分对上；`未完成` 列写明缺哪一部分 |
| `unverified` | **未取证** | 只有实现观察，尚未与引擎读取点关联；不得当作协议事实 |
| `unknown` | **未查明** | 读取点已看，但语义（谁消费、代表什么）不确定 |
| `no-sample` | **无样本** | 语料里没有该变体的样本，无法核验 |
| `out-of-scope` | **不属于 ALF** / **不属于 AGF** | 归属别的协议或加载器；本台账不覆盖 |

★ **与 `audit.json` 的 `status` 是两套词**：`verified` 是协议核验状态，`draft | reviewed` 是规则的证据档位；⛔ 不由"已核验"推出 `reviewed`，也不由 `reviewed` 推出"已核验"。

表里不写复合状态：不属于本协议的条目直接写 `out-of-scope`。

## ALF

| id | 声明 | 状态 | 关联 | 未完成 |
|---|---|---|---|---|
| `FMT-ALF-S4-HEADER` | S4 索引头 **268** 字节；签名 `S3AI` / `S4AI` / `S3AC` / `S4AC` | **verified** | 加载器 `sub_401100`：`.text:00401084` `memset(obj+0Ch,0,10Ch)`；`.text:0040116A` `ReadFile(...,10Ch)`；`.text:00401180` / `00401188` / `00401190` / `00401198` 比较四个签名；`.text:004011A4` 置 AC 标志 | — |
| `FMT-ALF-S?IN-S?IC` | `S3IN` / `S4IN` / `S3IC` / `S4IC` 是**另一个加载器**（初始化文件，头 300 字节）；本版不覆盖，归属 CORE-FS / CORE-BOOT | **out-of-scope** | `sub_414AC0`：`.text:00414B2A` 读 `0x12C`；`.text:00414B49`–`00414B85` 比较四个签名 | — |
| `FMT-ALF-ADDON` | 加装档 `AC`：`[268,280)` 是 **12** 字节压缩块，TOC 压缩（压缩数据从 **280** 起） | **verified** | `.text:00401201` `push 0Ch`（仅 AC）；`.text:00401266` / `.text:0040126C` 解压；非 AC 分支 `.text:00401280` 起裸读 `FileSize-268` | — |
| `FMT-ALF-S5` | S5 / UTF-16LE 头 540（只有实现） | **no-sample** | 实现；语料无样本 | 找 S5 索引 / 引擎分支 |
| `FMT-ALF-TOC` | TOC = archive_count + 归档记录（256，整条为名字）+ file_count + 文件记录（80 = 名字 64 + archive_index +0x40 + file_index +0x44 + offset +0x48 + length +0x4C） | **verified** | `.text:004012E2` / `.text:004012F9`（`count<<8`）、`.text:00401326`–`0040132E`（`count*80`）、`.text:0040146C`（`+40h`）、`.text:004014AB`（`+48h`）、`.text:004014C1`（`+4Ch`）、`.text:0040144D`（记录起点当名字） | — |
| `FMT-ALF-LZSS` | 4 KiB 窗口 / 初始写位置 4078 / 最短 3 / 最长 18 / flag 低位先 / 位置 12 位（环内下标）/ 长度 = 低 4 位 + 3 | **verified** | 唯一解压例程 `sub_436A80`：`.text:00436ACE`（`0FEEh`）、`.text:00436B1A` / `00436B5C` / `00436B70`（`0FFFh`）、`.text:00436B3C`（`and eax,0Fh`）/ `00436B42`（`+2`）、`.text:00436B76` / `00436B79`（循环含等号 ⇒ +3）、`.text:00436AE0` / `00436B02`（LSB-first） | — |
| `FMT-ALF-FILE-INDEX` | 文件记录的 `file_index`（记录 +0x44） | **unknown** | 布局里有该字段（`.text:0040146C` 一带的四个 u32 之一） | 引擎是否有消费点 |

## AGF

| id | 声明 | 状态 | 关联 | 未完成 |
|---|---|---|---|---|
| `FMT-AGF-READ-PATH` | AGF 读取 / 解码路径（入口 + 头 / 段 / ACIF / 像素） | **verified** | 引擎图像读取段 `sub_43E9F0` 起；头 / 段 / ACIF / 像素的读取点见下面四行 | — |
| `FMT-AGF-HEADER` | 头 24 字节（头 4 字节不参与判定，常见 `ACGF`；version 1/2、unknown0=0）/ meta / body 段 | **verified** | `.text:0043EB11`（BM 分流）、`0043ED43` / `0043ED58`（version / unknown0）、`0043ED68`–`0043ED8C`（12 字节 meta 块）、`0043EE77`–`0043EED9`（12 字节 body 块）、`0043FEF7`（拒绝） | — |
| `FMT-AGF-V1` | version 1 不读 ACIF；头 4 字节不参与判定 | **partial** | 真实样本 `SO006.AGF`（头 4 字节为 0、无 ACIF）；引擎 version 读取分支 `0x43ED43`（1/2 都接受） | 该样本是 8bpp；v1 的 24 / 32bpp 像素路径没有单独样本 |
| `FMT-AGF-LZSS` | 与 ALF **同一例程、同一参数**（`sub_436A80`） | **verified** | meta：`.text:0043EE46`；body：`.text:0043EF5C`（缓冲分配 `.text:0043EEE2`）；alpha：`.text:004017B1` | — |
| `FMT-AGF-ACIF` | version 2 的 body 之后**紧接** `ACIF`（缺则读取失败）；24 字节头 + 12 字节长度块 + alpha；只校验宽高、不校验 alpha 字节数 | **verified** | `.text:0043EFDA`（仅 v2）、`.text:00401620`（读 24 字节）、`00401655`（magic）、`00401668` / `00401675`（宽高）、`004016F6`（12 字节块）、`0040179A` / `0040179E`（按 +12 分配）、`004017AC`（解压）；失败路径 `0043F048` → `0043F0A2` → `00440488` | — |
| `FMT-AGF-PIXELS` | 宽高 @ meta+20/+24 / bpp（16 位，本版只取 8/24/32）@ meta+30 / 调色板槽 meta+56（1024 字节，8bpp 256 项）/ 行按 32 位（4 字节）对齐 / bottom-up / 盘上 BGR(A) 与**输出接口** top-down RGBA8 | **partial** | `.text:004396CE`（bpp）、`0043945F` / `00439433`（调色板）、`00439632` / `004396F6`（bottom-up）、`0043A12B` / `0043A14A` / `0043A14E`（24bpp 通道）、`0043A2C6` / `0043A2CD` / `0043A2E6` / `0043A2EA`（32bpp 通道；A 取自像素第 4 字节）、`0048BEE7` / `0048BF85`（第二消费者同判据 + 32bpp 步进 4）、`0043D427` / `0043D488`（第三消费者：只有 8/4/1/default，default = 3 字节/像素 ⇒ 无 32bpp 支）、`0047992D`（第四条路径：body 包成内存 BMP 后交平台库，路径内不建 alpha 对象）、`0043928D`（目标位段）、`0048B090`（上传器按 32 位对齐行宽） | 24bpp 的非 4 倍数宽度无样本，行尾填充是按 8bpp 1026 宽（body = 1028×height）外推；**32bpp 没有样本**（本作可枚举的 AGF：16 个 8bpp、2 个 24bpp，均 version 2 且带 ACIF），32bpp 的 alpha 取舍已在引擎侧穷举（32bpp 分支不读 alpha 段），缺的只是端到端样本 |

## 怎么用

- 正文的「核验状态」只引用状态，不复制 EA；
- 升级一条：补"关联"列（EA / 台账 / 守卫），更新状态，再改正文；
- 未核验的 ALF/AGF 字段不得被当成已证实协议事实。
