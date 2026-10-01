# 翻译环境重建（字体 / 渲染 / 打包链按新结构重写）

- id: REQ-01M3SVH2V332E4CJYN0BHH0YK8
- type: req
- status: open
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
2. **文本渲染**：UI 截图链（headless + @font-face 引本地字体）与效果规范（E1…E10 参考块已入位）；
3. **打包**：`patch.config.json` 式同步清单 + install-manifest 重建；旧仓 `patch/` 产物**不迁**。

## 判据
（待写：每一件都要有可执行的守卫 —— 字体构建可复现、渲染参考块可对照、打包清单可校验。）
