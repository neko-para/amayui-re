# 发布链的 AGF 改用入库件（v1.14 发布包固化进仓），默认不重新烘焙

- id: REQ-01M4GE06NFB98BPVESG144XNXQ
- type: req
- status: done
- parent: REQ-01M3TCCVC01AWFEMP6GJM96KQ4
- verify: tools/test/release-agf-source.test.mjs#★ 缺省 AGF 目录 = 入库件（不是就地烧出来的那份）
- done_reason: 已落地：corpus/assets/ui-agf/ 10 张入库（清单 assets/ui-agf-dist，来源 = v1.14 发布包 AGF/，材料化到 .staging 由 corpus validate 逐字节比对）；release 缺省 DEFAULT_AGF_DIR=入库件、--baked 显式覆盖、两个集合双向对账；新增守卫 release-agf-source.test.mjs（4 条）并已随 pnpm test 绿。效果：release pack 不再需要 Chrome，实测 AGF 10 张全部来自入库件。
- tags: [release, ui-bake, assets]

## 这条要做什么

把**发布链的 AGF 来源**从「每次重烧」改成「**用入库件**」，并把 v1.14 发布包里的那 10 张固化进仓库。

## 为什么（用户口径 + 实测）

1. `pnpm tools ui-bake build` 要 **headless Chrome**（`tools/ui-bake.md` §5.1）⇒ 默认路径绑浏览器 =
   **没有浏览器就发不出包**（实测：在 macOS 沙箱里 `ui-bake build` 直接挂在渲染步）。
2. 同一个配方在不同 Chrome / 字体环境下烧出来的**字节不同**，而「发出去的那一版」必须固定
   ⇒ 发布链要读**固化下来的字节**，不是"这次烧出来的"。
3. 顺带解决一个隐蔽的一致性缺口：`release pack` 原先按**配方集合**取件、字节取自 `dist/ui-bake/`，
   于是"填上配方但没烧"的块会以**缺件**形式暴露，而"入库件与配方不一致"没有任何判据。

## 收口判据

* `corpus/assets/ui-agf/` 10 张（= v1.14 发布包 `AGF/`）入库，清单条目 `assets/ui-agf-dist`，
  来源材料化到 `.staging/release-v1.14-261005/`（`corpus validate` 逐字节比对「入库副本 == 来源」）；
* `release` 缺省从入库件取字节（`DEFAULT_AGF_DIR`），CLI 用 `--baked` 显式覆盖；
  集合与配方**双向对账**（配方有/入库件没有 ⇒ problem；反之 ⇒ warning）；
* 守卫：`tools/test/release-agf-source.test.mjs`（缺省目录是入库件 · 字节来自 agfDir · 两个集合都对账 · sha256 逐件算）。

## 有意不做

* **不**把「入库件 == 配方渲染结果」做成默认门禁：那要 Chrome。核对仍按需跑 `pnpm tools ui-bake verify`，
  已知残差（`SO001` 重放偏差）在 `REQ-01M42S9QSMTPCYTHDEDCHWX3R5`。
* **不**自动更新入库件：把新烧出来的那份固化进仓 = 人决定"这次就发它"。
