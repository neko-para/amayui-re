# 🐞S3 macOS：Finder 的 corpus/fixtures/.DS_Store 撞穿 fixtures 目录白名单 ⇒ pnpm verify 499/500 红（删了会复发）

- id: REQ-01M7HAK2H6R8RWEG15RBDH5585
- type: bug
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- verify: tools/test/fixtures.test.mjs#★ 目录白名单判据（纯函数）：OS 元数据放过；未登记的普通文件（真样本）必须红
- repro: tools/test/fixtures.test.mjs#悄悄塞进来的样本
- severity: S3
- done_reason: 判据抽成纯函数（tools/lib/samples.mjs 的 classifyFixtureEntry/isAllowedFixtureEntry）：登记集合 | OS 元数据（点文件 + thumbs.db/desktop.ini）通过，其余红。取证：touch corpus/fixtures/.DS_Store ⇒ fixtures 7 pass；再 touch random.bin ⇒ 6 pass/1 fail（真样本依旧红）。变异：mutate-check 加 1 条并实测 1/1 红。门禁：pnpm test 552/552 · pnpm verify 501/501（500 + 新增纯函数单测）。余留：corpus/fixtures/README.md:50 措辞未同步（corpus 边界禁改）；守卫仍只扫顶层（既有）。
- tags: [toolchain, fixtures, platform]

## 一句话

`pnpm verify` 在 macOS 上会被 **Finder 元数据**弄红：`corpus/fixtures/.DS_Store` 落在 `tools/test/fixtures.test.mjs` 的目录白名单之外 ⇒

```
✖ 目录里除了载荷、目录 README、samples.json 及其同名说明书之外，不应有别的文件（防"悄悄塞进来的样本"）
  AssertionError: .DS_Store 没登记在 samples.json 里（要么登记，要么删掉）
ℹ pass 499  ℹ fail 1
```

`.DS_Store` 已经在 `.gitignore:37`（untracked、不会入库），但只要有人在 Finder 里打开过 `corpus/fixtures/` 就会重新生成
⇒ **删掉只是当次止血，下次浏览目录就复发**。

## 复现（2026-11-16 / macOS arm64）

- 现状：`node --test --test-isolation=none tools/test/fixtures.test.mjs` ⇒ 5 pass / 1 fail（上面那条）。
- 对照：把它删掉后 `pnpm verify` ⇒ **500/500**；`pnpm test` ⇒ 552/552（本来就不受它影响）⇒ 失败与代码无关，纯 OS 元数据。
- 触发条件：Finder 打开过 `corpus/fixtures/`（`ls -l` 该文件 8,196 B / mtime 11-13）。

## 两种修法（未做，等裁决）

1. **守卫跳过 OS 垃圾 / 点文件**（`.DS_Store` · `Thumbs.db` · `desktop.ini`）—— 保留"防悄悄塞样本"的原意，只收窄判据范围。
2. **改成正向白名单**（只认 `samples.json` 提到的载荷 + 目录 README + 同名说明书）—— 现在的写法是负向排除，所以任何未知文件都会撞上，包括 OS 文件。

★ 判据（无论选哪种）：修完仍要能红 —— 往 `corpus/fixtures/` 塞一个**真样本**（非点文件）必须依旧红；并在
`tools/mutate-check.mjs` 里加一条对应变异（`AGENTS.md` §5.3）。
⛔ 不要以"删了文件就好"结单：Finder 会重新生成。

## 未做

- 未改 `tools/test/fixtures.test.mjs`（本轮是 mac 构建链接那单的收口，这条按单处理）。
- 未动仓库根与 `corpus/` 下的另外两处 `.DS_Store`：它们当前没触发任何守卫，删了也会复发，不当作修法。
