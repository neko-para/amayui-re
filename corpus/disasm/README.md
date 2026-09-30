# corpus/disasm/ — 反汇编语料（4 个文件，一个 zip）

```text
corpus/disasm/
  disasm-20260930.zip    # ★ 入库（走 LFS）—— 内含 4 个 **UTF-8 + LF** 文件（M1 产出）
  README.md              # 本文件：清单 + 来源 + sha256 + 转写规则 + 断言口径
  files/                 # ← gitignore：解压产物；agent 直接读这里的
```

**为什么合成一个 zip**：这 4 个文件是**只读、基本不会再改的归档工件**；合成一个 blob 既不产生逐文件 diff 噪声，
文本压缩率又高（原始 49 MB → zip 6.77 MB）。**入库的是 zip；解压产物 ignore**（本地解一次即可）。

**为什么只带这 4 个**（§1.8.1）：即使把旧仓那对 `天结_unpacked.exe` 也迁进来，**其余文档仍要到旧仓原位置查**，
并不能自包含 ⇒ 半迁移只增加"重复且会不一致"的风险，换不来可用性。

---

## 1. 清单与来源

| # | zip 内文件名 | 真前身（`derivedFrom`） | 原始投递件位置 | 原始 sha256 |
|---|---|---|---|---|
| 1 | `AGE.EXE__dumped.sectfix.EXE.c` | `binary/age-sectfix` | `staging:AGE.EXE__dumped.sectfix.EXE.c` | `75ba5371aca6fe7f86b6ed217f06d53069f4f4dd653e5300b7fc8a4a2c97bd9e` |
| 2 | `AGE.EXE__dumped.sectfix.EXE.lst` | `binary/age-sectfix` | `staging:AGE.EXE__dumped.sectfix.EXE.lst` | `931c576f73bd33b955dbf35e6979a03ecfd98ef21c98350db35c669632da1352` |
| 3 | `AGERC.DLL.c` | `binary/agerc-debug-unpacked` | `staging:AGERC.DLL.c` | `f213ac2e041b813f1f7ead1c1266435ab50ae9d98d4ebe398d65cb2ab5920aad` |
| 4 | `AGERC.DLL.lst` | `binary/agerc-debug-unpacked` | `staging:AGERC.DLL.lst` | `f70bf5589692988987ae138a51a43c51f83056e22bbff4bdeca3cc791e495e71` |

* **原始投递件只在 `.staging/` 过一手**（仓库内的中转区，gitignore，**不是"来源"**）——
  上表 sha256 就是"它没被改过"的证据；机器真源是 `corpus/assets.json`
  （`disasm/raw-source-sectfix` / `disasm/raw-source-agerc`，同值）。
* `binary/age-sectfix` = `oldRepo:raw-parts/AGE.EXE__dumped.sectfix.EXE`（节表修补后的**基线二进制**，
  与原文件仅 24 字节不同）。
* `binary/agerc-debug-unpacked` = `oldRepo:raw-parts/DATA1/AGERC.DLL`（**不带壳的调试版**，有调试入口）。
  ⚠ 旧仓 `engine/AGERC.DLL.{c,lst}` 与 `_utf8` 那对是针对**被改过文案**的 `install/AGERC.DLL`
  反汇编的 ⇒ **已被取代**（登记为 `disasm/superseded-agerc-oldsrc`，禁止引用）。
* 两个 `AGE.EXE__dumped.EXE.{c,lst}`（节表修补**前**的导出）**明确不带**：留档在 `.staging/`，
  登记在 `disasm/excluded`。

## 2. 转写规则（**不是本仓发明的，沿用旧仓已定案并实测过的口径**）

规则来源（旧仓，只读参考）：

| 旧仓文件 | 提供了什么 |
|---|---|
| `docs-new/03-engine/agerc-module.md` §4 | 转写规则 1–4 + **半角假名前导字节 bug 的修正** |
| `.tmp/convert_to_utf8.py` | 引擎本体那份的逐行字节解码方法（规则 1/2/4） |
| `.tmp/convert_agerc_utf8.py` | AGERC 那份：在其上加了 UTF-16LE 宽串规则与修正版字节判定 |
| `docs-new/99-records/2026-09-disasm-baseline/disasm-baseline.md` | 验收口径：**假名计数才是编码判别器**（GBK 解也 0 个 U+FFFD，但 0 个假名） |

逐行规则：

1. **纯 ASCII 行 ⇒ 原样**（逐字节等价）。
2. **含 `; File Name   :` 的行 ⇒ 整行按 GBK**（IDA 用运行机器的 ANSI 码页写路径：`CC EC BD 59` = `天結`）。
3. **其余 ⇒ 按 CP932 的字节结构逐字节解**：
   * `0xA1..0xDF` 是**单字节**半角假名，**永远不是前导字节**（旧仓的 bug 就在这：盲试两字节会把 `B1 8F`
     配成汉字，`ｱ` 变成 Latin-1 `±`；修正后 AGERC 两份 0 处 Latin-1 残留）；
   * `A1 F4..FE` 是 IDA 的 GBK 箭头字形（◆□■△←↑↓〓）；
   * CP932 解不出的**单字节保留原码位**（IDA 的 `; '\x80'` 字节字面量 ⇒ U+0080）。
