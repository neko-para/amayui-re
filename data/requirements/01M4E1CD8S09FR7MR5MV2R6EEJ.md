# 占位核对（马上删）

- id: REQ-01M4E1CD8S09FR7MR5MV2R6EEJ
- type: req
- status: dropped
- parent: REQ-01M4AGPP1T9A60HQYW3GX9W592
- dropped_reason: 这是核对"add 是否真落盘"用的**探针节点**（标题即写明"马上删"），交付物为零 ⇒ 不做了；它本身不属于任何需求。

## 处置（已裁决）

* **float 族**：早已迁到区域（格内容 = **float32 位模式**），守卫在 `tools/test/emulator-pool-regions.test.mjs`。
* **string 族**：由决策 `REQ-01M4E07ZQ9S7EBA1SK0PREPY4E` 定案 —— 元素按用户口径**不透明**（JS 字符串，
  数据留在池的 Map），池进地址空间只为**发地址**（区域 `elemBytes = 28`，与引擎元素步长一致）。
  ⇒ 本单原来的担心（28 字节 SSO + 长串的堆）**不再需要**，`0xe`（local string-ptr，语料里 1446 次）可用。
* 按字节访问字符串元素的指令**一律抛**（不许静默给个东西）。
