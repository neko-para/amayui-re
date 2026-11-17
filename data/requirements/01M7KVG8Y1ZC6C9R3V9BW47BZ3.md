# tools/mutate-check.mjs 的一条变异是自指的：from 只匹配清单自己的字面量 ⇒ 红是静态失配，不是机制被改坏

- id: REQ-01M7KVG8Y1ZC6C9R3V9BW47BZ3
- type: bug
- status: open
- parent: REQ-01M7JHSFB2SSBBNSJ82YDED8M1
- repro: tools/mutate-check.mjs#d.target !== m.buildTarget
- severity: S2
- tags: [toolchain, mutation]

## 一句话

`tools/mutate-check.mjs` 的既有条目（`buildTarget` 比较那条，约 `:3555-3568`）的 `from` 带 **12 个空格**，而实现行只有 **2 个空格** ⇒ 文件里唯一匹配是**该条目自己的 `from` 字符串字面量**。施加变异后 `replace` 改的是清单（不是实现），随后 `mutate-static` 的静态检查因 `from` 片段不再存在而红 ⇒ 工具记「守卫红了」，但**实现里的比较从未被关掉**。这是「只整体退出码非 0 的变异可能假红」的同类（`AGENTS.md` §12.7）。

## 证据（2026-11-17 · buildTarget 单的复审发现）

- `tools/mutate-check.mjs:3555-3568`：`from: '            if (d.target !== m.buildTarget) {'`；实现 `checkBuildTarget` 里的同一行是 `  if (d.target !== m.buildTarget) {`（2 空格）。
- 该 `from` 在文件里的唯一命中 = 条目自己的字符串字面量（12 空格）；`to` 不保留 `from` ⇒ 施加后实现未变、清单条目自身的 `from` 消失。
- 跑 `node tools/mutate-check.mjs --match '<该条>'` 会显示「守卫红了」，但红的原因是 `tools/test/mutate-static.test.mjs` 的 `from 片段已不存在 / 出现次数 ≠ 1`，不是 `buildTarget` 比较被关掉。
- 对照：同轮新增的 spawn 依赖条目特意把 `from` 跨两行写、`to` 原样保留 `from`，红的原因能落到 `h)/i)` 的断言上（见 `REQ-01M7K2J7VXKJMK1BJ3H7YCVJFJ` 的报告 §4.1）。

## 判据

1. `from` 必须在**实现行**里唯一匹配（不是清单字面量）；`to` 原样保留 `from`（或等价），施加后 `--list` 的静态检查仍绿。
2. 真跑 `--match '<该条>'` ⇒ 红 1/1；红的原因必须是**实现里的比较被关掉**（手工施加后跑守卫，看失败断言），不是「from 出现 2 次 / 清单过期」。
3. 该条目的 `what` 写清“红在哪个断言”；若可行，加一条自检/守卫防同类（`from` 的缩进必须匹配实现，而不是清单）。
4. ⛔ 不改其它变异条目；不做通用 AST 分析。

## 未做 / 边界

- 不改 `mutate-check` 的运行器；只修这一条 + 一条守卫。
- 与 `REQ-01M7K2J7VXKJMK1BJ3H7YCVJFJ`（spawn 依赖静态检查）分开：那条已收口，不把本单塞进它的判据。
