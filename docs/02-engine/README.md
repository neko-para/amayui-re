# 02 · 引擎域（**只写语料与工件事实，不写任何游戏语义结论**）

> ★ 本文件只记录**语料与工件层面**的事实：路径、哈希、编码、节表修补做了什么、验收口径、消费规则。
> **字段含义 / 函数用途 / opcode 语义 / 脚本角色一律不在这里** —— 那些属于待重建的知识层
> （见 `../00-origin/knowledge-rebuild.md`），在 K3 通过前不入库。

## 1. 二进制与语料的事实 `[§1.8]`

引擎二进制约 **1.3 MB**；游戏业务数据 > 2 GB；IDA 反编译 C 约 **19.6 万行 / 5 MB**。

| 文件 | 路径 | MD5 | 说明 |
|---|---|---|---|
| 原版脱壳件（原件） | `gameInstall:AGE.EXE__dumped.EXE` | `F8A4BC5962CF2BC598DE2FF83B1ABE9E` | PE 节表**未还原**，直接分析会丢数据段 |
| **节表修补副本（= 基线二进制）** | `oldRepo:raw-parts/AGE.EXE__dumped.sectfix.EXE` | `50ECBA9B35F0886031B2752B5D8F73CE` | 与原文件**仅节表 24 字节不同** |
| 汉化改写版导出（对照用） | `oldRepo:engine/天结_unpacked.exe{,_utf8}.{c,lst}` | — | ⚠ **非忠实**（见下） |

**AGERC 模块（三份二进制、两个世代）**：

| 文件 | 路径 | 说明 |
|---|---|---|
| 带壳发布版 | `gameInstall:AGERC.DLL`（旧仓 `raw/` 是指向同一目录的符号链接） | 打包发布件 |
| **脱壳调试版** | `oldRepo:raw-parts/DATA1/AGERC.DLL` | 不带壳、有调试入口 ⇒ **当前 AGERC 语料的真前身** |
| 改过文案的版本 | `oldRepo:install/AGERC.DLL` | ⚠ 旧仓那两对 AGERC 语料是针对**它**反汇编的 ⇒ 已作废（见 §4） |

### 1.1 节表修补做了什么（24 字节）

节 0 补 `IMAGE_SCN_CNT_CODE(0x20)` 并命名 `.text`；节 1/2/5 去掉 `MEM_EXECUTE` 并命名 `.data` / 空 / `.edata`；
节 6 `.idata` 去 `CNT_CODE`；节 7 `poly` 保持 `CODE`。
**OptionalHeader / DataDirectory / 其余字节一律不动。**

### 1.2 修补后的验收口径（实测）

* `.lst` 段声明：`.text` / `poly` = `'CODE'`，`.data` / `seg002` / `seg003` / `.idata` = `'DATA'`；
* `assume ds:` = `_data`；`#error` **0 条**；
* `char aXXX[] =` 声明 **782** 个；distinct 字符串字面量 **1357** 个；
* `0x51D738` 显示为 `asc_51D738 db '…',0`（修补前是 `dword_51D738 dd …`）。

### 1.3 原版 vs 汉化改写版的实质差异（只有两类）

1. **唯一确证的指令级补丁 `sub_41A6C0` = ASCII→全角映射表**：
   原版 CP932（前导 `0x82`，偏移 `+0x1F`/`+0x20`，符号位 `817B/817C/8194`）；
   汉化版 CP936/GBK（前导 `0xA3`，偏移 `+0x80`，`'#'→A3A3` / `'-'→A3AD` / `'+'→A3AB`）。
2. **导入调用"拉平"**：原版 `call ds:` 1470 处（163 个具名 Win32 API），汉化版仅 13 处 + 56 处 `jmp ds:` 跳板。
   残差法实测 **3189/3758（84.9%）函数残差为 0** ⇒ 体长差异由此解释，语义未变。

## 2. 入库的反汇编语料（4 个文件，一个 zip）`[§1.8.1]`

