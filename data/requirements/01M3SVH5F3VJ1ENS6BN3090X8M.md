# windows 资源重建（res 的 rc 系列重新设计组织）

- id: REQ-01M3SVH5F3VJ1ENS6BN3090X8M
- type: req
- status: dropped
- parent: REQ-01M3SVH2V332E4CJYN0BHH0YK8
- order: 25
- dropped_reason: 实质＝AGERC.DLL 的修改逻辑；已裁决直接使用入库可信产物（binary/agerc-dist），自建链与 rc 重组另由 REQ-01M411CSDXDVGTP83BPS6TNV48 track

## 范围
旧仓 `res/` 根下的 Windows 资源件 —— 它们都是**从 `AGERC.DLL` 导出、本身没有任何修改**的：
`AGERC.DLL.rc` / `AGERC_RAW.DLL.rc` · `MANIFEST2_1.txt` · `build-localized-agerc.ps1` ·
`inject-localized-agerc.rsh`（另有 `CURSOR*.cur` / `IDI_ICON1.ico` 同类：已由 M1b 入 `corpus/assets/ui-images/`）。

## 口径
**本轮只登记，不搬入**（M1b 有意不迁 `res/` 的 rc 系列）；参考件在旧仓只读位置，
三条 `AGERC.DLL` 二进制登记见 `corpus/assets.json` 的 `binary/agerc-*`。

## 评估与结论的**真源不在这里**
`docs/01-translation/agerc-design.md` 收着全部实测与六个方案评估。它给出的、会改变设计的结论（索引）：
① 成品是**两条链叠加**（Resource Hacker 重建 DIALOG/MENU + `scripts/patch-menu.js` 按硬编码偏移原位改写）；
② 判据数是三个，别混：**46** 处文字 / **38** 个命令 ID / **16** 个 DIALOG 模板；
③ 整份改动 **< 2.5 KB**，对玩家可见的只有主菜单栏 + 一个退出确认框；
④ 对话框中文用**占位码位** + FONT 改 `Amayui CN` ⇒ 换字典会**静默**渲染错（DLL 侧没有 `subsSha` 那样的指纹）；
⑤ 引擎按**名字** `LoadLibraryA("AGERC.DLL")` ⇒ 基线必须取 **ALF 内那份**（根目录那份是加壳件），产物写回根目录；
⑥ 效果投射靠宿主对象 `AGE:reg`（59 个字符串键，可归一化）与 `AGE:IAGEService`（~60 槽，逐槽签名待定）；
⑦ 旧链（链① 的基线路径 + 链② 的偏移表）在本机**已跑不动** ⇒ 只读前提下不可复现。

## 判据
（本节点已收口，不再有判据。）原判据是"重新设计后的组织落进新仓 + 一条命令从基线打出产物 + 判据全绿"，
它随 §裁决 一起转给了 `REQ-01M411CSDXDVGTP83BPS6TNV48`（二进制重建）与 `release/README.md` §5.2（本版口径）。

## 裁决（2026-10，用户）：**本节点关掉**
本节点的实质 = `AGERC.DLL` 的修改逻辑；本版已裁决**直接使用入库的可信产物**
（`corpus/assets/agerc/AGERC.DLL`，条目 `binary/agerc-dist`，"可信"由 `tools/test/agerc-artifact.test.mjs` 机械复核）；
"怎么把那份二进制造出来 / rc 系列怎么重新组织"由 **`REQ-01M411CSDXDVGTP83BPS6TNV48`** 单独 track
⇒ 本节点不再作为落点（`dropped`，理由指向那条）。
