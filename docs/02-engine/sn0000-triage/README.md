# SN0000 足迹残差 triage 报告

这组报告是 `docs/02-engine/spec-audit-2026-11.md` §九的**证据附件**：对 `route sn0000` 的足迹里
**路径内**残差逐条给出判定（covered / narrowed / real-gap / duplicate / needs-machine / deferred-audio）、
依据（正文原句或 EA + 指令原文）与处置建议。它们**不是**规范正文，也不进分发产物。

## 怎么重算

```bash
# 1) 跑足迹（两次逐字节相同；--settle 覆盖进入 SN0000 后的稳定期）
pnpm tools routes footprint --id sn0000 --steps 2500000 --settle 2000000 --twice --install "$AMAYUI_INSTALL"

# 2) 按足迹分三桶（路径内 / 路径外 / 判不了）
node tools/spec.mjs --residuals --footprint .cache/footprint/sn0000.jsonl --scope path
```

路径桶按 **opcode** 粗判：规则覆盖的 opcode 在动态足迹里出现，该规则的所有残差都会进 `path`。
所以「在路径内」不等于「这条残差的分支真的执行了」；每份报告都单列了本次足迹到底跑到哪条支。

## 报告与簇

| 文件 | 规则簇 |
|---|---|
| `report-a1-font-format.md` | `0x204` 字体 / 文本格式 · `0x1d2` · `0x195` · `0x7a` · `0x20a` |
| `report-a2-cellreveal-advext.md` | `0x1ce` 逐格揭示 · ADV 文本层 / 门 · 列表默认组 |
| `report-b-choice.md` | ADV 选项 `0x90` / `0x97` / `0x93`·`0x94` |
| `report-c-scene3d.md` | 场景 3D `0x218`–`0x242` |
| `report-d-scene.md` | 场景 `0x203` / `0x215` / `0x224` / `0x23b` / `0x256` / `0x32a` / `0x344` |
| `report-e-texture-render.md` | 纹理创建 / 释放 / 尺寸 · 清屏 `0x20e` · 帧步进 |
| `report-f-state.md` | 池 `lea` / `memcpy` · 状态字 · 层格 · 存档布局 |
| `report-g-audio.md` | audio 18 条（13 条不是音频后端缺失） |
| `report-h-vm-timing.md` | 运行区间 `0x1f4` / `0x1f5` · 等待 · 影片对象表 |

## 边界

- 证据锚只有 EA（`.lst` 的行首地址）；`.c` 的行号 / 文本、函数名、助记符都不当锚。
- 报告里的「判定」是**裁决建议**；落进 `data/spec` 前还要过 `pnpm tools spec validate` + 独立复审。
- 报告写在 triage 当时；规范正文变化后，以 `pnpm tools spec residuals` 与 `data/spec/audit.json` 为准。
