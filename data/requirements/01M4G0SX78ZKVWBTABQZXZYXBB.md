# mac 的 amayui-cn/raw 有 41 支散装基线 BIN 被旧仓汉化产物覆盖（Windows 侧正确；已逐字节还原）

- id: REQ-01M4G0SX78ZKVWBTABQZXZYXBB
- type: bug
- status: done
- parent: REQ-01M3TC9660ZG1YMB7KAKE4W94B
- repro: tools/lib/patch.mjs#基线指纹不符
- severity: S2
- tags: [corpus, patch, baseline]

## 是什么（★ 两侧必须分清）

**坏的是 mac 上的这份拷贝** —— 不是 Windows 侧，也不是游戏本体：

| 侧 | 状态 | 判据（都可复算） |
|---|---|---|
| **Windows**：`E:\Games\Eushully\天結\raw` | **正确（原版）** | 旧仓自己的 `raw-manifest.json`（2026-08-03 生成）记的那 41 个 `md5`，与**重汇编出的原版字节**逐一相同 —— **41 / 41** |
| **mac**：`amayui-cn/raw`（普通目录，**不是**符号链接） | **被覆盖成汉化产物** | 覆盖前的内容与旧仓 `patch/BIN/<同名>`（= 旧仓**汉化补丁产物**目录）的 `md5` 逐一相同 —— **41 / 41**（其中 18 件同时也等于 `install/` 里那份） |

⇒ mac 那 41 件的**内容就是旧仓的汉化产物**：既不是"另一种原版"，也不是随机漂移。

★ **不是新仓写坏的**：新仓首个提交是 `2026-09-30`，而那些文件的 mtime 是 `2026-09-15T05:09`
（这一簇与"散装 ≠ `baseSha`"那 41 支**集合完全相等**）—— 时间上新仓还不存在。

★ **Windows 侧的 junction 是另一件事，别与本条混为一谈**：旧仓 `scripts/setup.js:34` 把 `raw` 做成
指向 `GAME_DIR`（游戏本体目录）的 junction ⇒ **在 Windows 上"往 `raw/` 写"= 往游戏本体写**，那边动
`raw/` 要格外小心（`config.js` 注释本已禁止汇编产物写这里）。但 **mac 上没有这个 junction**，
"往 mac 的 `raw/` 写"只写这份拷贝 ⇒ **junction 解释不了这次覆盖**。

## 观测

| 判据 | 修前 | 修后 |
|---|---|---|
| `pnpm tools patch verify` | **41 红** / 453（`基线指纹不符 …（来源 loose:<名>）`） | **453 / 453 通过 · 失败 0** |
| 散装件 ≠ `patch.json` 的 `baseSha` | 41 支 | **0 支**（66 支散装全部相符；387 支走 ALF） |
| `SC0000.BIN` 的 base 视图 | 中文「好鼇害，真是年代久遠的遺迹…」 | 日文「凄いな、年代物の遺跡だ。修理の依頼を受けて正解だった」 |

★ 这 41 支**不是"多出来的文件"**，而是**本来就有、被覆盖了内容**：

* 旧仓 `raw-manifest.json`（149 件名单）里 **41/41 都在**；与名单相比本机真正"多出来"的只有 7 项
  非 BIN（`.DS_Store` 与 6 个备份目录/文件）；
* 它们同时也是 ALF 归档里的成员（散装件本来就是官方补丁、覆盖归档里的旧版）。

★ 别删这些散装件：删了只会回退到 ALF 里**更旧的日文版**，`verify` 照样红（ALF 副本对 41 支
**全都不等于** `baseSha`，逐支验过），还把原版文件丢了。

## 怎么还原（可重复的配方）

原版字节来源 = 旧仓 `data/<名>.txt`（941 支**日文基线**文本，mtime 09-11）经
`node packages/age-format/cli.mjs asm-asm <txt> --out <bin>` 重汇编。

**写入前**逐件过两道闸门（任一不过 ⇒ 一个字节都不写），并先留回退快照：

1. 重汇编结果 `sha256` == `patch.json` 的 `baseSha`　→ **41 / 41**
2. 重汇编结果 `md5` == 旧仓 `raw-manifest.json`（**Windows 侧**的记录）的条目　→ **41 / 41**

**写入后**复核：`patch verify --base <修好的根> <这 41 支>` **41/41 通过**；全量 `patch verify` **453/453**。
派生缓存要一并刷新（基线字节 + mtime 变了）：`pnpm tools patch index --write` + `pnpm tools patch view`
（实测基线指纹 `b2559334…` → `bfa5b987…`）。

## 重开条件

`pnpm tools patch verify` 再次报 `基线指纹不符（来源 loose:*）` ⇒ 又有一侧被覆盖了。**先分侧**：
拿 `patch/BIN/<同名>` 与（Windows 记录）`raw-manifest.json` 各比一次即可当场判定，再按「怎么还原」重跑。

## 残留（有意不单开单）

mac 上**具体是哪一步**把这 41 件从 `patch/BIN/` 抄进了 `raw/`，本机已无法复原（相关动作发生在
2026-09-15，早于新仓首个提交，且旧仓 `raw/` 不在其 git 里）。因旧仓已列入退役（批 M8）、且本域已有
`patch verify` 当哨兵，故不单开节点，只在此记明边界。
