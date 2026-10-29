# spec 成篇 · 声音族（0xc5/0xc6/0xc7/0x1ba/0xb6/0xb7/0xb8/0xba）

- id: REQ-01M63XDZ26ZDGHR9DJ9PZZAC84
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- order: 2
- verify: tools/test/spec-gaps.test.mjs#★ 精确划分
- tags: [engine spec]

## 判据（可判定）

`audit.gaps[]` 里不再出现这 4 个 opcode：`0xc5` `0xc6` `0xc7` `0x1ba`；它们各有规则标题、`bindings` 站点、
`audit.rules` 条目；`pnpm tools spec validate` 13/13 绿 + 精确划分守卫绿（收口凭据）。

## 已落（2026-10-29）

`data/spec/chapters/60-ops-voice.md`：

- `CORE-OPS-AUDIO-VOLUME-GET`（0xc5）：声道 0..4 ⇒ 落点 ← `sound:Volume0..4`；越界不写、只上报。
- `CORE-OPS-AUDIO-VOLUME-SET`（0xc6）：键 ← 原值 **且** 下发到该声道；越界两样都不做；**键必须写（即使后端不在）**。
- `CORE-OPS-AUDIO-MODE-GET`（0xc7）：**音乐用符号判真（≥0）**，音效/语音/影片用"≠0"——四支逐条写进表。
- `CORE-OPS-AUDIO-MODE-SET`（0x1ba）：音乐 = 旧值 ±3 阶梯（同侧 no-op）；音效/语音 = 归一 0/1（同态 no-op）；
  影片 = **原值**（同值 no-op）。★ 影片不归一这一点是独立复核 `.lst` 时纠正的（实现注释也这么写）。

## 取证与订正

- `.lst` 逐 handler 复核：0xc6 是**双写**（键 + 下发）、0xc7 的四支阈值**互不相同**、0x1ba 的影片支**原值写**。
- `apps/emulator-cxx/src/core/vm_ops_engine.cpp` 的 `op_1ba` 注释里"尚无规范条目"已改成指向规则 id
  （`spec-rule-refs` 守卫会解析这个 id）。

## 范围调整

原单还含 `0xb6` / `0xb7` / `0xb8` / `0xba`（声音槽 stop/play + 音乐渐变对象）—— 它们**已挪到单独一张单**，
因为那四条牵涉"渐变对象"的字段语义（`0xAA5D8`）与 ds 槽的两套边界，值得单独取证 + 单独复审。
