# ALF / AGF 核验台账（内部）

> 不是分发的规范正文；它是 `chapters/20-alf.md` / `chapters/30-agf.md` 的核验记录。
> 正文只写"部分 / 待核验"的状态，EA / 台账 id / 守卫在这里。

## ALF

| id | 声明 | 状态 | 关联 | 未完成 |
|---|---|---|---|---|
| `FMT-ALF-S4-HEADER` | S4 索引头 300 字节；签名 `S3IC` / `S4IC` | **verified** | 加载器 `sub_414AC0`：`.text:00414B2A` 读 `0x12C` 字节；`.text:00414B49` 比较 `S3IN`、`.text:00414B5D` 比较 `S4IN`、`.text:00414B71` 比较 `S3IC`、`.text:00414B85` 比较 `S4IC` | — |
| `FMT-ALF-ADDON` | 加装档 `AC` 段起点 268 / 532 | **partial** | 实现；引擎 `S3IN`/`S4IN` 分支未逐条对 | 加装档分支的段起点计算 |
| `FMT-ALF-S5` | S5 / UTF-16LE 头 540 | **unverified** | 实现；语料无样本 | 找 S5 索引 / 引擎分支 |
| `FMT-ALF-S3IN-S4IN` | 引擎在 `S3IC` / `S4IC` 前还比较 `S3IN` / `S4IN` | **unverified** | 加载器 `.text:00414B49` / `.text:00414B5D` | 这两个签名的头布局与用途 |
| `FMT-ALF-TOC` | TOC = archive_count + 归档 + file_count + 文件记录 | **partial** | 实现 + `SYS4INI.BIN` TOC 偏移记录（`KN-01M4H0NJNQ1G7Q575C082K0605`） | 逐字段对引擎读取点 |
| `FMT-ALF-LZSS` | 4 KiB 窗口 / 最短匹配 / 段头 LZSS | **unverified** | 实现（Okumura 变体） | 定位引擎解压函数并逐参对账 |
| `FMT-ALF-FILE-INDEX` | 文件记录的 `file_index` | **unknown** | 实现注释也不解释 | 引擎消费点 |

## AGF

| id | 声明 | 状态 | 关联 | 未完成 |
|---|---|---|---|---|
| `FMT-AGF-READ-PATH` | AGF 读取/解码路径 | **partial** | 引擎图像读取段 `sub_43E9F0` 起；错误串 `AGFヘッダの読み込み時...`（`.text:0043FFE4` 一带） | 逐字段读取点 |
| `FMT-AGF-HEADER` | `ACGF` 24 字节头 / meta / body 段 | **unverified** | 实现 | 头字段对读取指令 |
| `FMT-AGF-LZSS` | 窗口 4096 / 初始 4078 / 最短 3 | **unverified** | 实现 | 引擎解压函数 |
| `FMT-AGF-ACIF` | 可选 `ACIF` + alpha | **unverified** | 实现；语料有样本 | 引擎 alpha 读取/使用点 |
| `FMT-AGF-PIXELS` | 宽高 / bpp / 调色板 / 行序 / 通道 | **unverified** | 实现 + 解码守卫 | 引擎像素读取 / blit 路径 |

## 怎么用

- 正文的「核验状态」只引用状态，不复制 EA；
- 升级一条：补"关联"列（EA / 台账 / 守卫），更新状态，再改正文；
- 未核验的 ALF/AGF 字段不得被当成已证实协议事实。
