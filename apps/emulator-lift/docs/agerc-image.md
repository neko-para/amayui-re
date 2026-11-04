# AGERC.DLL 镜像取证：ALF 条目 / 安装树根件 / 正版加壳件

> **这是取证记录，不是进度。** 每条结论都带可重跑的命令 / 偏移 / 数字 / sha256；
> 结论里区分**实测**与**推测**，测不了的写"未测"。⛔ 本文件不含"完成度"。
> 口径落点：`repo:apps/emulator-lift/SCOPE.md` §1（平台偏差）· `repo:apps/emulator-lift/docs/plan-m2-2d-text.md` §3 F6 / §4 D6 / §12。
> 引擎语义的锚**只有 EA**（`AGENTS.md` §6）；本文件引用 `.lst` 时一律写 EA，不写行号。

## 0. 一句话结论

`DATA1.ALF` 里那份 `AGERC.DLL`（索引 `SYS4INI.BIN` 第 21072 条）**是免壳的完整 PE**：
7 个正常节、导出 21 个（含引擎要的 3 个）、`.rsrc` 40 条资源**0 条映射失败**、`VS_VERSIONINFO` 可读
（`FileVersion 39.1.0.0`）——**不需要再找/另做拆壳件，也不需要伪 HICON 兜底**。
它与安装树根上那份 `AGERC.DLL` **不是同一份字节**（差 512 字节）：根上是同一 build 经**汉化替换 10 条
MENU/DIALOG 资源**后的产物，`.text` 逐字节相同，图标与版本资源逐字节相同。

- ★ "原件资源在 `.rsrc` 虚拟尾部、文件里没有字节"这条**只对正版安装树根上那份加壳件成立**
  （`repo:links/raw/AGERC.DLL`，335,872 B，AsProtect）：它有 **26/40** 条资源数据项不可映射，
  另外 14 条能映射、其中 **13 条**的字节不是真资源（`RT_MANIFEST` 恰好相同；见 §4.3）。
  **不是 AGERC 的通性。**
- ★ "本机没有拆壳件"**不成立**：免壳件的字节就在 `links/install/DATA1.ALF` 里（同目录的
  `SYS4INI.BIN` 是索引）；`repo:links/raw/AGERC.DLL__dumped.DLL` 还有一份零售版的脱壳 dump。

## 1. 副本清单（逐个给 sha256）

`repo:` 前缀省略；路径都相对仓库根。

| # | 路径 | 字节 | sha256 | 壳 / 构建 | 资源映射 | 用途 |
|---|---|---|---|---|---|---|
| A | `links/install/AGERC.DLL` = `dist/install/AGERC.DLL` = `corpus/assets/agerc/AGERC.DLL` | 848,896 | `6241de666d2f533b66bd48ccd09fa5277acec6473bf099c7b81b0b2c113f977a` | 免壳 · Debug build | **40/40** | 当前测试安装树的根件（汉化产物；`corpus/assets.json` 的 `binary/agerc-dist`） |
| B | `DATA1.ALF` 条目（索引 `SYS4INI.BIN` 第 21072 条） | 849,408 | `f48d1c0fc09b486b003ebd829cf13c9970291a6eaeab13c6d7d0e68e61a297f4` | 免壳 · Debug build（**同一 build 的未汉化版**） | **40/40** | `AGERC.DLL.lst` 语料的真前身（`corpus/assets.json` 的 `binary/agerc-debug-unpacked`） |
| C | `links/raw/AGERC.DLL`（正版安装树根件） | 335,872 | `5f318955f605818965e58d0da2bf9a2a4cef7770758d17e98600ef269035604b` | **加壳**（AsProtect） | **14/40**（且这 14 条字节也不是真资源） | `corpus/assets.json` 的 `binary/agerc-packed`（只登记） |
| D | `links/raw/AGERC.DLL__dumped.DLL` | 425,472 | `a4e6a0b0d27395298b8f51ab780ad2e01fab78ffafe5a3bb538b7cc4bcc56e12` | 零售版的脱壳 dump | 40/40 | 若要"零售版镜像"时的拆壳件（非 Debug build） |
| E | `links/raw/AGERC.DLL__AsProtect.dll` | 186,368 | `98d17eabd3190fe54847a3abc4006b858f6d3694d86071f10a71c993160c01ed` | AsProtect 自身模块，**不是 AGERC** | 无资源目录 | 不作候选 |
| F | `links/raw/_extracted/AGERC.DLL` | 848,896 | `75444e67146eea82788804cb4135ba7e5c0b71d965399cd30bb890e40f686b9e` | 免壳 · Debug build | 40/40 | 旧仓提取期的另一种汉化迭代（只列，不作基线） |

- A/B 的关系是**实测**：同一 `TimeDateStamp`（`0x583B94F6` = 2016-11-28 02:22:46）、同一节表 VA、
  同一 `ImageBase`（`0x10000000`）、同一 `SizeOfImage`（`0x1AC000`）、同一调试 PDB
  （`C:\Git\System4\System\AGERC\3900EU-018\Debug\AGERC.pdb`）；`.text` / `.data` / `.reloc` **逐字节相同**。
  差异集中在 `.rsrc`（10 条资源被汉化替换）以及 `.rdata` / `.idata` 的少量字节（未逐段归因）。
