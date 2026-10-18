# 批 S1l · 跳表目标集导出：把'目标未知的间接跳转'变成'目标集已知'（L0 的一档可消）

- id: REQ-01M57VNC7QD83CVADYT0ERFA6G
- type: req
- status: doing
- parent: REQ-01M52F3ASMGWQXX64JSA18MP2N
- order: 53
- tags: [engine shape]

## 目标（能力，不是"多分析几个函数"）

把 **"目标未知的间接跳转"** 变成 **"目标集已知"**，并让这件事出现在 L0 判据里 ——
现在它被算成"未闭合"（全表 58 个函数），而其中**绝大多数是跳表**：目标集是能从语料**机械导出**的。

## 为什么可消（已取证，不是设想）

`pnpm tools disasm-at cases --ea 0x41BF50` 已经能导出：

```
跳转表 1 个 · jpt_41BF99 表在 .text 行 41777 → **14 个 case**
[0]→loc_41BFA0 … [8]→def_41BF99 … （全部落在函数内）
```

⇒ "目标未知"对这类函数是**工具的表达缺口**，不是语料缺口。

## 要做的事

1. **一条判据**：`jmp ds:off_X[reg*4]`（跳表形态）⇒ 导出目标集；目标集**全在函数内** ⇒ 该处**不再计入**
   `indirectJmp` 原因（改为"目标集已知 n 个"，进 L1 点名，不挡 L0）；有目标落在函数外 ⇒ 仍然"未闭合"（那才是真逃逸）。
2. **一个落点**：把导出接进 `coverage`（`tools/lib/shape-coverage.mjs` 的 `escapes.indirectJmp`），
   ⛔ **不许**在 coverage 里重写一份跳表解析 —— 复用 `disasm-at cases` 的那一份（两套实现必漂）。
3. **一条守卫**：合成 `.lst` 里放一个"目标全在函数内"的跳表 ⇒ 判 L0 闭合；再放一个"目标在函数外"的 ⇒ 仍判未闭合。
   变异清单一条（把"函数内"判据拆掉 ⇒ 当场红）。
4. **归档口径**：`--json` 里每个 `indirectJmp` 带 `targets: [...]` 与 `escapesFunction: bool`（⛔ 不许只给一句"已解决"）。

## 反模式

* ⛔ 把"目标集导出"说成"函数语义已闭合"：目标集只说**控制流去了哪**，不说它**改了什么**（那是 L1/L2）。
* ⛔ 用"表在 `.data`/`.text` 之后"这种位置启发式代替真的读表项。
* ⛔ 目标落在函数外时也用"跳表"糊过去 —— 那是真逃逸，必须留在未闭合档。

## 复用点（已勘查，下一轮从这里起）

跳表解析**已经有了**：`tools/lib/disasm.mjs` 的 `switchTables(file, idx, ea, {fileLines})`
（返回 `{jptAt, jumpAt, cases, targets[]}`；实测 `0x41BF50` = 14 个 case）。
接入 `coverage` 需要两件它现在没拿的东西：**语料文件路径**（`opcodes.mjs` 的 `listingPath(repoRoot)`）与
**索引对象 `idx`**（`spanOfFunction` 用）。⇒ 下一轮先看 `runCoverage` 能不能直接复用 `disasm.mjs` 的
`loadIndex`/`fileLines` 缓存（⛔ 不许在 coverage 里重写第二份跳表解析）。

## 怎么查
```bash
pnpm tools disasm-at cases --ea <函数起点>      # 现成的跳表导出（本单复用它）
pnpm tools shape coverage --all --json | jq '.counts.byReason.indirectJmp'
```
