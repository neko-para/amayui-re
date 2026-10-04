# 翻译环境重建（字体 / 渲染 / 打包链按新结构重写）

- id: REQ-01M3SVH2V332E4CJYN0BHH0YK8
- type: req
- status: doing
- parent: REQ-01M3TC9660ZG1YMB7KAKE4W94B
- order: 35

## 范围
旧仓翻译工具链（`scripts/` 里的翻译管线 + `res/` 的资源构建）**按新结构重写**，不逐文件搬运。
本节点是它在本树里的落点；归属 `REQ-01M3TC9660ZG1YMB7KAKE4W94B`，与 M 批**平级**（它横跨 M2 / M4 / M5：
既是 M5 翻译域的前置，又依赖 M2 容器层与 M4 模拟器）。

## 非目标
不搬旧仓 `scripts/`（登记为 `tooling/translation-pipeline` + `tooling/re`，`role: rebuild`）；
**不在本节点写任何翻译结论**（那属知识层，过 K3 准入才进台账）。

## 已入位的资源（M1b 的成果，本节点直接消费）
`corpus/assets/fonts/`（分发字体 + Sarasa 上游 7z）·
`corpus/assets/ui-images/`（生效版 PNG + `versions.json`）·
`data/translations/subs-cn-jp.json`（简→日写法占位字典）。

## 要重建的能力
1. **字体构建**：Sarasa SC 基底 + cnjp cmap 替换 + 族名 `Amayui CN` + 声明 Shift-JIS 932 码页；
2. **文本渲染**：UI 截图链（headless + @font-face 引本地字体）与效果规范；
3. **打包**（★ **已落点**）：`pnpm tools release` 的 `install` / `pack` 两条命令 ——
   变更集**算出来**（`patch.json` 的键 + ui-bake 的配方集 + 入库的 AGERC / 字体），
   `patch.config.json` 式同步清单**不再入库**；两份清单（`dist/install-manifest.json` 与 `<版本>.manifest.json`）
   都是**生成物**。落点 / 禁令 / 踩过的坑见 `tools/release.md`，形状的真源见 `pnpm tools release describe`；
4. **翻译数据模型**（已定案）= **patch 叠加层**：入库的只有 patch，`data`/`src` 都是实时视图 ——
   实施在子节点 `REQ-01M3XJXVYBRFW1VT7RD8SNRXKV`（设计见 `docs/01-translation/patch-design.md`）。

## 测试树跑起来的环境口径（★ 实测踩过，症状会伪装成"工具坏了"）
* **当前目录**：引擎按当前目录找自己的件 ⇒ 树里放一份 `启动游戏-LE.cmd`（`install --le-cmd` 生成，`cd /d "%~dp0"`），
  从 shell 菜单直接起 AGE.EXE 时当前目录未必是游戏目录；
* **完整性标签**：受限沙箱工作区里建出的树带 `Mandatory Label\Low`，而**从带 Low 标签的 EXE 起的进程就是 Low 完整性**
  ⇒ Locale Emulator 起不来（现象：**进程起来了、窗口没建出来**；直接双击却能看到窗口只是没转码）。
  处置 `--relabel-medium` 或建到工作区外。两条的成因与命令见 `tools/release.md` §3.1。

## 判据
（待写：每一件都要有可执行的守卫 —— 字体构建可复现、渲染参考块可对照、打包清单可校验、patch 可重建视图。）
* **打包**那一件的守卫已经在了：`tools/test/release.test.mjs`（+ `pnpm tools release plan install|pack`）；
* 其余三件（字体构建可复现 / 渲染参考块可对照 / patch 可重建视图）分别归各自的子节点。