- B 与 `.lst` 的绑定是**实测**：`repo:corpus/disasm/files/AGERC.DLL.lst` 头部
  `; Input SHA256 : F48D1C0FC09B486B003EBD829CF13C9970291A6EAEAB13C6D7D0E68E61A297F4`
  `; File Name : E:\Games\Eushully\天結\raw-parts\DATA1\AGERC.DLL` ⇒ **`.lst` 的 EA 就是 B 的 EA**。
  A 的 `.text` 与 B 相同 ⇒ 同一套 EA 对 A 也成立。

## 2. 从 ALF 切出 B 的可复核记录

### 2.1 索引结构（`SYS4INI.BIN`，446,558 B，sha256 `b6738009b593dab650db5b8e06603c3d29efe822486c7c818edbb9c7b5c9ee1f`）

```text
[0, 4)          'S4IC'                       ← S4 系（单字节文件名；非加装档）
[0, 300)        S4HDR（标题 + unknown）
[300, 312)      S4SECTHDR: uint32 original_length=1692004
                           uint32 original_length2=1692004
                           uint32 length=446246
[312, …)        LZSS 压缩数据 ⇒ 解出 1,692,004 字节的 TOC
TOC:            uint32 archive_count = 8      → DATA1.ALF … DATA8.ALF（每条 256 B）
                uint32 file_count = 21109     （fileHdrOffset = 4 + 8*256 = 2052）
                每条 80 B = name[64] + archiveIndex + fileIndex + offset + length
```

- 布局参数与读法来自 `repo:packages/age-format/src/alf.mts`（`detectLayout` / `readAlfBuffer`）；
  **载荷不做任何解压**：`.ALF` 是裸拼接，索引只给 `(archiveIndex, offset, length)`。
- ★ 索引与归档是**两个文件**：`SYS4INI.BIN` 只有 TOC，字节在 `DATA1.ALF` 里；
  `links/install/DATA1.ALF` 与正版游戏目录里那份是**同一个文件**（NTFS 硬链接：同 inode / `nlink=2`），
  即这份归档**没有被汉化/补丁改过**。索引也不同 inode，但**两份 `SYS4INI.BIN` 字节相同**
  （都是 `b6738009…`，446,558 B）⇒ 正版树里同样能定位到第 21072 条。

### 2.2 条目（第 21072 条）

```text
tocOffset   = 1687816 = 2056 + 21072*80
raw 80 B    = 41474552432E444C4C000000…（name = "AGERC.DLL"）
archiveIndex= 0            → DATA1.ALF
fileIndex   = 0
offset      = 1081523185    (0x4076BBF1)
length      = 849408        (0x000CF600)
offset+length = 1082372593  ≤ DATA1.ALF 的 1,086,863,170 ⇒ 在文件内
```

### 2.3 切出的字节

```text
读 849,408 / 849,408 字节（无短读）
sha256 = f48d1c0fc09b486b003ebd829cf13c9970291a6eaeab13c6d7d0e68e61a297f4
前 16 字节 = 4D 5A 90 00 03 00 00 00 04 00 00 00 FF FF 00 00   ← DOS 头（MZ）
e_lfanew = 0xF0，该处 4 字节 = 50 45 00 00                    ← PE\0\0
```

- ⇒ **载荷没有额外压缩/加密**（否则前 16 字节不会正好是 DOS 头，且 §3 的 PE 结构不会自洽）。
- ⇒ 与 `corpus/assets.json` 的 `binary/agerc-debug-unpacked.origin[0].sha256` **逐字节一致**。

### 2.4 怎么重跑

```bash
# 目录 / 条目（只看索引，不读 1 GB 归档）
node packages/age-format/cli.mjs alf-list links/install/SYS4INI.BIN

# 切一条并核 sha（把 <out> 落 .tmp/，⛔ 不写 links/install）
node --input-type=module -e '
import fs from "node:fs"; import crypto from "node:crypto";
import { readAlf } from "./packages/age-format/src/alf.mts";
const dir = "links/install", alf = readAlf(`${dir}/SYS4INI.BIN`);
const e = alf.entries.find((x) => x.filename === "AGERC.DLL");
const arc = `${dir}/${alf.archives[e.archiveIndex].filename}`;
const fd = fs.openSync(arc, "r"); const b = Buffer.alloc(e.length);
let got = 0; while (got < e.length) got += fs.readSync(fd, b, got, e.length - got, e.offset + got);
fs.closeSync(fd);
console.log(e.offset, e.length, got, crypto.createHash("sha256").update(b).digest("hex"));
'
# 期望：1081523185 849408 849408 f48d1c0f…61a297f4
```

## 3. B（ALF 条目 / 免壳 Debug 版）的逐项判据

### 3.1 文件头

| 字段 | 值 |
|---|---|
| `MZ` / `PE\0\0` | 有 / 有（`e_lfanew = 0xF0`） |
| `Machine` | `0x014C` |
| `NumberOfSections` | 7 |
| `TimeDateStamp` | `0x583B94F6`（2016-11-28 02:22:46） |
| `SizeOfImage` | `0x001AC000`（1,753,088） |
| `AddressOfEntryPoint` | `0x00032C08`（落在 `.text`） |
| `ImageBase` | `0x10000000` |
| `SectionAlignment` / `FileAlignment` | `0x1000` / `0x200` |
| `SizeOfHeaders` | `0x400` |
| `Subsystem` | 2（GUI） |
| `DllCharacteristics` | `0x0140`（`DYNAMIC_BASE` \| `NX_COMPAT`；无 `FORCE_INTEGRITY`） |
| `Characteristics` | `0x2102`（DLL \| 32BIT \| EXECUTABLE_IMAGE） |
| 文件尾 | 最后一节文件尾 `0x000CF600` = 文件大小 ⇒ `overlay = 0` |

