# 素材清单的 origin 与机器/平台绑定：本机私有覆盖落地后暴露「缺件 / 本地构建产物 / 原版 vs 安装树」三类不一致

- id: REQ-01M4FEFA9RRP5TKVWH351NV1RD
- type: req
- status: open
- parent: REQ-01M3TC9660ZG1YMB7KAKE4W94B
- tags: [corpus, manifest, platform]

## 是什么

本机按用户口径落了**本机私有清单覆盖** `corpus/assets.local.json`（`.gitignore` 命中 `*.local.json`，不入库）：

```json
{ "roots": { "oldRepo": "/Users/nekosu/Documents/Projects/amayui-cn",
             "gameInstall": "/Users/nekosu/Documents/Projects/amayui-cn/raw" } }
```

写入口：`pnpm tools corpus set-root <名> <路径> --local --write`；读它的只有唯一的清单加载器
`loadManifest()`（`corpus` / `fixtures` / `opcodes` / `ledger` / `patch` / `release` / `translate-ref` 共用）。
★ 它**只覆盖 `roots`**（多一个键就抛）：条目是仓库事实，不许本机改写。

## 落地之后暴露的三类不一致（`pnpm tools corpus validate` 由 1 红变 2 红）

| 类 | 实例 | 为什么这不是"改个 sha256 就好" |
|---|---|---|
| **缺件** | `binary/age-sectfix` → `oldRepo:raw-parts/AGE.EXE__dumped.sectfix.EXE`；`reference/design-discussion` → `oldRepo:DESIGN.md` | 旧仓的 `raw-parts/` 本身被旧仓 `.gitignore` 忽略（`/raw-parts`），**两台机器上都可能不在盘上** ⇒ 清单里的 `external-only` 素材并不总能解析 |
| **本地构建产物** | `binary/agerc-modified-install` → `oldRepo:install/AGERC.DLL`（清单记 `6241de…`，本机实际 `5f3189…`） | 旧仓 `.gitignore` 第 1 行就是 `/install` ⇒ 那**不是仓库内容而是本机构建产物**。给它记 `sha256` 等于把"某一台机器上跑出来的东西"当成可再校验的来源 ⇒ 永远机器相关 |
| **原版 vs 安装树** | `assets/samples-asm` 的 4 个 `gameInstall` 来源里 3 个不符（`$1$OFINIT.BIN` / `PLINIT.BIN` / `INFOFA.BIN`） | 清单的 `sha256` 是在 **Windows 的（改过的）安装树**上量的；本机 `amayui-cn/raw` 是**原版日文资源**。两者**本来就不是同一份内容** ⇒ 覆盖 `gameInstall` 必然报不符（这不是漂移，是**同一根名的两种语义**） |

## 要裁决/要做的

1. **`roots.gameInstall` 的语义写清楚**：它指的是"**哪一份**安装"（原版 / 汉化后 / 打补丁后）？
   若是"记录时那份"，那本机的原版资源就不该复用同一个根名 —— 应新增一个根名（如 `gameInstallJa`）
   或让 `assets/samples-asm` 的 `storage` 明确为 `external-only`+说明。
2. **`external-only` 且来源被上游 `.gitignore` 的条目**（`install/`、`raw-parts/`）：
   要么改成 `deferred` 并在 `consume` 里写清"永久不可再校验"，要么**去掉 sha256**（它给不出保证）。
3. **`raw-parts` 缺件**：确认是"这台机器没同步"还是"旧仓已把它删掉"；前者允许 warning 延续，
   后者应把 `binary/age-sectfix` 标成 `deferred` 并把台账里指向它的 `reference` 锚的证据等级降下来。
4. 顺带：（另单）台账的 `reference` 锚语义已按"取不到 ⇒ warning（不可校验 ≠ 失效）"落地 ——
   本条不重复它，只处理**清单**这一侧。

## 收口判据

`pnpm tools corpus validate` 在**两台机器上**都能给出"可解释的红/绿"：
本机允许"参考仓/安装树不在场"的 warning，但**不允许**出现"sha256 与盘上不符"这类**机器相关**的红。
