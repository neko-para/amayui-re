# 批 M4 · 模拟器与原生输入（按新结构重写）

- id: REQ-01M3TCSNH00RVXCPZGM1J6EXRC
- type: req
- status: open
- parent: REQ-01M3TC9660ZG1YMB7KAKE4W94B
- order: 60

## 范围
`apps/emulator` + `packages/host-input`：按新结构重写，把旧仓寄生在测试下的 10 个跨域守卫拆回各域。

## 子节点分工（★ 判据不同，别混）
| | 含什么 | 是否知识线前置 |
|---|---|---|
| **M4-1 · 无头核心** `REQ-01M47SGXYWPQ1KWET2J49TJQST` | `src/{vm,script,save,text,audio,frame,host,util}` + fixture 驱动测试设施 + 拆守卫 | ★ **是**（K2 的硬前置） |
| **M4-2 · 表现层与窗口** `REQ-01M47SH17F08E4B2VTBVTKA4YF` | `src/renderer`（Pixi）· `electron/`（窗口壳）· `src/web` · `packages/host-input` · `plugins/amayui-emulator` | 否 |

## 判据
（待写：无头实例可跑；跨域守卫各自落在本域。）

## 沿革（★ 为什么拆开）
原先只有本节点、且前置写着 "M2 / M3"，于是"K2 必须等 M4"看起来等于"K2 必须等整个模拟器"。
实测推翻了这个粗粒度前置：79 条 `modeled-verified` 的守卫文件**全部落在 T0/T1 档**（T2 全仓只有
`e4-gamestart-shot.test.ts` 一个文件，且不进 `all`/`verify`，见旧仓 `test/run.ts`）⇒
复核只需**无头核心 + fixture 驱动**，不需要窗口、原生输入、真游戏安装。
另：本节点**不依赖 M3**（模拟器只用 `packages/age-format`；旧仓 `package.json` 的 dependencies 只有 `pixi.js`，
代码里零引用 `age-format` / 已删除的 `script-dsl`）。