### 3.2 节表（逐节）

| # | Name | VirtualAddress | VirtualSize | SizeOfRawData | PointerToRawData | Characteristics | 权限 |
|---|---|---|---|---|---|---|---|
| 0 | `.textbss` | `0x00001000` | `0x000303F5` | **0** | `0x00000000` | `0xE00000A0` | R W X, CODE |
| 1 | `.text` | `0x00032000` | `0x00064DE7` | `0x00064E00` | `0x00000400` | `0x60000020` | R X, CODE |
| 2 | `.rdata` | `0x00097000` | `0x00011B51` | `0x00011C00` | `0x00065200` | `0x40000040` | R, IDATA |
| 3 | `.data` | `0x000A9000` | `0x000AADB8` | `0x00001800` | `0x00076E00` | `0xC0000040` | R W, IDATA |
| 4 | `.idata` | `0x00154000` | `0x0000156C` | `0x00001600` | `0x00078600` | `0xC0000040` | R W, IDATA |
| 5 | `.rsrc` | `0x00156000` | `0x0004FCF8` | `0x0004FE00` | `0x00079C00` | `0x40000040` | R, IDATA |
| 6 | `.reloc` | `0x001A6000` | `0x00005A87` | `0x00005C00` | `0x000C9A00` | `0x42000040` | R, IDATA, DISCARD |

- **`SizeOfRawData == 0` 的节**：`.textbss`（`VirtualSize = 0x303F5`）—— 它是**零填充**节
  （加载器按全零铺），入口点与所有导出/资源都不在其中；不是"资源被挪到虚拟尾部"。
- **`VirtualSize > SizeOfRawData` 的节**：`.textbss`（gap `0x303F5`）、`.data`（gap `0xA95B8`，
  未初始化数据/BSS 的典型形态）。其余 5 节的 `SizeOfRawData` 都 ≥ `VirtualSize` 的页对齐值。

### 3.3 数据目录

| 目录 | RVA | size |
|---|---|---|
| 0 EXPORT | `0x000A8790` | `0x000003C1`（961） |
| 1 IMPORT | `0x00154000` | `0x000000A0`（160） |
| 2 RESOURCE | `0x00156000` | `0x0004FCF8`（326,904） |
| 5 BASERELOC | `0x001A6000` | `0x000044E8`（17,640） |
| 6 DEBUG | `0x00097B50` | `0x0000001C`（28，1 条目） |
| 12 IAT | `0x00154498` | `0x000003F8`（1,016） |
| TLS / LOAD_CONFIG / DELAY_IMPORT / EXCEPTION | 无 | — |

### 3.4 导出（21 个名字 / 21 个函数，Base = 1）

| 导出 | RVA |
|---|---|
| `_GetInstance@0` | `0x00033121` |
| `_ShowDialog@12` | `0x00033446` |
| `_OperateMenu@16` | `0x00032A69` |
| 其余 18 个（`_FindClash@20` / `_GetEv@20` / `_GetLand@20` / `_InitRect@20` / `_SetChParams@20` / `_SetMapEvPN@20` / `_SetMapEvPX@20` / `_SetMapEvPY@20` / `_SetMapLandIdx@20` / `_SetMapLandPN@20` / `_SetMapLandPX@20` / `_SetMapLandPY@20` / `_SetMapParams@20` / `_SetMapWallCr@20` / `_SetMapWallPN@20` / `_SetMapWallPX@20` / `_SetMapWallPY@20` / `_SetNameLenMax@20`） | `0x320DC`–`0x3362B` |

- `_GetInstance@0` 的 RVA 处是 **5 字节 `E9 2A 1C 00 00`** = `jmp 0x10034D50`
  （`0x10033126 + 0x1C2A`）。目标处（`.lst` EA `0x10034D50`）是 `mov eax, hInstance`（`0x10034D6E`）；
  `hInstance` 由 `DllMain` 的 `hinstDLL` 写入（EA `0x10034CD2`/`0x10034CD5`）⇒ **`_GetInstance@0` 返回的就是模块基址**。
  ★ `.lst` 把 `0x10033121` 标成 COLLAPSED、没有列出 jmp 目标；上面这条是**读字节**补的，不靠 IDA 命名。

### 3.5 导入（7 个 DLL / 150 个函数）

| DLL | 函数数 |
|---|---|
| `VERSION.dll` | 3 |
| `WINMM.dll` | 1 |
| `d3d9.dll` | 1 |
| `KERNEL32.dll` | 97 |
| `USER32.dll` | 41 |
| `ADVAPI32.dll` | 3 |
| `SHELL32.dll` | 4 |

（对照 §4.3 的加壳件：8 个描述符 / 10 个函数 —— 差一个量级，这是"加壳"的强判据。）

### 3.6 资源（40 条，**映射失败 0**）

| 类型 | 条目数 | 备注 |
|---|---|---|
| `RT_CURSOR` (1) | 2 | id 1/2 |
| `RT_ICON` (3) | 15 | id 3–17，语言 `1041`；最大的 id 13 = 270,376 B |
| `RT_MENU` (4) | 2 | id 110 / 124 |
| `RT_DIALOG` (5) | 16 | id 2–6, 8–10, 100–105, 134, 136 |
| `RT_GROUP_CURSOR` (12) | 2 | id 106 / 135 |
| `RT_GROUP_ICON` (14) | 1 | **名字 `IDI_ICON1`**，语言 `1041`，216 B，sha256 `a49cb23cb173eda8…` |
| `RT_VERSION` (16) | 1 | id 1，语言 `1041`，816 B，sha256 `ae51fa4244fe5f32…` |
| `RT_MANIFEST` (24) | 1 | id 2，346 B |

