# 批 M1b · 纯资源第二批（脱壳件 / 节表修补件 / UI 图片 / 字体 / 补丁产物）

- id: REQ-01M3TCM5R0MYPJXZFJYXTKS1JX
- type: req
- status: done
- parent: REQ-01M3TC9660ZG1YMB7KAKE4W94B
- order: 30
- verify: tools/test/corpus-assets.test.mjs#① 7z 的**解压产物**不得入库：.gitignore 逐目录命中，且 7z 本身不被忽略

## 判据（已完成的部分）
1. **UI 图片**：10 张汉化完成图的**生效版**（版本号最高者）+ 7 张未汉化图的 `-0` 原图 + 8 个 E 系列渲染
   参考块 + `versions.json`（版本表）落 `corpus/assets/ui-images/`；中间版本与烘焙后的 AGF **不入库**
   （AGF 只登记 sha256，作日后重建链的复现参照）。
2. **字体**：`Amayui-CN_cnjp{,-Bold}.ttf` + `SarasaGothic{SC,J}-TTF-1.0.40.7z` 落 `corpus/assets/fonts/`；
   **7z 解压产物不入库**（.gitignore 命中 + 测试钉住）；WenQuanYi 系列有意排除。
3. **字典**：`subs_cn_jp.json` → `data/translations/subs-cn-jp.json`（纯文本，走 git）。
4. **二进制**：`age-original` / `age-sectfix` / 三份 `AGERC.DLL` —— 用户口径**不迁移**，
   条目由 `deferred` 翻成 `external-only` + `archived`（逐件 sha256 只对不入库件写）。
   ★ AGERC 的**载荷**已随 `assets/fonts-dist` 入仓（`AGERC.DLL` 与其中间产物都在 SC 7z 里）。

## 非目标
* **补丁产物（标题里那项）：旧仓 `patch/` 整个目录有意跳过** —— `AGF/`(10) `BIN/`(453) `AGERC.DLL`
  `CHANGELOG.md` `patch.config.json` `README-测试版说明.md` **既不入仓、也不在新仓台账登记**
  （`translation/patch` 只说明"为何弃置"）。理由：产物是容器层输出（M2 重建）、打包清单待
  「翻译环境重建」重设计。
  ★ **其中一项已改判（2026-10-03，用户口径，原结论保留可见）**：`CHANGELOG.md` 与
  `README-测试版说明.md` 是**随补丁包发给玩家的产品文本**，不是内部变更记录 ⇒ **必须保留**
  （不能与"内部沿革只有 `git log`"混为一谈）。原理由的后半句"CHANGELOG 是手写状态而
  `-N` 序列 + 逐文件 git log 已足够"**作废**；**产物**（`BIN/` `AGF/` `AGERC.DLL`）与
  `patch.config.json` 的跳过**不变**（后者可由 `patch.json` 的键派生 ⇒ 存它就是第二真源）。
  落点与规格见 `release/README.md`；工作项 = `REQ-01M40RP2BN9S86K6SGEW671PYA`。
* `res/` 根下的 rc 系列（`AGERC.DLL.rc` / `MANIFEST2_1.txt` / `build-localized-agerc.ps1` /
  `inject-localized-agerc.rsh`）：走需求树 **「翻译环境重建」→「windows 资源重建」**。
* 中间版本 PNG、WenQuanYi 字体、`raw-parts/*.idb`（63 MB，永不入库）、`raw-parts/*.AGF`。

## 凭据
* `pnpm tools corpus validate`：9 条断言全绿（含 #4 目录型 dest 逐字节比对、#6 已跟踪载荷的 LFS 属性、
  #4 不入库件的 sha256 与盘上一致）。
* `pnpm run test`（`tools/test/corpus-assets.test.mjs`）：7z 解压产物不入库、载荷按扩展名走 LFS。
* 反查：清单里剩下的 `deferred` 条目全是 `tooling/rebuild`（M2/M4/M5/M6）与 `agent-infra`（M6），
  以及明确不带的 `disasm/excluded` —— 没有仍待处理的纯资源。
