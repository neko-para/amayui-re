# tools/ — 跨域 CLI（**从零写，不迁旧脚本**）

> 语言口径：本目录**一律 `.mjs`**（无构建步骤、无 `tsconfig`）—— 只有 `apps/emulator` 用 TypeScript。
> 旧仓的工具（台账生成器 / 校验器 / `.tmp/` 里那些一次性脚本）**一律按"重建"处理**，只登记不迁移。

## 1. 现有工具

| 工具 | 作用 |
|---|---|
| `corpus.mjs` | ★ **`corpus/assets.json` 的守卫 + 唯一写入口**（§3.3 的 9 条断言） |
| `disasm-recode.mjs` | 反汇编语料的 **CP932→UTF-8 + CRLF→LF 无损转码**与保真断言（§1.8.1） |
| `old-repo-inventory.mjs` | **重新实测旧仓** → `docs/00-origin/old-repo-inventory.md`（只读旧仓） |
| `test/corpus-manifest.test.mjs` | 守卫的单元测试 + 端到端测试（`node --test`） |

## 2. 怎么跑

```bash
pnpm validate                          # = node tools/corpus.mjs --validate
pnpm test                              # = node --test "tools/test/**/*.test.mjs"
pnpm corpus -- --list                  # 条目一览
pnpm corpus -- --scan --write          # 补 origin[].sha256（唯一写入口；缺省 dry-run）
pnpm corpus -- --set <id> '<patch-json>' --write   # 改条目（写后回读复验，不绿回滚）
pnpm inventory                         # 重测旧仓 → docs/00-origin/old-repo-inventory.md
pnpm recode -- --verify                # 语料转码断言（不落盘）
pnpm recode -- --build                 # 转码落盘 + 打确定性 zip（M1 用）
```

## 3. `corpus.mjs` 的 9 条断言（§3.3）

| # | 断言 | 备注 |
|---|---|---|
| 1 | schema：必填齐全、枚举合法、`id` 全局唯一 | |
| 2 | 自洽：`external-only`/`deferred` ⇔ `dest === null`；`lfs`/`git` ⇔ `dest` 非空 | |
| 3 | 存在性：`dest` 在盘上；`origin[].path` 按 `roots[root]` 解析后必须存在 | ★ `root=staging` 例外：可缺失，只报 warning（中转区，不是长期位置） |
| 4 | 校验和纪律：**入库件不得写 `origin[].sha256`**；**不入库的文件件必须写且与盘上一致**；目录型 origin 不写 | "没被改过"的唯一证据 |
| 5 | 忽略一致性：`external-only` 且 `dest` 在仓内 ⇒ 必须被 `.gitignore` 命中 | |
| 6 | LFS 一致性：`storage=lfs` ⇒ `git check-attr filter -- <dest>` 必须是 `lfs` | |
| 7 | 语料保真：**入库的** `disasm-corpus` ⇒ `recipe` 必填且**真跑一次断言** | ★ 只对"真正入库的那一份"强制（用户拍板）：转码前的原件与"明确不带的清单"豁免 |
| 8 | 知识准入门：`kind=knowledge-source` ⇒ `storage` 只能是 `external-only`/`deferred` | K3 通过前不得入库 |
| 9 | 真前身可解析：`derivedFrom[].ref` 必须是存在的 `id`；**入库的** `disasm-corpus` 必须有一条指向 `kind=binary` | ★ 同 #7 的作用域 |

## 4. 为什么要有这些工具（而不是手写脚本）

* 旧仓 `.tmp/` 里有 **724 个 `.mjs` + 431 个 `.py`** 一次性脚本 —— 那是"没有工具层"的代价：
  每次都临时写、写完就废、结论无处沉淀。
* 因此新仓的工具是**长期资产**：有 `--help`、有退出码、有测试、有确定的输出格式；
  **不写进 `tools/` 的脚本，就不该被反复用第二次**。