- 校验方式：对每条 `IMAGE_RESOURCE_DATA_ENTRY` 的 `OffsetToData` 做 RVA→文件偏移映射，
  要求整段落在某个节的 `PointerToRawData .. +SizeOfRawData` 内。
- **失败 0 条、空数据 0 条。** 最大的图标（id 13，270,376 B）也在 `.rsrc` 的真实文件区间内。

### 3.7 版本资源（`VS_VERSIONINFO`）

```text
VS_FIXEDFILEINFO  sig=0xFEEF04BD strucver=0x00010000
FileVersion    = 39.1.0.0   (MS=0x00270001 LS=0x00000000)
ProductVersion = 39.1.0.0
FileFlagsMask=0x003F  FileFlags=0x0001 (VS_FF_DEBUG)  FileOS=0x40004  FileType=0x0002
CompanyName      = "ARC Software Laboratory"
FileDescription  = "ARCGameEngine Resource"
FileVersion      = "39.1.0.0"
InternalName     = "AGERC"
LegalCopyright   = "Copyright (c) ARC Software Laboratory 2016"
OriginalFilename = "AGERC.DLL"
ProductName      = "ARC Software Laboratory AGERC"
ProductVersion   = "39.1.0.0"
Translation      = 0x0411 / 0x03A4   ← 引擎查的就是 \StringFileInfo\041103a4\FileVersion
```

- 引擎侧（§5）用 `atoi` 把 `"39.1.0.0"` 截成 **39**，再与 `set:RCVersion` 比。
- `VS_FF_DEBUG` = 1 与 §3.8 的 PDB 路径一致 ⇒ **这是 Debug 构建**（不是零售件）。

### 3.8 调试目录（证明"Debug 构建"）

```text
IMAGE_DEBUG_DIRECTORY × 1: Type=2 (IMAGE_DEBUG_TYPE_CODEVIEW), SizeOfData=79,
PointerToRawData=0x00074DD4
RSDS + GUID 2963DC8B7712994F8F3050D6575678B7 + Age 1
PDB = C:\Git\System4\System\AGERC\3900EU-018\Debug\AGERC.pdb
```

### 3.9 "是否像加壳"逐条给值

| 判据 | 实测 | 判 |
|---|---|---|
| 节名特征（UPX / aspack / themida / vmp / pelock / mpress / enigma） | 0 个节命中；节名是 `.textbss/.text/.rdata/.data/.idata/.rsrc/.reloc` | 否 |
| 入口点是否落在可写+可执行节 | `0x32C08` 在 `.text`（`0x60000020`：可执行、**不可写**） | 否 |
| 导入表是否异常小 | 7 DLL / 150 函数、`size=0xA0`（8 个描述符） | 否 |
| 高熵节 | `.text` 5.6473 · `.rdata` 3.7783 · `.data` 3.0764 · `.idata` 4.5351 · `.rsrc` 5.8177 · `.reloc` 5.7448（`.textbss` 0） | 否（全部 < 6） |
| `SizeOfRawData` vs `VirtualSize` 大面积不等 | 只有 `.textbss`（零填充）与 `.data`（BSS 尾）；`.rsrc` 的 raw **大于** VSize | 否 |
| 节权限是否全 RWX | 只有 `.textbss` 是 RWX（且 0 字节）；`.text` X、`.rsrc` 只读 | 否 |
| 是否有 overlay / 附加数据 | `overlay = 0`，最后一节文件尾 = 文件大小 | 否 |

⇒ **B 是免壳的完整 PE**（实测）。加壳的只有 §4.3 的 C。

## 4. 另外两份（C / A）的判据

### 4.1 A = `links/install/AGERC.DLL`（安装树根件，汉化产物）

- 头部与节表 **与 B 同构**：`Machine 0x14C` / 7 节 / `TimeDateStamp 0x583B94F6` / `ImageBase 0x10000000` /
  `EP 0x32C08` / `SizeOfImage 0x1AC000` / `DllCharacteristics 0x0140` / `overlay 0`。
- 差异：`.rsrc` 的 `VirtualSize 0x4FB4A`（B 是 `0x4FCF8`）、`SizeOfRawData 0x4FC00`（B 是 `0x4FE00`）；
  `.reloc` 的 `PointerToRawData 0xC9800`（B 是 `0xC9A00`）；文件小 **512 字节**（848,896 vs 849,408）。
- 资源：40 条、**映射失败 0**。与 B **逐条比 sha256，10 条不同**：
  `RT_MENU 110 / 124` + `RT_DIALOG 2 / 3 / 4 / 5 / 6 / 10 / 105 / 134`（大小也随之变）。
  `RT_ICON`（15 条）、`RT_GROUP_ICON IDI_ICON1`、`RT_VERSION`、`RT_MANIFEST`、`RT_CURSOR` **全部逐字节相同**。
- 版本资源与 B **同一份字节**（sha256 `ae51fa42…`）⇒ `FileVersion 39.1.0.0`、`VS_FF_DEBUG=1` 不变。
- 结论：**A 是 B 的汉化派生件**（同一 build；替换的是菜单/对话框文案），不是另一份构建、
  也不影响图标/版本读取。**A 与 B 不能互相当"字节相同"的证据**（差 512 字节）。
  ⇒ 从 A 读到的**菜单/对话框文案是汉化后的内容**（预期，不是缺陷）；`RT_ICON` / `RT_GROUP_ICON` /
  `RT_VERSION` 仍是**原版字节**。
