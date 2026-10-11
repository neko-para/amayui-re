# 台账锚的 repo 枚举缺「游戏安装根」档：6 个锚指向 dist/install/SYS4INI.BIN 永远无法机械复核

- id: REQ-01M4NG7X2RXAJB4HEFR0EHMZ5V
- type: req
- status: open
- parent: REQ-01M3TD4N309J3Y6YMWWR2GTQAA

## 问题
**6 个** `bin` 锚（`repo=reference`）的 `path` 是 `dist/install/SYS4INI.BIN` —— 那是**游戏安装树**的相对路径，
而 `resolveAnchor` 把 `repo=reference` 解析到**只读参考仓根**（清单 `roots.oldRepo`）⇒ 永远"取不到"，
只能靠不变量 #4 的 warning 豁免活着（`pnpm tools ledger report` 的"参考仓取不到（warn）=9"里有 6 个是它）。
⇒ 这 6 条观察**没有任何机械判据**能发现"那段字节被换掉"（它们的 `len`/`sha256` 都齐，却永远算不出来）。

★ 它们全在**历史行**（`01M4GSFEH2422M416W404Z1E16` · `01M4GT0H8G5Q5J0Q584K5D4A08` ·
`01M4H0NJNQ1G7Q575C082K0605` ×2 · `01M4H0PC8K294Z3T373D775H7A` ×2，均 `retracted`）
⇒ append-only 口径下**不就地改**。

## 要做什么
1. 给锚点 `repo` 加**第三档**（例如 `gameInstall`）：`resolveAnchor` 按清单 `roots.gameInstall` 解析，
   `len`/`sha256` 照样现算（摘要的硬要求在**新档**上同样生效，不许因为"换了个档"就免检）；
2. 门禁与只读帮助动作同步支持新档：`pnpm tools ledger validate` 的不变量 #3（`repo` 不越出对应仓库根）、
   `--hash-anchor`、守卫的 `digestOf`；
3. **后继记录用新档**：将来重挂这 6 条观察时用新档 + `len` + `sha256` **追加**新记录（历史行留档不动）。

## 判据
* `pnpm tools ledger report` 的"参考仓取不到（warn）"里，不再有 `dist/install/SYS4INI.BIN` 这类
  **安装树相对路径**（新记录一律走新档）；
* 一条守卫：`repo=gameInstall` 的 `bin` 锚，在场时**必须**解析得到、且 `len`/`sha256` 与现算一致
  （安装树不在场时按既有口径 warn —— 不许把"这台机器上没有"记成"验过了"）。

## 不做
不追认那 6 条历史行（append-only）；不改写 `self`/`reference` 的既有语义；
不把"游戏安装树"与"只读参考仓"合并成一档（那是两台机器上完全不同的东西）。
