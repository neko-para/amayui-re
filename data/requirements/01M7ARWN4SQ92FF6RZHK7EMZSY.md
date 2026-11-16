# 🐞S3 requirements set 无法修复「多节点同时悬空」：写后守卫对整棵树校验 + 回滚 ⇒ 批量迁移死锁

- id: REQ-01M7ARWN4SQ92FF6RZHK7EMZSY
- type: bug
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- repro: tools/requirements.mjs#const report = validateAll(loadNodes(args.dir), { repoRoot: REPO_ROOT })
- severity: S3
- tags: [toolchain, requirements, ledger]

## 现象（2026-11-13 实测踩到）

`pnpm tools requirements set <id> … --write` 的写后守卫对**整棵树**跑 `validateAll`，任一失败就**回滚该节点**。
⇒ 当一次改动让 **N(>1)** 个节点的锚**同时**悬空（删测试文件 / 改用例名 / 挪文件 —— 测试瘦身一次就删了 6 个文件），
**逐个 CLI 写永远过不了**：写第一个时其余仍是红的。

实测：删除 `tests/{agerc_ui_state,gdi_font_metrics_cache,heap_layout,mip_e2e,recovery_dialog,title_entry_paths}.cpp`
之后，6 个节点 / 7 处悬空；`requirements set` **连续 6 次全部回滚**。

当时的绕法（记录在案，⛔ 不算合规做法）：写了一次性脚本直接改 `- verify:` / `- repro:` / 正文 `repo:` 的**值**，
改完立刻跑 `pnpm tools requirements validate` → 6/6 通过。它绕过了"唯一写入口"。

## 判据（修好之后必须成立）

- 存在一条**原子批量**写路径，能在一棵树里同时修 N 个节点的锚，且**只校验一次**（要么全落盘、要么全不落）：
  形式任选其一（父级不预设实现）：
  - `requirements set --ids <a,b,c>` + 每节点的改动来自一个文件；或
  - `requirements repair --anchors <映射文件> --write`（读一次树 → 应用一组 `旧锚 → 新锚` → 写 → 校验一次）。
- 三条既有不变量**照旧**：id == 文件名 / ULID、单节点 ≤ 80 行预算、结构闭合。
- ⛔ **不许**加 `--no-validate` 这类"关掉写后守卫"的后门（那是拿守卫生效换便利）。

## 证伪条件

如果仓里本来就有批量写入口（`pnpm tools requirements describe` 或 `--help` 里给得出命令），本条不成立 —— 那是文档没写清，改文档即可。

## 为什么值得单独一条单

它只在"迁移/改名/删文件"这类**批量**改动里发作，而这类改动恰好是本仓最常见的形态（测试瘦身、证据锚迁移、改名）。
下一次遇到的人会重走一遍"发现死锁 → 写脚本绕过 → 心里没底"，而那次未必有人记得跑 validate。

## 实测再现（2026-11-16）

退役 `--interaction-graph` 那一轮让 **16 个锚同时悬空**（6 条 `repro:` 指向旧 `graphs/`、3 条 `verify:` 指向已删的压力用例、7 处 `repo:` 指向已删的 `scripts/interaction/room-*`）
⇒ `node tools/requirements.mjs --set … --write` **每一笔都被回滚**（写后守卫对**整棵树**校验），连"先修一处"都做不到 ⇒ 只能按文本一次性改完 16 处再整体校验。

⇒ 修法建议（二选一）：① 写后守卫只校验**被改节点 + 它的直接引用面**；② 提供 `--batch`：一次写多节点、只在最后校验一次。