- 结构完整性：`.text` 逐字节相同；`.rdata` / `.idata` / `.rsrc` 有差异（`.rsrc` 的差异已归因到上面 10 条），
  资源映射 40/40、PE 头/节表/导入/导出都正常 ⇒ **汉化没有破坏 PE 结构**。

### 4.2 与"同一份"的比对结论

| 比对 | 结果 |
|---|---|
| ALF 条目 vs `corpus/assets/agerc/AGERC.DLL` | **不同**（849,408 vs 848,896；sha 不同） |
| ALF 条目 vs `binary/agerc-debug-unpacked`（清单记录） | **相同**（`f48d1c0f…`）—— 见 §1 |
| 安装树根件 vs `corpus/assets/agerc/AGERC.DLL` | **相同**（`6241de66…`） |
| `links/install/AGERC.DLL` vs `dist/install/AGERC.DLL` | 同一个文件（同 inode） |
| `links/install/DATA1.ALF` vs 正版目录 `DATA1.ALF` | 同一个文件（硬链接，同 inode / `nlink=2`）⇒ 归档未被动过 |

### 4.3 C = `links/raw/AGERC.DLL`（正版安装树根件，**加壳**）

```text
size=335,872  sha256=5f318955…  8 节（表见下）
TimeDateStamp=0x583B94A2  ImageBase=0x10000000  EP=0x000F9001  SizeOfImage=0x157000
```

| # | Name | VA | VSize | RawPtr | RawSize | Characteristics |
|---|---|---|---|---|---|---|
| 0 | （空） | `0x1000` | `0x11000` | `0x400` | `0x8200` | `0xE0000040`（RWX） |
| 1 | （空） | `0x12000` | `0x4000` | `0x8600` | `0x1E00` | `0xE0000040` |
| 2 | （空） | `0x16000` | `0x8F000` | `0xA400` | `0x600` | `0xE0000040` |
| 3 | `.rsrc` | `0xA5000` | `0x50000` | `0xAA00` | `0x20400` | `0xE0000040` |
| 4 | （空） | `0xF5000` | `0x3000` | `0x2AE00` | `0x1000` | `0xE0000040` |
| 5 | （空） | `0xF8000` | `0x1000` | `0x2BE00` | `0x1000` | `0xE0000040` |
| 6 | `.data` | `0xF9000` | `0x5D000` | `0x2CE00` | `0x25200` | `0xE0000040` |
| 7 | `.adata` | `0x156000` | `0x1000` | `0x52000` | **0** | `0xE0000040` |

- **8 个节全部 `0xE0000040`（可写 + 可执行）**；6 个节名被抹空；入口点在 `.data`（RWX）。
- 导入：8 个描述符 / **10** 个函数（`kernel32`×3、`version` 1、`winmm` 1、`user32` 1、`advapi32` 1、
  `shell32` 1、`oleaut32` 1、`kernel32` 1）—— 加壳桩的典型形态。
- 节熵：`7.9946 / 7.9760 / 7.8739 / .rsrc 7.9854 / 7.9492 / 1.1373 / .data 7.8319 / .adata 0`
  ⇒ 5 个节接近 8.0（加密/压缩），一个 1.1373 的低熵节是导出桩。
- 数据目录：`EXPORT 0xF8000/0x24C` · `IMPORT 0xF9A60/0x1B8` · `RESOURCE 0xA5000/0x4FCF8` ·
  `BASERELOC 0xF99EC/8` · **[15] `0/0x100000`（非法/壳标记）**；无 DEBUG、无 TLS。
- 导出仍是 21 个名字，但 RVA 落在 `0x1040`–`0x1B10`（第 0 节，RWX）⇒ **不是**免壳件里那些真体的地址。
  **推测**：这些是壳的入口桩（未执行、未验证）。⇒ 这个文件在磁盘上**没有可用的未解包代码布局**，
  也没有可用的资源字节。
- **资源映射：40 条里 26 条失败**（`.rsrc` 的 `VirtualSize 0x50000` > `SizeOfRawData 0x20400`）。
  失败的 26 条：`RT_ICON` id 13–17（5）· `RT_MENU` id 110/124（2）· `RT_DIALOG` 16 条 ·
  `RT_GROUP_CURSOR` id 106/135（2）· `RT_GROUP_ICON` `IDI_ICON1`（1）。
  例：`IDI_ICON1` 的 `OffsetToData=0xF4794`、size=216，delta 已超出 `.rsrc` 的 132,096 字节文件数据。
- 另外 14 条（`RT_CURSOR` 2 + `RT_ICON` id 3–12 + `RT_VERSION` + `RT_MANIFEST`）能映射，
  但 **13 条的字节与真资源不同**（`RT_MANIFEST` 相同）：例如 `RT_ICON` id 3 是
  `92552c196297…`（B 里是 `e4d8a39edd01…`），大小却一样（744）⇒ 资源数据被就地加密/压缩。
- **版本资源在 C 里仍可读**（`0xF9D74` 落在 `.data` 的文件区间内）：`FileVersion 39.1.0.0`、
  `FileFlags=0x0000`（零售，无 `VS_FF_DEBUG`）。⇒ "版本能读"**不能**推出"资源能读"。
