# 旧仓盘点（实测快照）

> 本文件由 `pnpm tools old-repo inventory`（`tools/old-repo-inventory.mjs`）**重新实测生成** —— **不要手改，改脚本**。
> 快照里**不写生成时间**（那只会让每次扫描都产生 diff）；只写实测数字与旧仓 HEAD。
> 旧仓全程**只读**：本脚本只跑只读 git 命令与 fs 读取。

* 旧仓：`E:\Games\Eushully\天結`
* HEAD：`e90722fed832983b45b9299ec21143d8abf295a9`（2026-09-30T02:29:53+08:00）
* HEAD 提交：chore: archive preparing
* 提交数：556　|　跟踪文件：3678　|　pack 体积：170.8 MiB
* 根 `package.json`：**不存在**（⇒ 旧仓没有 workspace）
* `.gitattributes`：LFS 规则 12 条，`text`/`eol` 规则 0 条
* LFS 跟踪文件：149

## 1. 顶层目录实测

| 目录 / 文件 | 类型 | 跟踪 | 全部文件 | 体积 | gitignore | 备注 |
|---|---|---:|---:|---:|---|---|
| `raw-parts` | DIR | 0 | 27639 | 8.1 GiB | 是 |  |
| `install` | DIR | 0 | 548 | 7.5 GiB | 是 |  |
| `.tmp` | DIR | 0 | 16885 | 3.7 GiB | 是 |  |
| `app` | DIR | 596 | 47300 | 793.9 MiB | 否 |  |
| `res` | DIR | 78 | 100 | 772.4 MiB | 否 |  |
| `tools` | DIR | 5 | 3946 | 492.1 MiB | 否 |  |
| `patch` | DIR | 6 | 469 | 128.4 MiB | 否 |  |
| `engine` | DIR | 11 | 11 | 118.2 MiB | 否 |  |
| `tickets` | DIR | 735 | 735 | 103.7 MiB | 否 |  |
| `src` | DIR | 941 | 941 | 91.8 MiB | 否 |  |
| `data` | DIR | 941 | 941 | 82.9 MiB | 否 |  |
| `native` | DIR | 15 | 864 | 6.9 MiB | 否 |  |
| `scripts` | DIR | 115 | 184 | 6.1 MiB | 否 |  |
| `docs-new` | DIR | 123 | 123 | 6.1 MiB | 否 |  |
| `plugins` | DIR | 47 | 648 | 5.1 MiB | 否 |  |
| `cache` | DIR | 7 | 7 | 3.4 MiB | 否 |  |
| `output` | DIR | 10 | 10 | 3.2 MiB | 否 |  |
| `analysis` | DIR | 7 | 7 | 2.5 MiB | 否 |  |
| `.agents` | DIR | 32 | 32 | 382.9 KiB | 否 |  |
| `.github` | DIR | 1 | 1 | 1.9 KiB | 否 |  |
| `.vscode` | DIR | 1 | 1 | 227 B | 否 |  |
| `v1.13-260901.zip` | FILE | 0 | 1 | 105.6 MiB | 是 |  |
| `v1.12-260825.zip` | FILE | 0 | 1 | 105.2 MiB | 是 |  |
| `v1.11-260824.zip` | FILE | 0 | 1 | 105.2 MiB | 是 |  |
| `v1.10-260822.zip` | FILE | 0 | 1 | 100.8 MiB | 是 |  |
| `v1.10.1-260822.zip` | FILE | 0 | 1 | 100.8 MiB | 是 |  |
| `v1.9-260821.zip` | FILE | 0 | 1 | 79.4 MiB | 是 |  |
| `v1.8-260820.zip` | FILE | 0 | 1 | 79.4 MiB | 是 |  |
| `v1.7-260819.zip` | FILE | 0 | 1 | 79.4 MiB | 是 |  |
| `v1.6-260817.zip` | FILE | 0 | 1 | 79.4 MiB | 是 |  |
| `v1.5-260816.zip` | FILE | 0 | 1 | 79.4 MiB | 是 |  |
| `v1.4-260814.zip` | FILE | 0 | 1 | 79.4 MiB | 是 |  |
| `v1.3-260814.zip` | FILE | 0 | 1 | 79.4 MiB | 是 |  |
| `v1.2-260814.zip` | FILE | 0 | 1 | 76.0 MiB | 是 |  |
| `v1.1-260813.zip` | FILE | 0 | 1 | 76.0 MiB | 是 |  |
| `v1.0-260813.zip` | FILE | 0 | 1 | 76.0 MiB | 是 |  |
| `install-manifest.json` | FILE | 1 | 1 | 29.6 KiB | 否 |  |
| `DESIGN.md` | FILE | 1 | 1 | 27.3 KiB | 否 |  |
| `AGENTS.md` | FILE | 1 | 1 | 10.7 KiB | 否 |  |
| `raw-manifest.json` | FILE | 1 | 1 | 8.3 KiB | 否 |  |
| `.gitignore` | FILE | 1 | 1 | 2.5 KiB | 否 |  |
| `emulator.config.json` | FILE | 0 | 1 | 1.3 KiB | 是 |  |
| `emulator.config.example.json` | FILE | 1 | 1 | 1.1 KiB | 否 |  |
| `.gitattributes` | FILE | 1 | 1 | 516 B | 否 |  |
| `raw` | LINK | 0 | 1023 | 7.7 GiB | 是 | 符号链接 → `E:\Games\Eushully\天結いキャッスルマイスター` |

