# 复核并迁移 spec/台账里指向废弃实现测试的证据锚（之后才能删那 75 个测试文件）

- id: REQ-01M6XZ444RH3YWHAZYWFS8VGA5
- type: spike
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- done_reason: 裁决：不删也不冻结——严格直迁=0（lift 不实现这些引擎语义），删 62 个文件只省 ~1.2 s，却断 107 条 spec 规则（2 reviewed）、38 条需求锚、122 条 accepted ledger 观察；冻结同样省 ~1.2 s 且让绿变假。重开条件见正文。

## 结论（2026-11-09，父级裁决：**不删、也不冻结**）

本单的原计划（"删掉废弃实现测试 + 一条哈希校验替代"）被实测推翻：**严格直迁 = 0**。
`apps/emulator-lift` 执行原机器码、**不实现**这 107 条规则描述的引擎语义 ⇒ 没有等价观察可迁。
删掉它们是拿 ~1.2 s 换三份台账的断链，**不划算**；冻结同样只省那 ~1.2 s，却让"绿"变成假绿
（61 条规则零证据时 `spec validate` 仍绿，因为 #7 只约束 reviewed）。

## 重跑读数（2026-11-09，命令见 `analysis.md` §0；⛔ 不手抄旧数字）

| 量 | 现值 | 旧正文 |
|---|---|---|
| 删除集 | **62** 个（`emulator-new-*` 47 + 点名 15；pure 47 / assets 15） | 75（`5e4fca3` 已删 13） |
| 不在删除范围 | **8** 个（5 个 `emulator-cxx-*` + spec-rule-refs / ledger / manifest-local） | 5 |
| `bindings.json` 的 `guard:` | **141 / 34 文件**（剔 `emulator-cxx-audio-slots` 后 140 / 33） | 140 / 33 |
| 涉及规则 | **107**（draft 105 / reviewed 2；全仓 reviewed 仅 14） | — |
| 需求单侧字段锚 | **38 / 29 节点**（verify 29 / repro 9） | 94 / 29 / 35 错 |
| ledger 指向 | **499 处 / 483 记录**（`accepted` **122 处 / 112 条**） | 未计 |
| 删后红 | spec **169 严格 / 190 self**；requirements **38**；ledger **122** | 190 / 35 / — |

## 为什么"严格直迁 = 0"

- lift 的 CTest 测的是**执行器 / 平台 shim**，这 107 条规则测的是**引擎语义**（池布局 / 帧表 / 数值 op…）。
- 只有 3 处"同子系统不同观察"的部分候选（`skip-logo` / `audio-device` / `option-roundtrip`）。
- 6 个文件 / 38 条锚"要人判"；其余 27 个文件 / 102 条锚唯一去处是**降 draft + 残差**（≈100%）。
- 去重后 **62 条规则删后零证据**（含 2 条 reviewed：`SPC-POOL-LOCAL-FAMILIES`、`SPC-POOL-STRING-SSO`）。
- `tools/frozen.mjs` **不存在**；冻结树 89 个 tracked 文件。本裁决下**不建**（没有要冻结的东西）。

## 重开 / 证伪条件

1. 出现**在维护的证据线**能给出这 107 条规则的等价观察（按 §6 准入门登记）；
2. 或这 62 个文件成为**真实维护负担**（例如因无关重构反复变红、修它要改废弃实现）——那时改用"冻结 + 哈希棘轮"，
   并同时给 spec 加"证据档降级"棘轮，⛔ 不许只删不改证据档。

## 保留的可复核口径（下一轮别重新踩）

- `bindings.json` 的 `guard:` **不能**指向 `.cpp`（`testNames()` 只认 `test(` / `add_test(NAME`，不认 `lift_test::finish`）
  ⇒ 写 `apps/emulator-lift/CMakeLists.txt#<ctest>`；CTest 名是**子串匹配**（`#loader` 会命中 `pe-loader`）。
- `repo:` 与 `verify:` 两套写法都要搜（`audio-device` 落在 `audio.cpp`，与 CTest 名无字面关系）。
- 需求单侧写入口只认 `pnpm tools requirements set --verify/--repro/--done-reason/--unset --write`。

## 明确不做（沿用）

⛔ 补弱证据撑 `reviewed` · ⛔ 先降后升 · ⛔ 改指不存在文件 · ⛔ 为腾预算删别的单 · ⛔ 用"调大容差/删用例"换绿。