- ⇒ 旧结论"原件资源在虚拟尾部、文件里没有字节"**只对 C 成立**（而且比原话更强：
  能映射的那 14 条也不是真资源）。

### 4.4 D = `links/raw/AGERC.DLL__dumped.DLL`（零售版脱壳 dump，非 Debug）

- 425,472 B，sha256 `a4e6a0b0…`；8 个节，全部 `0xE0000040`，节名为空 + `.rsrc` + `.idata` + `poly`；
  `EP 0x7DB4` 在 RWX 节；`TimeDateStamp 0x583B94A2`（零售构建的时间戳）。
- 导入 6 DLL / **125** 函数；无 DEBUG 目录；**资源 40/40 可映射**；版本 `39.1.0.0`、`FileFlags=0x0000`。
- 与 B 的资源逐条比：**只有 `RT_VERSION` 不同**（差的就是 `VS_FF_DEBUG` 这一位），其余 39 条逐字节相同。
- ⇒ 若有人坚持"必须用零售版镜像"，D 是那份拆壳件；但它**与 `.lst` 语料的 build 不同**（`.text` 布局不同），
  **不能拿它的 EA 引 `.lst`**。M2 只需要图标/版本/导出，B 与 D 等价；用 B 更省事且与语料同源。

## 5. 引擎实际怎么拿到 AGERC.DLL（`.lst` 逐字证据）

结论（**实测**）：引擎**直接 `LoadLibraryA("AGERC.DLL")`，按名字交给 Windows 加载器**，读的是
**应用目录（游戏根）** 那份；**没有**"先从 ALF 取出、落盘、再装载"的代码。`DATA1.ALF` 里那份 B
在真机上**不参与** `LoadLibraryA`。所以"从 ALF 装载"是**模拟器的替代策略**（理由见 §6），不是引擎行为。

```text
; —— WinMain（起点 0x4BA890）——
.text:004BA8DC lea     ecx, [ebp+var_338]      ; RCDLL 对象（栈上，-0x338..-0x320）
.text:004BA8E2 call    sub_48E600              ; ctor：vftable + [4]/[8]/[0Ch]/[14h]=0
.text:004BA92F call    sub_48E730              ; 装载 AGERC
.text:004BA934 test    eax, eax
.text:004BA936 jz      loc_4BAB77              ; 装载失败 ⇒ 退出

; —— sub_48E730：按名字 LoadLibraryA + 取 3 个导出 ——
.text:0048E744 push    offset tstrFilename     ; "AGERC.DLL"  (.data:005292D8)
.text:0048E74B call    LoadLibraryA
.text:0048E751 mov     [esi+4], eax            ; HMODULE → [RCDLL+4]
.text:0048E79C mov     edi, GetProcAddress
.text:0048E7A2 push    offset aGetinstance0    ; "_GetInstance@0"
.text:0048E7A8 call    edi ; GetProcAddress
.text:0048E7AA mov     [esi+8], eax            ; → [RCDLL+8]
.text:0048E7B4 push    offset aShowdialog12    ; "_ShowDialog@12"
.text:0048E7BA call    edi ; GetProcAddress
.text:0048E7BC mov     [esi+0Ch], eax
.text:0048E7C6 push    offset aOperatemenu16   ; "_OperateMenu@16"
.text:0048E7CC call    edi ; GetProcAddress
.text:0048E7CE mov     [esi+10h], eax
.text:0048E81B call    sub_48E640              ; 读 "AGERC.DLL" 的 FileVersion → [RCDLL+14h]

; —— sub_48E640：版本来自**文件**（按名字再来一次 Win32 API）——
.text:0048E657 push    offset tstrFilename     ; "AGERC.DLL"
.text:0048E65E call    j_GetFileVersionInfoSizeA
.text:0048E688 push    offset tstrFilename     ; "AGERC.DLL"
.text:0048E68D call    j_GetFileVersionInfoA
.text:0048E6A4 push    offset SubBlock         ; "\\StringFileInfo\\041103a4\\FileVersion"
.text:0048E6AA call    j_VerQueryValueA
.text:0048E6DB call    _strchr                 ; 截到第一个 ','
.text:0048E6F7 call    _atoi                   ; "39.1.0.0" → 39
.text:0048E6FF mov     [ebx+14h], eax

; —— sub_48E620：模块在就尾跳已解析的 [RCDLL+8] ——
.text:0048E620 cmp     dword ptr [ecx+4], 0
.text:0048E624 jnz     short loc_48E629
.text:0048E626 xor     eax, eax
.text:0048E629 mov     eax, [ecx+8]
.text:0048E62C jmp     eax

; —— hInstance / HICON（WNDCLASSEXA，不是 WM_SETICON）——
.text:004BA996 lea     ecx, [ebp+var_338]
.text:004BA99C call    sub_48E620
.text:004BA9A1 push    eax                     ; hInstance = AGERC 模块基址
.text:004BA9A2 mov     esi, LoadIconA
.text:004BA9A8 call    esi ; LoadIconA
.text:004BA9AA mov     [ebp+var_37C.hIcon], eax
.text:004BA9C3 mov     [ebp+var_37C.hIconSm], eax
.text:004BA9FB lea     ecx, [ebp+var_37C]
.text:004BAA02 call    j_RegisterClassExA      ; 类图标在这里生效

; —— 版本比较（0x4BAF0A）——
.text:004BAF18 push    offset aSetRcversion    ; "set:RCVersion"
.text:004BAF1D mov     edx, [eax+4]
.text:004BAF20 call    edx
.text:004BAF26 cmp     eax, [ebp+var_324]      ; var_324 = [RCDLL+14h] = 39
.text:004BAF2C jz      short loc_4BAF3C
.text:004BAF35 push    offset aAgercDll_0      ; "AGERC.DLLのバージョンが異なります．"
```