## 2. 行尾实测（跟踪的文本文件）

| 项 | 数量 |
|---|---:|
| 已扫描 | 3508 |
| 纯 LF | 3444 |
| 纯 CRLF | 55 |
| **混合行尾** | **8** |
| 无行尾符 | 1 |
| 跳过（二进制 / 含 NUL） | 156 |
| 跳过（> 4.0 MiB） | 2 |

混合行尾样本（最多 20 个）：

* `app/amayui-emulator/test/save-slot-tdz.test.ts`
* `docs-new/99-records/2026-09-impl-audit/raw/summary.md`
* `tickets/T-0102/notes.md`
* `tickets/T-0182/evidence/curtain-drop-hold-after.log`
* `tickets/T-0182/evidence/curtain-drop-no-hold.log`
* `tickets/T-0182/evidence/transition-frames-before.md`
* `tickets/T-0188/evidence/op1b-load-from-title-79-calibrated.log`
* `tickets/T-0189/evidence/e2e-load-from-adv.log`

## 3. 大小写冲突实测（win/mac 不敏感、Linux CI 敏感）

**0 组冲突**（跟踪路径里没有仅大小写不同的重名）。

## 4. 跨域引用实测（说明"为什么不能简单按目录切"）

| 从 | 命中 token | 命中文件 | 命中行 | 扫描文件 |
|---|---|---:|---:|---:|
| `tickets` | `amayui-emulator` | 418 | 3722 | 735 |
| `app/amayui-emulator` | `tickets/T-` | 363 | 1599 | 503 |
| `docs-new` | `amayui-emulator` | 79 | 827 | 123 |
| `plugins` | `tickets/` / `src/` | 26 | 133 | 47 |
| `.agents` | `tickets/` | 9 | 62 | 32 |
| `scripts` | `analysis/` | 12 | 51 | 115 |

## 5. 供迁移轮注意的实测要点

* `.agents/skills/` 下**实测 8 个技能**（每个一个 `SKILL.md`）：`amayui-engine-analysis`、`amayui-mnemonic-rename`、`amayui-script-analysis`、`amayui-script-translate`、`amayui-script-update`、`amayui-ticket-ledger`、`amayui-ui-text-render`、`batch-task-runner`
* 技能发现路径**固定** `.agents/skills/<名字>/SKILL.md`（DSH 硬要求）⇒ 新仓该目录必须在，内容从零重写。
* 大件（`install/` `raw/` `raw-parts/` `.tmp/` `tools/`）**不入库**：用符号链接 / 路径登记引用。
* `raw/` 是**符号链接**（指向外部游戏安装目录）：可用，只要不 track。

