# `binary/agerc-modified-install` 的 sha256 pin 指向旧仓里的旧产物（5f3189…），与 v1.14 发布包/入库件的 6241de… 不符

- id: REQ-01M4GE4HSJGZ4XF9BSTZFHKXKG
- type: bug
- status: dropped
- parent: REQ-01M4FEF7VH1N0JDT5ZVPZS204M
- repro: corpus/assets.json#binary/agerc-modified-install
- dropped_reason: 不是缺陷/不修：本机四份 AGERC 逐个实测都与清单一致（旧仓 install/ 与 patch/ = 6241de666d2f533b…、入库件 corpus/assets/agerc/AGERC.DLL 同、corpus validate #4 全绿），而 5f318955f605… 的真身是**另一条** binary/agerc-packed（root=gameInstall）的带壳原版；git show 16fbd0a:corpus/assets.json 显示该条自 init 起就是 install/AGERC.DLL + 6241de…，**从没记过 5f3189** ⇒ 不是 pin 指错，而是**跨机器 root 解析差异**（原始报告在 macOS，那里 oldRepo/gameInstall 解析到另一棵树）；真问题由 REQ-01M4FEFA9RRP5TKVWH351NV1RD（origin 与机器/平台绑定）跟踪。★ 边界：mac 侧**无法在本机验证**（本机是 win32，没有那两条 mac 绝对路径），"mac 上 install/AGERC.DLL 是 5f3189"只是推断而非直接观察；重开条件 = corpus validate #4 对该条目再报红。
- tags: [corpus, release, assets]

# `binary/agerc-modified-install` 的 sha256 pin 指向旧仓里的旧产物（5f3189…），与 v1.14 发布包/入库件的 6241de… 不符

- id: REQ-01M4GE4HSJGZ4XF9BSTZFHKXKG
- type: bug
- status: dropped
- parent: REQ-01M4FEF7VH1N0JDT5ZVPZS204M
- repro: corpus/assets.json#binary/agerc-modified-install
- tags: [corpus, release, assets]

## 现场（原始报告，2026-10）

`pnpm tools corpus validate` 的 #4（校验和纪律）报红：

```
binary/agerc-modified-install: sha256 与盘上不符
  清单 6241de666d2f… / 实际 5f318955f605…  → install/AGERC.DLL
```

| 件 | sha256（前 12） | 说明 |
|---|---|---|
| **v1.14 发布包**（`/Users/nekosu/Downloads/v1.14-261005/AGERC.DLL`） | `6241de666d2f` | 最新一次发出去的那份 |
| 本仓入库件 `corpus/assets/agerc/AGERC.DLL`（条目 `binary/agerc-dist`） | `6241de666d2f` | **与发布包逐字节相同** ✓ |
| 旧仓 `install/AGERC.DLL`（条目 `binary/agerc-modified-install` 的 pin） | `5f318955f605` | **更旧的一次构建** |

## ★ 裁决：**不是缺陷**（`dropped`）—— 本机四条实测 + init 提交事实都指向"跨机器 root 解析差异"

**① 本机四份逐个实测 sha256（2026-10，本机 win32）**：

| 件 | 实测 | 与清单那一格的关系 |
|---|---|---|
| 旧仓 `E:\Games\Eushully\天結\install\AGERC.DLL` | `6241de666d2f533b…` | = 该条的 `origin[].sha256` ⇒ **一致** |
| 旧仓 `E:\Games\Eushully\天結\patch\AGERC.DLL` | `6241de666d2f533b…` | 同上 |
| 入库件 `corpus/assets/agerc/AGERC.DLL` | `6241de666d2f533b…` | = `binary/agerc-dist` 的基准 ✓ |
| 游戏安装件 `…\天結いキャッスルマイスター\AGERC.DLL`（带壳原版） | `5f318955f605…` | 属**另一条** `binary/agerc-packed`（root = **gameInstall**，清单 `:440` 记的也是 `5f3189`）⇒ 同样一致 |

`pnpm tools corpus validate` = **9 断言：8 通过 / 0 失败 / 1 警告**（`#4 校验和纪律 → 全部合规`）。

**② init 提交事实（可复跑）**：`git show 16fbd0a:corpus/assets.json` ⇒ `binary/agerc-modified-install` 的
`origin` **自 init 起**就是 `{ root: oldRepo, path: install/AGERC.DLL, sha256: 6241de666d2f… }` ——
**它从来没有记过 `5f3189`**。⇒ 报错里那行"清单 6241de / 实际 5f3189 → install/AGERC.DLL"的
**清单侧读数没错，错的是"实际"那一侧的 root 解析**。

**③ 所以原判"pin 指错"不成立**：
* 不是 pin 错、不是入库件坏（入库件与发布包逐字节相同）；
* `5f3189` 是**另一条条目**（`binary/agerc-packed`，root = `gameInstall`）的正当读数；
* **真正的问题是 `oldRepo` / `gameInstall` 这两个 root 在不同机器上解析到不同的树** —— 原始报告跑在
  **macOS**（`corpus/assets.local.json` 的覆盖里 `oldRepo` 指 `/Users/nekosu/Documents/Projects/amayui-cn`、
  `gameInstall` 指它的 `raw/`），在那里 `install/AGERC.DLL` 是**另一份文件**。
  ⇒ 这是**跨机器 root 解析差异**，不是这条 pin 的缺陷。

**④ 真问题由谁跟踪**：`REQ-01M4FEFA9RRP5TKVWH351NV1RD`（「素材清单的 origin 与机器/平台绑定：
本机私有覆盖落地后暴露『缺件 / 本地构建产物 / 原版 vs 安装树』三类不一致」）。

**⑤ 边界（★ 必须写明）**：**mac 侧无法在本机验证** —— 本机是 win32，没有那两条 mac 绝对路径；
"mac 上的 `install/AGERC.DLL` 是 `5f3189`"这件事**本轮没有直接观察**，只有"原始报告的输出 + 两条 root
在那个覆盖下解析到同一棵树"这个推断。若将来在 mac 上复跑 `corpus validate` 再报同一条红，
**重开条件** = `pnpm tools corpus validate` 的 #4 对 `binary/agerc-modified-install` 报红。

★ 顺带确认（原报告）：字体两份也与 v1.14 发布包**逐字节相同**。
