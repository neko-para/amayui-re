# corpus/disasm/ — 反汇编语料（4 个文件，一个 zip）

```text
corpus/disasm/
  disasm-20260930.zip    # ★ 入库（走 LFS）
  README.md              # 本文件：口径 + 怎么跑 + 怎么查（**不列文件表**）
  files/                 # ← gitignore：解压产物；agent 直接读这里的
```

**内容契约**：zip 里**正好 4 个 UTF-8 + LF 文件** —— 基线 `AGE.EXE__dumped.sectfix.EXE.{c,lst}` 与配套模块 `AGERC.DLL.{c,lst}`。
任何一次转写都必须正好是这 4 个（多一个少一个都是口径错误）。

**为什么合成一个 zip**：这 4 个文件是**只读、基本不会再改的归档工件**；合成一个 blob 既不产生逐文件 diff 噪声，
文本压缩率又高。**入库的是 zip；解压产物 ignore**（本地解一次即可）。

**为什么只带这 4 个**（§1.8.1）：即使把旧仓那对 `天结_unpacked.exe` 也迁进来，**其余文档仍要到旧仓原位置查**，
并不能自包含 ⇒ 半迁移只增加"重复且会不一致"的风险，换不来可用性。

## 1. 逐件的来源 / 前身 / sha256 在哪（**不在本文件**）

| 想知道 | 真源 |
|---|---|
| zip 由哪些文件构成、每个文件从哪个二进制导出（真前身）、去哪 | `corpus/assets.json` 的 **`disasm/bundle`**（`origin` = 4 个投递原件，`derivedFrom` = 两个二进制） |
| **投递原件的 sha256**（"没被改过"的证据） | 同上的**来源记录**条目 `disasm/raw-source-sectfix` / `disasm/raw-source-agerc` 的 `origin[].sha256` |
| 条目 / 存储去向 / 哪些是 deferred | `pnpm tools corpus list` |
| 明确**不带**的那些（修补前导出、旧 exe 那对、3 个头文件） | `disasm/excluded`；已被取代的旧 AGERC 语料 → `disasm/superseded-agerc-oldsrc` |

★ 本 README **故意不抄**文件清单、哈希、行数、计数：它们一变就会不一致（口径见 `docs/00-origin/decisions.md` §6）。

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
4. **不启用 UTF-16LE 宽串规则 —— 本批实测不需要，且这条差异被断言钉住了**：
   旧仓那两份 `_utf8.*` 里的宽串（`显示消息窗口(`）是**汉化版**的产物；本批 4 个文件来自**原版**二进制，
   宽串上下文里**一个真宽串都没有** —— `dwTypeData` 下的高字节串按 UTF-16LE 解是乱码（`꿒낾…`），
   按 CP932 解才是正文（`ﾒｯｾｰｼﾞｳｲﾝﾄﾞｳを表示する(&O)`）。
   ⇒ 断言 #9 把这件事**机械化**：宽串上下文里若出现「CP932 可解」且「UTF-16LE 也像正经文本」的歧义串，
   断言直接红，逼人裁决（而不是静默按某一边解）。

★ **永远的禁令**：转写只做"解码 + 行尾"，**不做任何符号改写** ——
❌ 禁止 `::` → `__`、❌ 禁止 `this` → `_this`、❌ 禁止折叠空格或重命名符号。
旧仓 `sanitize_symbols.py` 就是这么把语料搞坏的：同一份语料 raw 的 `::` **4754** / `this` **36753**，
其 `_utf8.c` 只剩 **3** / **1** ⇒ 据此得出的字符串层结论全不可信（见 `docs/00-origin/decisions.md` §4）。

## 3. 怎么跑

```bash
pnpm tools disasm verify          # 断言（守卫 #7 也走这条）
pnpm tools disasm build           # 转写落盘到 files/ + 打确定性 zip（需要 .staging/ 里的原件）
pnpm tools disasm restore         # 由 zip **反解回投递原件**（默认写回 .staging/）

# ★ 读语料是**另一个域**（`disasm-at`：只读定位与切片，`--describe` 看全部动作）
pnpm tools disasm-at stats                        # 段 / 行数 / EA 范围 / 单调性
pnpm tools disasm-at at --ea 0x401000 --lines 60  # 有界上下文（.lst；截断会明说）
pnpm tools disasm-at pseudo --ea 0x40D500         # ★ 该 EA **所属函数**的 Hex-Rays C 体 + .lst 区间
```