```text
corpus/disasm/
  disasm-20260930.zip    # ★ 入库（LFS）—— 内含 4 个 **UTF-8** 文件
  README.md              # 清单 + 来源 + sha256 + 解压/转码/断言口径
  files/                 # ← gitignore：解压产物，agent 直接读这里
```

| # | 文件 | 来历 | 角色 |
|---|---|---|---|
| 1–2 | `AGE.EXE__dumped.sectfix.EXE.{c,lst}` | `binary/age-sectfix` 的 Hex-Rays 导出（出仓 → `.staging/`） | **基线** |
| 3–4 | `AGERC.DLL.{c,lst}` | `binary/agerc-debug-unpacked`（`raw-parts/DATA1/AGERC.DLL`）的 Hex-Rays 导出（出仓 → `.staging/`） | 配套模块 AGERC.DLL |

* **为什么不带旧仓那对 `天结_unpacked.exe`**：即使把旧的迁进来，**其余文档仍要到旧仓原位置查**，
  并不能自包含 ⇒ 半迁移只增加"重复且会不一致"的风险，换不来可用性。
* **为什么合成一个 zip**：这 4 个文件是只读、基本不再改的归档工件；合成一个 blob 既不产生逐文件 diff 噪声，
  文本压缩率又高。**入库的是 zip；解压产物 ignore**（本地解一次即可）。
* **转写规则沿用旧仓已定案的口径**（`docs-new/03-engine/agerc-module.md` §4 + `.tmp/convert_*.py`）：
  纯 ASCII 行原样；含 `; File Name   :` 的行整行按 GBK；其余按 CP932 的字节结构逐字节解
  （`0xA1..0xDF` 是单字节半角假名、绝不是前导字节；`A1 F4..FE` 是 IDA 的 GBK 箭头；解不出的单字节保留原码位）。
  **绝不做符号改写**：既不 `::` → `__`、也不 `this` → `_this`、更不折叠空格或重命名符号。
* 本文 4 个文件来自**原版**二进制，**不含** UTF-16LE 真宽串（旧仓那两份 `_utf8.*` 里的宽串是**汉化版**产物）；
  这条差异由断言守卫（歧义即红）。逐行规则与 9 条断言的完整说明见 `corpus/disasm/README.md`。
* **原始投递件的 sha256 记进 `corpus/assets.json`**（原件只在 `.staging/` 过一手、不入库）——
  这样"没被改过"这件事仍然可复核。

## 3. 锚点纪律

IDA 输出**不具可复现性** ⇒ 整体作为只读资源保存。
**锚点锚二进制里的 EA**；语料只提供 `EA → 当前这份导出里的行号` 映射。
换一次反汇编**只重建映射，不改任何锚**（这条正是旧仓 `raw N` 行号锚全废的教训）。

## 4. 旧仓语料的地位（本轮只登记，不复制）

| 旧仓件 | 地位 |
|---|---|
| `engine/AGERC.DLL.{c,lst}`、`engine/AGERC.DLL_utf8.{c,lst}` | **已被取代**：反汇编目标是**改过文案**的 `install/AGERC.DLL`；现有基于脱壳调试版的新导出 ⇒ 登记为 `archived`，**禁止引用** |
| `engine/天结_unpacked.exe{,_utf8}.{c,lst}` | 汉化改写版的导出；其中 `_utf8.c` 还经过 `sanitize_symbols.py`（**非忠实**）⇒ 只作对照，登记在 `disasm/excluded` |
| `engine/{defs.h,engine.hpp,hxclang_prelude.h}` | Hex-Rays 的配套头文件 ⇒ 本轮明确**不带**（登记在 `disasm/excluded`） |

## 5. 本轮明确不做

* ❌ 不复制任何语料（连 4 个文件也还没入 zip —— 那是 M1）；❌ 不写任何字段 / 函数 / opcode 结论；
* ❌ 不处理两个 exe 的入库（本轮与 M1 都不含）。