4. **不启用 UTF-16LE 宽串规则 —— 本轮实测不需要，且这条差异被断言钉住了**：
   旧仓那两份 `_utf8.*` 里的宽串（`显示消息窗口(`）是**汉化版**的产物；本轮 4 个文件来自**原版**二进制，
   宽串上下文里**一个真宽串都没有** —— `dwTypeData` 下的高字节串按 UTF-16LE 解是乱码（`꿒낾…`），
   按 CP932 解才是正文（`ﾒｯｾｰｼﾞｳｲﾝﾄﾞｳを表示する(&O)`）。
   ⇒ 断言 #9 把这件事**机械化**：宽串上下文里若出现「CP932 可解」且「UTF-16LE 也像正经文本」的歧义串，
   断言直接红，逼人裁决（而不是静默按某一边解）。

★ **永远的禁令**：转写只做"解码 + 行尾"，**不做任何符号改写** ——
❌ 禁止 `::` → `__`、❌ 禁止 `this` → `_this`、❌ 禁止折叠空格或重命名符号。
旧仓 `sanitize_symbols.py` 就是这么把语料搞坏的：同一份语料 raw 的 `::` **4754** / `this` **36753**，
其 `_utf8.c` 只剩 **3** / **1** ⇒ 据此得出的字符串层结论全不可信（见 `docs/00-origin/decisions.md` §4）。

## 3. 实测数据（`pnpm recode -- --verify` 的真实输出）

| 文件 | 原始字节 | 行数 | 非 ASCII 行 | GBK 行 | 保留原码位 | 宽串上下文串 | `::` | `this` | 假名（CP932 解 / GBK 解） |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `AGE.EXE__dumped.sectfix.EXE.c` | 5,416,797 | 184,516 | 751 | 0 | 0 | 0 | 5,216 | 36,961 | 10,181 / **0** |
| `AGE.EXE__dumped.sectfix.EXE.lst` | 18,916,332 | 530,297 | 2,590 | 1 | 574 | 0 | 12,843 | 3,339 | 21,193 / **0** |
| `AGERC.DLL.c` | 176,978 | 5,102 | 29 | 0 | 0 | 2 | 127 | 151 | 165 / **0** |
| `AGERC.DLL.lst` | 27,933,338 | 1,074,604 | 571 | 1 | 515 | 3 | 543 | 63 | 265 / **0** |

> "假名 / GBK 解" 一列是**编码判别器**：同一份字节按 GBK 解出的假名数必须是 **0**（旧仓口径）。
> 这说明主体确实是 CP932 日文，而不是 GBK。

## 4. 怎么跑

```bash
pnpm recode -- --verify        # 只断言（不落盘）—— 守卫 #7 也走这条
pnpm recode -- --build         # 转写落盘到 files/ + 打确定性 zip（M1 用）
```

`--verify` 的 9 条断言（**任一不过 ⇒ 退出码 1，且 `--build` 拒绝产出任何工件**）：

| # | 断言 | 说明 |
|---|---|---|
| 1 | 源文件是干净的 CRLF（LF 数 == CRLF 数，无孤立 CR） | 否则"只动行尾"这句话本身不成立 |
| 2 | 行数不变 | 行号引用（`EA → 行号` 映射）的前提 |
| 3 | LF 数不变 | |
| 4 | 输出是 LF-only（无任何 CR） | |
| 5 | ★ **逐行反解回字节 == 源字节** | **最强的一条**：把输出文本按同一套判定反解回字节，与源行**逐字节相同**（GBK 行用 GBK 表反解）⇒ 没有字符被丢、被换、被合并 |
| 6 | 纯 ASCII 行原样 | |
| 7 | 输出无 `U+FFFD` | 没有静默替换 |
| 8 | 编码判别器（见 §3） | |
| 9 | ★ 宽串守卫（见 §2 规则 4） | 歧义必须红，不许静默选边 |

反解表（文本 → 原字节）由**同一套解码原语**枚举所有可解序列现场构造，不依赖任何编码器 ⇒
"忠实"这件事是**自证**的，不是靠信任某个库。

`--build` 用固定时间戳 + deflate 打包 ⇒ **同输入同字节**（已实测两次打包哈希相同）。
产物解压后 **UTF-8 + LF + 无 BOM**（已用 `Expand-Archive` 校验过）。

## 5. 消费规则

* **只读**：不得就地修改；任何"解析友好化"只能是**派生的内存视图**，不得落回语料文件。
* **锚点锚二进制 EA**：语料只提供 `EA → 当前这份导出里的行号` 映射。
  换一次反汇编**只重建映射，不改任何锚**（这正是旧仓 `raw N` 行号锚全废的教训）。
* `files/` 里的解压产物**不入库**（`.gitignore` 的 `/corpus/disasm/files/`），本机解一次即可。

## 6. M1 的三步

```bash
pnpm recode -- --build          # 1) 产出 files/ 与 disasm-20260930.zip
pnpm corpus -- --set disasm/bundle '{"storage":"lfs","dest":"corpus/disasm/disasm-20260930.zip"}' --write
                                # 2) 翻牌：deferred → lfs（写后回读复验，不绿回滚）
pnpm validate                    # 3) 守卫 #6 会核对 git check-attr filter == lfs
```

翻牌后 `disasm/bundle` 就不再是"未产出件"，守卫 #7 会自动开始真跑上面的断言（作用域只对**入库的那一份**强制；
`disasm/raw-source-*` 与 `disasm/excluded` 豁免 —— 它们是参照件与清单，不是被加工的语料）。
