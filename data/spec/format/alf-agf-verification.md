# ALF / AGF 核验台账（内部）

> 不是分发的规范正文；它是 `chapters/20-alf.md` / `chapters/30-agf.md` 的核验记录。
> 正文只写"部分 / 待核验"的状态，EA / 台账 id / 守卫在这里。

## ALF

| id | 声明 | 状态 | 关联 | 未完成 |
|---|---|---|---|---|
| `FMT-ALF-S4-HEADER` | S4 索引头 **268** 字节；签名 `S3AI` / `S4AI` / `S3AC` / `S4AC` | **verified** | 加载器 `sub_401100`：`.text:00401084` `memset(obj+0Ch,0,10Ch)`；`.text:0040116A` `ReadFile(...,10Ch)`；`.text:00401180` / `00401188` / `00401190` / `00401198` 比较四个签名；`.text:004011A4` 置 AC 标志 | — |
| `FMT-ALF-S?IN-S?IC` | `S3IN` / `S4IN` / `S3IC` / `S4IC` 是**另一个加载器**（初始化文件，头 300 字节） | **verified（不属于 ALF）** | `sub_414AC0`：`.text:00414B2A` 读 `0x12C`；`.text:00414B49`–`00414B85` 比较四个签名 | — |
| `FMT-ALF-ADDON` | 加装档 `AC`：头（268）之后多 **12** 字节压缩块，TOC 压缩；段起点仍是 268 | **verified** | `.text:00401201` `push 0Ch`（仅 AC）；`.text:00401266` / `.text:0040126C` 解压；非 AC 分支 `.text:00401280` 起裸读 `FileSize-268` | — |
| `FMT-ALF-S5` | S5 / UTF-16LE 头 540 | **unverified** | 实现；语料无样本 | 找 S5 索引 / 引擎分支 |
| `FMT-ALF-TOC` | TOC = archive_count + 归档 + file_count + 文件记录（归档 256 / 文件 80 / 记录内 +64 名字段） | **partial** | `.text:004012E2` / `.text:004012F9`（`count<<8`）、`.text:00401326`–`0040132E`（`count*80`）、`.text:0040146C`（`+40h`）、`.text:00401460` / `.text:004014AB`（`+48h`）、`.text:004014C1`（`+4Ch`） | 头内其余字段 |
| `FMT-ALF-LZSS` | 4 KiB 窗口 / 初始写位置 4078 / 最短 3 / 最长 18 / flag 低位先 / 位置在前 | **verified** | 唯一解压例程 `sub_436A80`：`.text:00436ACE`（`0FEEh`）、`.text:00436B1A` / `00436B5C` / `00436B70`（`0FFFh`）、`.text:00436B3C`（`and eax,0Fh`）/ `00436B42`（`+2`）、`.text:00436AE0` / `00436B02`（LSB-first） | — |
| `FMT-ALF-FILE-INDEX` | 文件记录的 `file_index` | **unknown** | 实现注释也不解释 | 引擎消费点 |

## AGF

| id | 声明 | 状态 | 关联 | 未完成 |
|---|---|---|---|---|
| `FMT-AGF-READ-PATH` | AGF 读取/解码路径 | **partial** | 引擎图像读取段 `sub_43E9F0` 起；错误串 `AGFヘッダの読み込み時...`（`.text:0043FFE4` 一带） | 逐字段读取点 |
| `FMT-AGF-HEADER` | `ACGF` 24 字节头 / meta / body 段 | **unverified** | 实现 | 头字段对读取指令 |
| `FMT-AGF-LZSS` | 与 ALF **同一例程、同一参数**（`sub_436A80`） | **verified** | meta：`.text:0043EE46`；body：`.text:0043EF5C`（缓冲分配 `.text:0043EEE2`） | — |
| `FMT-AGF-ACIF` | 可选 `ACIF` + alpha；**只校验宽高，不校验 alpha 字节数** | **partial** | 头解析 `sub_401610`：`.text:00401620` 读 24 字节、`.text:00401655` magic、`.text:0040167A` / `00401682` 版本、`.text:00401687` `+8==0`、`.text:00401668` / `00401675` 宽高；使用端 `sub_43DF20`：`.text:0043E1A0`–`0043E1A9` 按宽高逐行拷贝 | alpha 压缩路径 |
| `FMT-AGF-PIXELS` | 宽高 / bpp / 调色板 / 行序 / 通道 | **unverified** | 实现 + 解码守卫 | 引擎像素读取 / blit 路径 |

## 怎么用

- 正文的「核验状态」只引用状态，不复制 EA；
- 升级一条：补"关联"列（EA / 台账 / 守卫），更新状态，再改正文；
- 未核验的 ALF/AGF 字段不得被当成已证实协议事实。