- `"AGERC.DLL"` 在 AGE.EXE 语料里**只有 3 处代码引用**：`0x48E744`（`LoadLibraryA`）、
  `0x48E657` / `0x48E688`（两次 `GetFileVersionInfo*`）。没有第二处 `LoadLibrary`，
  也没有"读归档 → 写临时文件 → 装载"的代码。
- 因此：**根目录那份**才是引擎实际装载的文件。正版树里它是 C（加壳）；测试安装树里它是 A（汉化免壳件）。
- 负证据：`SetClassLong` / `WM_SETICON` 全语料 0 命中；HICON 只进 `WNDCLASSEXA.hIcon/hIconSm`，
  由 `RegisterClassExA` 生效 ⇒ 图标**不需要**在 headless 里被绘制，但 `LoadIconA` 必须真的从资源解析成功。
- AGE.EXE 自身的版本走另一条路（`0x4B8E43` `GetModuleFileNameA(NULL)` → 读自己的 exe），不参与上面的比较。

### 5.1 「期望版本」`set:RCVersion` 从哪来（2026-11-05 补测）

`0x4BAF26` 那两侧**都查清了**（锚全是 EA；实测值来自 `--until ea` + 寄存器/内存探针）：

| 侧 | 是什么 | EA 级证据 |
|---|---|---|
| 右：实际值 | `[RCDLL+0x14]`（`ebp-0x324`）= `atoi(VerQueryValueA("\\StringFileInfo\\041103a4\\FileVersion"))` | 读版本 `0x48E65E/0x48E68D/0x48E6AA` → `strchr(',')` `0x48E6DB` → `atoi` `0x48E6F7` → 存 `0x48E6FF` |
| 左：期望值 | `Reg::get("set:RCVersion")`：`sub_418940` 之后 `mov ecx,[dword_55E1BC]+0AA514`（= `Engine+697620`）→ `mov eax,[ecx]`（`.data:0x5297DC` 的 `Reg::vftable`）→ `call [eax+4]` = `sub_4904D0`（`sub_428E00` 查表；**未命中会弹「不正なキー」并返回 0**，命中返回 `*(int*)entry`） | `0x4BAF0A`..`0x4BAF20`；vtable `.data:0x5297DC`（slot+4 → `sub_4904D0`） |
| 跳过条件 | `cmp eax, ebx(0)` / `jz` ⇒ **期望值为 0 时整条检查跳过**；`0x4BAF2E` 起才是失败分支（`aAgercDll_0`） | `0x4BAF22` / `0x4BAF24` / `0x4BAF2C` / `0x4BAF2E` |
| 默认值 | **0**（`Reg` 的 ctor 把每个键都塞一遍默认值，`set:RCVersion` 是 0） | `sub_491880`：`v13=0; sub_434D00(this+1, aSetRcversion, &v13)` @`0x4927EF`..`0x4927FB` |
| 非 0 值的唯一来源 | **初始化文件 `SYS4INI.BIN`**（不是注册表、不是 ALF 里的 AGERC 条目）：`_WinMain` 打开它（`S4IC` ⇒ LZSS 解出 1,692,004 B），配置段里的 `RCVERSION` 键 → `atoi` → `Reg::set` | 打开 `0x4BAB1A`（`sub_414AC0`）；键分派 `sub_494220`（`Reg::vftable+44`）的 `_stricmp(aRcversion)` → `0x494EC0` `push aSetRcversion` |
| 注册表不参与 | `RegQueryValueExA` 那条链（`sub_490590`/`sub_491060`）服务的是窗口位置 / 字体链接等；**配置键**走的是初始化文件。缺失即用 ctor 默认值 | `sub_4904D0` 只有"查表"一条路；`sub_494220` 是唯一的 `set:RCVersion` 写入点 |

**2026-11-05 实测（`links/install`，本机 AppData 为空）**：期望值 `eax = 0x27 = 39`；修复前 `[RCDLL+0x14] = 3`、
修复后 `= 0x27 = 39`。根因在**我们**这侧：`VerQueryValueA` 把块里的 UTF-16LE 指针直接交回，引擎
`strncpy_s` 只拷到第一个 `00` ⇒ `atoi("3") = 3`。真机 A 变体的字符串是**单字节 ANSI**、`puLen` **含结尾 NUL**。


## 6. 模拟器装载口径（默认 / 回退 / 缺件）

**一句话（可照做）**：默认先装 `--game-dir` 下的 `AGERC.DLL`；**若它不是可映射的 PE 或 `.rsrc` 有映射失败**
（正版树里那份 C 就是这种），就回退到"用 `--game-dir/SYS4INI.BIN` 的 TOC 找 `AGERC.DLL` 条目、
按 `(archiveIndex, offset, length)` 从同目录 `DATA1.ALF` **直读**那一段"作为镜像；`--agerc <path>` 覆盖以上两步；
两步都拿不到 ⇒ `LoadLibraryA("AGERC.DLL")` 返回 0 + 错误 126，⛔ 不伪造句柄 / 不伪造资源。

