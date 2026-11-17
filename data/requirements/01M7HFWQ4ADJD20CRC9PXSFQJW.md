# opcode 0x0CD（姑且叫 get-input-type）语义未登记：handler 形状像「调用待定回调」，名字是猜的

- id: REQ-01M7HFWQ4ADJD20CRC9PXSFQJW
- type: spike
- status: done
- parent: REQ-01M7JHSEKXW8KAKB6K2RS645M7
- done_reason: 已按 EA 锚收口（台账 KN-01M7M6B79X6S6428733S4E3P13 / KN-01M7M6B3477V7H2Z1Z0S04475G）：opcode 0x0CD 的 handler = sub_41ACD0，观察层没有'读输入类型'——它只在 Engine+0x5EEA4 的按帧槽 256 项表上推动写指针/做深度校验（PC=base+4*target）。助记符属命名层、未复核；SBUNKI 主轮询等的 (local 90) 由鼠标回调的确认支路写，不是这个 opcode。同族 0x2FC（i2fc）= 读触摸输入队列（sub_431BA0），与 'i2fc 返回值' 无关（eax 被 VM 丢弃，脚本看的是操作数 1）。
- tags: [emulator-lift, reverse, semantics]

## 一句话

`get-input-type`（opcode `0x0CD`）这个助记符来自 `packages/age-format/src/asm/instruction-set.json`（**命名层**，不是观察）；
它的 handler `sub_41ACD0` 形状与 `0x8F call` 同形 ⇒ **语义未登记**（`AGENTS.md` §6 准入门要求结论绑可再校验观察）。

## 已知（来自 `REQ-01M7HC1Z1RD4DNH25BN5ZE1HQK` 的取证，EA 锚）

- 读 `((pc−base)>>2)+1` 作为返回位压入；`Engine+0x69240 != -1` 时 `PC = base + 4*target`。
- 对照：`0x8F call`（`sub_420560`）压的是 `+3`。
- ⇒ 更像"调用一个待定回调"，而不是"读输入类型"。名字是猜的，**不许当语义用**。

## 要产出

1. 这个 opcode 到底做什么（按 handler 的可观测副作用逐条：写哪里、读哪里、控制流怎么变）。
2. 它与 `0x8F call` 的差别（返回位 +1 vs +3 意味着什么）。
3. 一条**可再校验**的观察（EA 区间 + 内容摘要，或一个能红的守卫用例 id）。
4. 结论进 `data/ledger` / `docs/03-engine`（本单只出结论，不实现）。

## 判据

- 每条结论带 EA 锚（⛔ 不锚 `.c` 行号）；`instruction-set.json` 里的名字要么被观察支持、要么被标成"未复核"。
