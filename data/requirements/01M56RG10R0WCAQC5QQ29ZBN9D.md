# 批 S2w · 载体证据棘轮 G16：arg 载体必须带可解析 witness（已补 139/174，剩 35 条按间接调用/来路不可读分档）

- id: REQ-01M56RG10R0WCAQC5QQ29ZBN9D
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- verify: tools/test/shape-claims-guards.test.mjs#★㉙
- tags: [engine, shape]

# S2w · 载体证据棘轮（G16）—— **已落**

## 问题

模型里 174 条 `kind:"arg"` 载体，**一条 `witness` 都没有** —— 全部是"`F` 的 `ecx` 装着 `T_X*`"这句断言，
却没有任何 EA 挂在上面。`witness` 这个字段此前只有 `kind:"global"` 在用（G13 要求它必须是"写该全局的 store"），
`arg` 载体等于**免检**。

## 补齐（两批）

- **135 条自动补齐**：用 `tools/lib/shape-access.mjs` 的 `analyzeLines` 对每个候选调用方跑数据流，
  取"调用点上 `ecx` 的来路"（`this` / `interior(+K)` / `member(*(+K))` / `global`）作为 witness；
- **4 条手工裁决**：`c92` `0x4baa2e`（`mov ecx,eax` 紧跟 `operator new(0xAABC8)`）、`b10c08` `0x4b92b7`
  （`mov ecx,ds:dword_55E1BC` + `add ecx,408h`）、`b10c23` `0x40da7a`（`lea ecx,[ebx+511Ch]`）、
  `c112` `0x46947d`（`mov ecx,ebx`，而 `0x469290 mov ebx,ecx`）；
- **新批次一律自带 witness**（`merge-carriers-patch.mjs` 拒绝无 witness 的提案：batch-14 的 40 条全部有）。

## 判据（G16，已写进 `shape claims`）

1. `arg` 载体的每个 witness 只能是两类：**调用点**（那一行逐字是 `call <c.fn>`）或**来路**（该行出现 `ecx`）。
   ⛔ 其它一律红。实测这样抓到 4 条坏 witness —— 其中两条是 `lea ebx,[ebx+0]`（**对齐填充**，拿它当证据等于没证据）。
2. **至少一条必须是调用点**（只有来路证据证明不了"真的被这样调用过"）。
3. 语料里**确实没有** `call <fn>` 的载体不许编 witness，要声明 `indirectOnly: true`；
   该声明**可反证**：语料里若有 `call <fn>` ⇒ 当场红。
4. 既无 witness 又没声明 `indirectOnly` ⇒ **note**（backlog），⛔ 不算通过。清完再翻红。

## 现状

214 条 `arg` 载体：**179 条**有落在 `call` 上的 witness · **34 条** `indirectOnly`（机械核实：语料里 0 处
`call <fn>`）· **1 条** backlog：`c20 sub_401410`（唯一调用点 `0x455a62` 是 `push` 传参，`ecx` 来路不可读
—— 这个载体本身可疑，先挂着）。

★ 4 条坏 witness 已修（`.tmp/shape/fix-bad-witness.mjs`：重挑"调用点 + ecx 来路"配对，
例 `b14-08` → `0x406ada`+`0x406ad8`（`mov ecx,ebx`））。

## 落点

- `tools/lib/shape-claims.mjs`：G16 块（在 G14 之后、适配器检查之前）+ `eaTextOf` / `eaInBody` 两个惰性助手
- `tools/test/shape-claims-guards.test.mjs`：`★㉙`（6 个反例：合法两条 / 只有来路 / 拿填充当证据 / backlog / `indirectOnly` 说谎 / `indirectOnly` 成立）
- `tools/mutate-check.mjs`：两条（"不再要求至少一条调用点" · "`indirectOnly` 不再可反证"），115 → 117 条
- `docs/02-engine/shape-census.md`：「载体证据棘轮（G16）：`witness` 必须有一条落在调用点上」

## 复现

`pnpm tools shape claims data/shape/carriers.json | rg G16` ⇒
`G16 载体证据：179 条 … 34 条 indirectOnly … 1 条还没有 witness（c20）`。
