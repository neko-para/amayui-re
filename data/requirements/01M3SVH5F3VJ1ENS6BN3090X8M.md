# windows 资源重建（res 的 rc 系列重新设计组织）

- id: REQ-01M3SVH5F3VJ1ENS6BN3090X8M
- type: req
- status: open
- parent: REQ-01M3SVH2V332E4CJYN0BHH0YK8
- order: 25

## 范围
旧仓 `res/` 根下的 Windows 资源件 —— 它们都是**从 `AGERC.DLL` 导出、本身没有任何修改**的：
`AGERC.DLL.rc` / `AGERC_RAW.DLL.rc` · `MANIFEST2_1.txt` · `build-localized-agerc.ps1` ·
`inject-localized-agerc.rsh`（另有 `CURSOR*.cur` / `IDI_ICON1.ico` 同类：已由 M1b 入 `corpus/assets/ui-images/`）。

## 口径
**本轮只登记，不搬入**。后续按本需求单**重新设计这些文件的组织方式**（落点、构建链、与 `AGERC.DLL`
三条二进制的关系一起定），因此 **M1b 有意不迁 `res/` 的 rc 系列**。

## 出发点（参考件在旧仓只读位置）
`oldRepo:res/AGERC.DLL.rc` · `AGERC_RAW.DLL.rc` · `MANIFEST2_1.txt` · `build-localized-agerc.ps1` ·
`inject-localized-agerc.rsh`；三条 `AGERC.DLL` 二进制登记见 `corpus/assets.json` 的 `binary/agerc-*`。

## 判据
（待写：「重新设计后的组织」落进新仓 + 构建/注入链有条命令可复现 + 旧仓只读边界不变。）