- 当前测试安装树（`links/install`）走**默认路径**就是 A：免壳、40/40 可映射 ⇒ **P0 不需要 `--agerc`**。
- 正版树（`links/raw`）走**回退路径**得到 B：免壳、40/40 可映射 ⇒ **不需要额外找拆壳件**。
- 这不是"引擎会从 ALF 装载"（§5 已否证），而是**在拿不到可映射的原件时，用同一模块的免壳构建代替**：
  ① B 就是 `.lst` 语料的真前身（§1）；② B 与 A 的**图标与版本资源逐字节相同**，与 C 的**导出名与版本字符串
  相同**（C 的图标字节不可读，见 §4.3）；③ 三个导出按 `SCOPE.md` §1 仍是 no-op stub，代码字节不参与行为。
- 由 §4.1：**A 与 B 的图标 / 版本资源逐字节相同**，所以"M2 的图标/版本"这条路上，
  默认路径与回退路径**不可区分**（除非将来要读菜单/对话框资源 —— 那时才会看到汉化差异，且那是预期）。

> ★ `repo:apps/emulator-lift/SCOPE.md` §1 的"模态"行原先写着"原件典型 .rsrc 虚拟尾部、文件里没有字节"（**2026-11-05 已按本文件的取证改正**），
> 这是**针对加壳件的实测**被误当成通性。★ 该文件第 5 行规定"**不允许在用户没有明确要求的情况下编辑这个文件**"
> ⇒ 本文件**不改它**；等用户明确要求时把该句替换成"正版根件是加壳件（26/40 资源无文件字节），
> 免壳件在 `DATA1.ALF` / 安装树根上，资源可读；伪 HICON 只作应急"。

## 7. 实施建议（给后面写 DDraw / 字体 / 资源代理；一行一条）

1. `apps/emulator-lift/src/main.cpp`（`main()` 里"AGERC.DLL：作为真实 guest 模块装载"段）：
   把"显式 `--agerc` → `--game-dir/AGERC.DLL`"改成三步（显式 → 根件 → ALF 条目），
   并把**来源标签**（`root` / `alf:SYS4INI.BIN#21072`）与 sha256 一起传给 `Machine::loadAgercModule`。
2. 新增 `apps/emulator-lift/src/runtime/alf_index.{h,cpp}`：只解析索引头（`S4IC`/`S5IC` + LZSS 段 + TOC）、
   `findEntry(name)`、`readPayload(dir, entry)`（`openHostFileUtf8` + `fseek/fread`，**按需读**，⛔ 不把 1 GB 读进内存）。
   ⛔ 不要搬 `apps/emulator-cxx/src/core/alf.*`（那是另一套 core；读法可参考）。
3. `apps/emulator-lift/src/runtime/machine.cpp` `Machine::loadAgercModule`：接受 `(bytes, origin_label)`，
   保留 `sha256` 与 `agerc_base = 真实 ImageBase`；`cpu.shim.agerc_image` / `agerc_base` 语义不变；
   启动日志打 `agerc: <origin> sha256=<...> base=... resources=yes`。
4. `apps/emulator-lift/src/shims/user32_resource.cpp` `LoadIconA`：把"伪 HICON"从**预期路径**降级为
   **应急**（只有 `.rsrc` 数据项映射失败时才用，且必须 loud log）；正常路径从 `cpu.shim.agerc_image`
   读 `RT_GROUP_ICON`（`IDI_ICON1`）→ 逐个 `RT_ICON`（id 3–17，lang 1041）拼 `HICON`。
   A 与 B 的图标字节相同 ⇒ 不需要按来源分支。
5. `apps/emulator-lift/src/shims/kernel32_version.cpp`：逻辑不用改（已优先 `agerc_image`）；
   加一条启动日志打 `FileVersion/ProductVersion`（期望 `39.1.0.0`），并保证
   `LoadLibraryA` 与 `GetFileVersionInfo*` **指向同一份字节**（否则"镜像版本"与"文件版本"会分叉）。
6. 守卫：加一条 `assets` 级测试（`tools/test/…` 或 `apps/emulator-lift/tests/agerc_module.cpp`）——
   从 `links/install` 按 §2.4 切出的 `AGERC.DLL` 必须 sha256 = `f48d1c0f…`，
   `RT_GROUP_ICON/IDI_ICON1` 216 B 的 sha256 = `a49cb23c…`；安装树根件的对应值 = `6241de66…` / `a49cb23c…`
   （**图标相同是预期**，不是 bug）。

## 8. 未测 / 需要真机

- **未测**：正版 Windows 上把根目录 `AGERC.DLL` 改名后引擎是否报
  「AGERC.DLLを読み込み出来ません」——这是"按名字加载根目录那份"的**运行期**闭合实验；
  当前只有代码级证据（§5）。
- **未测**：AsProtect 运行期解包后的内存镜像（D 是旧仓的 dump，dump 工具/参数未知；未与运行期内存比对）。
- **未测**：A 的 10 条汉化资源与 B 的**内容级**对账（只比了 sha256 与大小；文案差异未逐条展开）。
- **已测（2026-11-05）**：`set:RCVersion` 的默认值与唯一来源 —— 见 §5.1（默认 0 = 跳过检查；
  非 0 值来自 `SYS4INI.BIN` 的 `RCVERSION` 键；本机期望 39，`[RCDLL+0x14]` 也是 39）。
- **未测**：图标的实际像素渲染（headless 不渲染；`LoadIconA` 只验证"资源解析成功"）。
- **推测**：B 是"开发者把 Debug 构建误放进 DATA1.ALF"还是"发行流程有意放一份免壳件"——无证据，不作结论。
