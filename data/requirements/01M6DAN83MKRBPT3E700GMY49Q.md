# 颜色字节序：0xRRGGBB vs 0xBBGGRR（60-ops-render / 60-ops-text / 0x33f 互斥）

- id: REQ-01M6DAN83MKRBPT3E700GMY49Q
- type: req
- status: done
- parent: REQ-01M6DAMF1MN58WVR07ZYMQ8FCP
- done_reason: 第一问（脚本侧字节序）已裁决并改正文（60-ops-render / 60-ops-text / 60-ops-scalar 口径对齐）；第二问（32 位表面通道归属）已收窄到残差 RES-RENDER-TEXT-SURFACE-FORMAT 继续跟踪。
- tags: [spec, color]

## 裁决（2026-11-02）

- **脚本侧 24 位颜色 = `0xRRGGBB`**（高字节红、低字节蓝）。
- `0x76` / `0x77` 把操作数的**低 24 位字节倒序**后写进字体对象；存储侧 = **COLORREF 的 BGR 序**（低字节红、高字节蓝）。
- `0x82` 借用的主 / 副色写格走同一反转；`0x204` 读存储值、直接作为 `SetTextColor` 的 `COLORREF`。
- 因此 `60-ops-render.md:108` 的「低字节为蓝」是错的，已改成「低字节红（COLORREF BGR 序）」；`60-ops-text.md:102` 的 `0x112233 → 0x332211` 与 `60-ops-scalar.md` 的「低 24 位字节倒序」都保留。
- 场景混合 `0x33f` 的 `0x00RRGGBB` 位段**不变**；正文已加「两套字节序不要互借」。

## 证据（EA）

- `0x76` handler `0x41F3B8..0x41F3D5`：`movzx edx,al` / `movzx eax,ah` / `movzx eax,[var_4+2]` / `shl 8` ×2 ⇒ `b0<<16 | b1<<8 | b2`，写 `[Font+550h]`。
- `0x82` 路径 `0x466428..0x466460`：同形序列写 `[Font+550h]` / `[Font+554h]`。
- 调用点 `0x46094E`：`mov edx,[ebx+550h]` → `push edx ; COLORREF` → `SetTextColor`（`0x456A5A`）。

## 仍未定（不在本单）

`RES-RENDER-TEXT-SURFACE-FORMAT`：32 位表面的**通道归属**（源色三字节 → 目标像素三通道）仍需一条写入路径 + 像素对照；已在该残差里收窄。