`--verify` 有**两种基准**，按 `.staging/` 在不在自动选，两种都会用清单里的 sha256 上锚：

| 模式 | 什么时候 | 拿什么当基准 | 断言 |
|---|---|---|---|
| **staging** | `.staging/` 里有投递原件 | 原件本身 | 9 条断言 + ①**原件 sha256 == 清单记录**（真实性锚） |
| **restored** | 原件已不在（**fresh clone 就是这样**） | 由 zip 反解出的字节 | 反解结果 sha256 == 清单记录的原件 |

9 条断言（**任一不过 ⇒ 退出码 1，且 `--build` 拒绝产出任何工件**）：

| # | 断言 | 说明 |
|---|---|---|
| 1 | 源文件是干净的 CRLF（LF 数 == CRLF 数，无孤立 CR） | 否则"只动行尾"这句话本身不成立 |
| 2 | 行数不变 | 行号引用（`EA → 行号` 映射）的前提 |
| 3 | LF 数不变 | |
| 4 | 输出是 LF-only（无任何 CR） | |
| 5 | ★ **逐行反解回字节 == 源字节** | **最强的一条**：把输出文本按同一套判定反解回字节，与源行**逐字节相同**（GBK 行用 GBK 表反解）⇒ 没有字符被丢、被换、被合并 |
| 6 | 纯 ASCII 行原样 | |
| 7 | 输出无 `U+FFFD` | 没有静默替换 |
| 8 | 编码判别器：CP932 产物含假名、整文件按 GBK 解不含假名 | |
| 9 | ★ 宽串守卫（见 §2 规则 4） | 歧义必须红，不许静默选边 |

反解表（文本 → 原字节）由**同一套解码原语**枚举所有可解序列现场构造，不依赖任何编码器 ⇒
"忠实"这件事是**自证**的，不是靠信任某个库。

`--build` 用固定时间戳 + deflate 打包 ⇒ **同输入同字节**；产物解压后 **UTF-8 + LF + 无 BOM**。
**要具体数字（行数 / `::` / 假名数 / 保留原码位…）就跑 `--verify`**（它逐文件逐条打印），本 README 不存快照。

## 4. 消费规则

* **只读**：不得就地修改；任何"解析友好化"只能是**派生的内存视图**，不得落回语料文件。
* ★ **两层分工：C 读 / lst 证**。`.c` 是 Hex-Rays 的**改写视图**（类型 / 变量名 / 结构都是它的推断，
  而且**一处地址都没有** —— 实测 `0x00xxxxxx` 计数为 0）⇒ 它**只用来提假设与看结构**；
  结论必须回 `.lst` / 字节核验，**锚只锚 EA**（`.c` 的行号与文本不许当锚）。
  ★ 不是每个 EA 都是函数起点、也不是每个函数都有 C ⇒ 用 `disasm-at pseudo` 让它**算**给你（别自己看行区间）。
  机械判据：`tools/test/disasm-pseudo.assets.test.mjs`；完整工作流与常踩的坑：
  技能 `.agents/skills/amayui-re-engine/SKILL.md`。
* **锚点锚二进制 EA**：语料只提供 `EA → 当前这份导出里的行号` 映射。
  换一次反汇编**只重建映射，不改任何锚**（这正是旧仓 `raw N` 行号锚全废的教训）。
* `files/` 里的解压产物**不入库**（`.gitignore` 的 `/corpus/disasm/files/`），本机解一次即可。

## 5. 入库流程（重做 / 换语料时照这个走）

```bash
pnpm tools disasm build          # ① 产出 files/ 与 disasm-20260930.zip（需要 .staging/ 里的原件）
pnpm tools corpus set disasm/bundle '{"storage":"lfs","dest":"corpus/disasm/disasm-20260930.zip"}' --write
                                # ② 翻牌：deferred → lfs（写前内存预验；写后回读复验，不绿回滚）
pnpm tools corpus validate                    # ③ 守卫 #6 核对 filter == lfs；#7 真跑转写断言
```

**投递原件（`.staging/` 里那几个 CP932 文件）随时可以丢**：`.staging/` 是 gitignore 的**中转区**，
而 zip 里的 UTF-8 文本经断言 #5 保证**逐字节可反解**回原件，清单里又记着原件的 sha256 ⇒
`pnpm tools disasm restore` 随时能把原件一字不差地还原回来。`pnpm tools corpus validate` 在原件缺席时自动切到
`restored` 模式（见 §3 的表），所以 **fresh clone 上守卫同样是绿的**。
