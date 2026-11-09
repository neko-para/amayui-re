# 给 cxx 证据锚补代表性变异 或 在 spec 侧降级（二选一，附判据）

- id: REQ-01M6Y28WF82JTAPQMDJN01F8CT
- type: req
- status: open
- parent: REQ-01M6XZ444RH3YWHAZYWFS8VGA5

## 为什么有这张单（现状，实测）

父单 `REQ-01M6XZ444RH3YWHAZYWFS8VGA5` 已顶到 80 行预算（不变量 #5 的上限），这一范围单独成单。

- 指向 `apps/emulator-cxx/**` 的**变异条目 99 条已删**（审计判 DROP-L3；HEAD **516 → 现 49**）。这 99 条里 **56 条的守卫是 cxx 的 CTest**、**43 条是 5 个 JS 用例**的守卫。
- 守卫本身还在：`apps/emulator-cxx/CMakeLists.txt` 里 **87 条** `add_test`（它仍可执行，是**对照实现**，见 `AGENTS.md` §11）。
- 但 `data/spec/bindings.json` 有 **101 处**证据锚（去重 **45** 个 CTest 名）指向它们，`repo-guards.md` §T-9 也仍把这批 CTest 记作 spec 证据锚
  ⇒ 今天"这些 CTest 会红"**只剩历史声称、没有机械证明**（HEAD 那 49 条变异也只覆盖过 45 个锚里的 32 个）。
- ⇒ 结论：**证据锚与它的机械证明脱钩了**。要么把证明补回来，要么把锚降级 —— 不许继续挂着"reviewed"却没有会红的守卫。

## 复核命令（都能重跑；数字要现算，别手抄）

```bash
node -e "import('./tools/mutate-check.mjs').then(m=>console.log(m.MUTATIONS.filter(x=>x.file.startsWith('apps/emulator-cxx/')).length))"   # 0
rg -o 'apps/emulator-cxx/CMakeLists\.txt#[A-Za-z0-9_-]+' data/spec/bindings.json | wc -l                                                    # 101
rg -o 'apps/emulator-cxx/CMakeLists\.txt#[A-Za-z0-9_-]+' data/spec/bindings.json | sed 's/.*#//' | sort -u | wc -l                           # 45
rg -c '^add_test\(NAME' apps/emulator-cxx/CMakeLists.txt                                                                                     # 87
node tools/mutate-check.mjs --list | wc -l                                                                                                   # 49（HEAD 的变异条目数）
```

## 判据（二选一，选定后写进 `done_reason`；两条路线都要留下可复核命令）

**① 给证据锚补代表性变异**（每类一条，⛔ 不回到"每个常量一条"）：
- 锚若迁到 `apps/emulator-lift`，就落**它的顶层 CTest**（顶层无条件注册 = `@env pure` 档，见 `AGENTS.md` §5）；
- ⛔ **不给 cxx 冻结线续命**：不往 `apps/emulator-cxx` 加新变异、不加新测试、不扩它的 CTest。

**② 在 spec 侧把这些锚降级**：
- 换成 `apps/emulator-lift` 的守卫，或降证据等级（`reviewed` → `draft`）+ 在残差里登记"缺哪条观察才能升回来"；
- ⛔ 之后**不许再标回 `reviewed`**（父单「明确不做」第 2 条）。

## 收口判据（缺一条不算完）

1. 上面 5 条复核命令的读数与选定路线**对得上**（要么 101 → 0 且每条锚都有会红的守卫，要么每条降级都留了残差登记）；
2. `pnpm tools spec validate` 与 `pnpm tools requirements validate` 全绿；
3. `pnpm test` / `pnpm verify` 全绿（降级路线不许靠删断言变绿）。
