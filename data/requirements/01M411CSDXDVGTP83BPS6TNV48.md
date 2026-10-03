# AGERC.DLL 二进制重建（把可信产物换成自建链）

- id: REQ-01M411CSDXDVGTP83BPS6TNV48
- type: req
- status: open
- parent: REQ-01M3SVH2V332E4CJYN0BHH0YK8
- order: 60
- tags: [translation, release, agerc]

## 现状（2026-10-03 定案）
**先用「可信产物」顶着，二进制重建留到以后。** 理由（用户口径）：这个菜单的实际使用人少，
而且 16 个对话框**基本没汉化**（只 `DIALOG 3` 一个）⇒ 自建链的收益有限；
而重建要重写 **11 个对话框过程 + 整条菜单状态同步**（80 处 `CheckMenuItem`）、并**失去逐字节判据**。

* **可信产物**：`corpus/assets/agerc/AGERC.DLL`（= 旧仓 `patch/AGERC.DLL`，848,896 B，sha256 `6241de66…`）
  ⇒ 清单条目 `binary/agerc-dist`（`storage: lfs` · `readOnly` · `derivedFrom: binary/agerc-debug-unpacked`）。
* **「可信」是可机械复核的**：`tools/test/agerc-artifact.test.mjs` 拿清单里 `binary/agerc-modified-install`
  记的 sha256 当基准（**一处真源**），并在旧仓在机时再做逐字节比对。
  ★ 这条守卫本来就能抓到一次真事：有人为测「加壳 vs 不加壳」临时替换了旧仓 `install/AGERC.DLL`，
  当时是 `pnpm tools corpus validate` 报红发现的。
* **复现路径已记录**（两条链 + 各自输入 + `derivedFrom`），但**链本身没重建** —— 那就是本节点。

## 要重建什么（方向已评估，见 `docs/01-translation/agerc-design.md`）
1. 基线 = **`DATA1.ALF` 里那份脱壳原版**（`SYS4INI` id 21072，849,408 B）。
   ★ **不能**照 `patch baseline` 的「散装优先」去取根目录那份（加壳件；26 条资源的数据不在盘上）。
2. 入库真源 = 菜单文本 + `DIALOG/3` 模板 + 3 个代码内嵌串槽的新串（**存定位规则，不存偏移**）+ 2 个导入改名；
   **并带 `subsSha` 指纹**（对话框文本是占位码位，与字典/字体强耦合）。
3. 构建 = **纯 Node**（摆脱 Resource Hacker 这条 Windows GUI 依赖）。
   ★ 先做设计文档 §6 的 **E1**（`DIALOG/3` 的语言标记能否保持 `0x411` —— 若不必改，目录树完全不用动）
   与 **E2**（那几个串槽能否按内容定位而非硬编码偏移）。
4. 或者走更大的「**凭空重建 DLL**」那条路（设计文档 §G：**未否决**，但必须先做**探针版**
   把可达面测成运行时清单，再决定要重写多少）。

## 判据
* 自建产物过设计文档 **§4C 的五条判据**：资源集合不变 · 未列出的资源与基线逐字节相同 ·
  `.text` 必须 **0 差异** · 与旧产物**资源级**对账 · 带 `subsSha` 指纹。
* 与**可信产物**对账：`MENU/110` · `MENU/124` · `DIALOG/3` 三条资源的**内容**相同；
  ★ **不要求整文件逐字节** —— 旧链掺了 Resource Hacker 的重序列化产物（7 条没改过的 DIALOG 被 +2 字节）。
* **一条命令**从文本真源打出产物，干净目录里可复现；旧仓只读边界不变。
* 换成自建产物后，`tools/test/agerc-artifact.test.mjs` 的基准要**显式改判**（不是删掉它）。

## 非目标
* ❌ 不追求「把 16 个对话框全部汉化」—— 那是**内容**工作，归翻译域，不是本节点的重建工程；
* ❌ 不把带壳的根目录那份当可编辑素材（它的资源字节不在盘上）；
* ❌ 不在本节点动**菜单/对话框的文案**（只重建「怎么产出那份二进制」）。
