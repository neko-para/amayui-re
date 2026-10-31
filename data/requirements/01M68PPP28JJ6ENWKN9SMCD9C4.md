# 音效：播放名第五组 · 操作数序 · 后端初值

- id: REQ-01M68PPP28JJ6ENWKN9SMCD9C4
- type: req
- status: dropped
- parent: REQ-01M674XHSX2HSFWSKDWZV2C63D
- dropped_reason: 残差清单式节点：audit.json 的残差已带 what/next，需求树不重复登记；无 audit 归宿的条目已并入 REQ-01M694VKD97ERC5ZBHDA4AHQV2。
- tags: [spec residual]

## 目标

收敛音效通道记录在批 2 暴露的**音效侧**缺口（交付侧已写成合同）。

## 具体项

1. **播放名第五组**（`0x4BC`–`0x4E0`，10 条）：由播放（`0xB4`）写、按通道停止（`0xB6`）清，另有 8 处 `rep movsd` 整块拷入 / 拷出（`0x410560` / `0x41085B` / `0x410D48` / `0x4114B7` / `0x40CFF3` 等）。语义归音频章。
2. **play-sound-effect 的操作数序**：通道 = 第 2、名 = 第 1（与 `0xB6` 不对称），需要复核（`0xB4` handler `0x420B00` → `sub_4B4F60`）。
3. **音效后端宿主块的初值来源**：释放入口只读 `+0x408` / `+0x414`，两者非 0 才转后端；这两个格的初始化 / 装配属宿主侧。
