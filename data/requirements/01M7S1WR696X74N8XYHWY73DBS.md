# 发行打包 · overlay 外置脚本随包（hud.js + kit.js + ui.d.ts + README.md）

- id: REQ-01M7S1WR696X74N8XYHWY73DBS
- type: req
- status: open
- parent: REQ-01M7PE0D9MJA4REXQ1YJFVPHGF
- tags: [emulator-lift, overlay, release]

## 目标 / 用户面

发行树里的 `emulator-lift-aot` 加 `--overlay` 必须能启动：`<exe 目录>/scripts/ui/` 下同时有
`hud.js` 与 `kit.js`（`hud.js` 是 module，`import './kit.js'`），以及作者用的 `ui.d.ts` /
`README.md`。缺 `hud.js` **或** 缺 `kit.js` 都必须 `exit 2` 并点名路径（⛔ 不回退空 HUD）。

## 为什么现在开单

- `repo:docs/03-emulator/overlay.md` 的 R10 一直把这条记为「`release/` 待补」；切片 3 之后依赖
  从 1 个文件变成 **2 个必须同目录的文件**（`hud.js` + `kit.js`）。
- `repo:tools/release.mjs` 目前只产翻译线发行包，没有任何「把 `apps/emulator-lift/scripts/ui/*`
  复制到发行树」的规则 ⇒ 发行树上的 `--overlay` 是**启动硬错**（响亮，但功能不可用）。

## 判据（实施时写 verify / 手动验收）

- 发行产物树含 `scripts/ui/hud.js`、`scripts/ui/kit.js`、`scripts/ui/ui.d.ts`、
  `scripts/ui/README.md`（路径相对 exe 目录）。
- 从发行树跑 `emulator-lift-aot --present window --overlay`（最小 run）⇒ 不进「HUD 脚本不在」
  分支；把 `kit.js` 临时改名 ⇒ `exit 2` 且错误串点名 `kit.js`。
- 打包规则是**生成物**（脚本 + 文件头写明「别手改」），⛔ 不把脚本副本入库。
- 不新增第二份清单：文件集只从 `repo:apps/emulator-lift/scripts/ui/` 的**目录本身**派生
  （⛔ 不在 release 脚本里手抄 4 个文件名而脱离目录）。

## 边界

- ⛔ 不改 `hud.js` / `kit.js` 的行为；⛔ 不改 `--overlay` 的加载语义（缺件仍然是硬错）。
- ⛔ 不把 `scripts/ui/*` 编进 exe（运行时读外置文件是既有口径）。
- `ui.d.ts` / `README.md` 只随包分发，不参与运行时加载。
