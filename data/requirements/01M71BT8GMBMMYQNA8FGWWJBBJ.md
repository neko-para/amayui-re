# 🐞S3 `COLLAPSED_RE` 漏了 `COLLAPSED CHUNK OF FUNCTION` ⇒ 这类折叠站点被当成正常函数体（绿而错）

- id: REQ-01M71BT8GMBMMYQNA8FGWWJBBJ
- type: bug
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- verify: tools/test/anchor-lst-collapsed.test.mjs#★ `COLLAPSED_RE` 必须同时认三种折叠说法（`COLLAPSED FUNCTION` / `COLLAPSED CHUNK OF FUNCTION` / `START OF FUNCTION CHUNK`）
- repro: tools/lib/anchor-lst.mjs#COLLAPSED_RE
- severity: S3
- done_reason: COLLAPSED_RE 改成词根判据（/COLLAPSED|START OF FUNCTION CHUNK/i）：旧正则 122/1805 未命中（全是 COLLAPSED CHUNK OF FUNCTION），新正则 1805/1805，第三形态 START OF FUNCTION CHUNK 502/502。守卫两道：pure 形状守卫（语料逐字原行钉三形态 + parseListingText 的 collapsed + parseCollapsedLine 长度）+ assets 机械遍历（每条注释行 100% 命中并端到端复核 EA）。★ 前提纠正：.collapsed 今天没有读者、122 个 CHUNK 形态 EA 与台账 1242 个 bin 锚交集为 0 ⇒ 今天 0 个锚变档，修的是字段正确性与未来消费方。
- tags: [tooling, disasm, anchor]

## 判据（存在性 + 观察）

`tools/lib/anchor-lst.mjs` 的 `COLLAPSED_RE = /COLLAPSED FUNCTION|START OF FUNCTION CHUNK/i`（`:54`）**漏了一种折叠行形态**：
`; [00000012 BYTES: COLLAPSED CHUNK OF FUNCTION …]`。实测语料里就有（`corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst` 的 `004E9B52`）。

- **后果**：那种 chunk 不会被标成 `collapsed` ⇒ 走"函数体"路径 ⇒ 拿到的 `bodies` 只有一行注释、`real` 判定失真。
  依赖这个判定的是锚点解析（`resolveAnchor` 一族）：**折叠站点会被当成正常函数体**，于是"锚点能否解析"这件事在**这一类站点上**给出错答案（绿而错）。
- 判据：① `COLLAPSED_RE` 覆盖 `COLLAPSED CHUNK OF FUNCTION`；② 语料里所有 `COLLAPSED * OF FUNCTION` 形态都被标 `collapsed`（机械遍历 `.lst` 断言，**不是**只看一个样例）；
  ③ 已解析的锚点结论**不变**（`pnpm tools requirements validate` 与 `pnpm tools corpus validate` 仍绿）。

## 为什么现在不做（严重度）

- 今天只有 **1 处**已知站点（`004E9B52`），且本批没有锚挂在那里 ⇒ 取 S3。
- 但它是**静默类**：错的方向是"看起来能解析"，不是"报错" —— 按本仓口径这类要给响亮/给守卫，不能靠"还没踩到"。

## 修法边界

- 只改正则 + 一条机械遍历语料的守卫；⛔ 不要顺手把 `.lst` 里的折叠行"展开"（语料只读）。
- 守卫落 `tools/test/`（pure 档；语料缺席则 `t.skip`）：对 `.lst` 里每一行注释做匹配，断言"含 `COLLAPSED` 的注释行 100% 被 `COLLAPSED_RE` 命中"。
- 同类检查也适用于 `tools/lib/disasm.mjs` 的 `parseCollapsedLine`（本批另一个 agent 说它"按行形态覆盖"了 —— 那条要写清判据，别只靠注释）。

## 与别的单的关系

- 发现于 `REQ-01M714HVX2P6VJQ1AHK6KJDFM7`（折叠区间取字节）的实现过程。
- 相邻：`REQ-01M713MSM6WKDDTMHCXR6FJX5Q`（折叠成因：1805/1805 全是库名、跨模块复现）。
