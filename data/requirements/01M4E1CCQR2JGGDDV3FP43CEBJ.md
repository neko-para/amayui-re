# ADR ② 剩余 · float / string 池迁到区域（已由裁决解决）

- id: REQ-01M4E1CCQR2JGGDDV3FP43CEBJ
- type: req
- status: done
- parent: REQ-01M4AGPP1T9A60HQYW3GX9W592
- done_reason: float 族已迁区域（格 = float32 位模式）；string 族由决策 REQ-01M4E07ZQ9S7EBA1SK0PREPY4E 定案（不透明元素 + 地址窗口），0x8/0xe 已实现且守卫覆盖。

## 处置（已裁决）

* **float 族**：早已迁到区域（格内容 = **float32 位模式**），守卫在 `tools/test/emulator-pool-regions.test.mjs`。
* **string 族**：由决策 `REQ-01M4E07ZQ9S7EBA1SK0PREPY4E` 定案 —— 元素按用户口径**不透明**（JS 字符串，
  数据留在池的 Map），池进地址空间只为**发地址**（区域 `elemBytes = 28`，与引擎元素步长一致）。
  ⇒ 本单原来的担心（28 字节 SSO + 长串的堆）**不再需要**，`0xe`（local string-ptr，语料里 1446 次）可用。
* 按字节访问字符串元素的指令**一律抛**（不许静默给个东西）。
