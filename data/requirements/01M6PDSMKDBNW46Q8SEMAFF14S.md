# emulator-lift · boot 中 >1s guest 函数的 native 化

- id: REQ-01M6PDSMKDBNW46Q8SEMAFF14S
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/native_override.cpp#native-override
- done_reason: 2026-11-07：本单一共 5 个 >1s 热点全部落地（48B090 / 436A80 / 429020 / 48AE90 函数级 + 414AC0 段级）。真实 boot（max-frames=1 + skip-logo，fresh appdata）：关 33.26 s / guest-block 24,149 ms → all 7.39 s / 3,416 ms；首帧 hash 0xD64F5548CAFE92E9、像素哈希 0x3FD4EB9B0441484C、BMP SHA256 两边逐位相同；启用 all 后 guest 墙钟表无 >1s 函数（最大 226.9 ms）。ctest 58/58 全绿。
- tags: [emulator-lift native-override boot profile]

## 目标

用**墙钟**（不是只看指令权重）列出 boot 首帧累计 >1s 的 guest 函数，逐个 native override；
每个都保持解释器回退、有守卫、默认关闭，并用真实 boot A/B 验证首帧逐位不变。

## 现状（2026-11-07）

| 函数 | 关 override 的墙钟 | 做法 | 子单 |
|---|---:|---|---|
| `0x48B090` | 5586.9 ms | 函数级 override（`NormalTexture::UploadImage`） | 本单（2026-11-06） |
| `0x436A80` | 4399.7 ms | 函数级 override（S4IC/LZSS） | `REQ-01M6PCDA64HJV9RG149MHWMGRJ` |
| `0x414AC0` | 1196.7 ms | **段级** override（中段填池循环 0x415384；不是整函数） | `REQ-01M6RVTSSTQ7DEB1GYCK4NYCN8` |
| `0x429020` | 750.0 ms | 函数级 override（`Map::findIndex`） | `REQ-01M6RVTK7D681GX90C5C99JP1Z` |
| `0x48AE90` | 663.4 ms | 函数级 override（`NormalTexture` 纯色填充） | `REQ-01M6RVTK7D681GX90C5C99JP1Z` |

★ `sub_414AC0` 的旧结论“热段是 82k 条容器构建 + 文件 I/O”**是错的**：块级拆解显示
7.39M hits 里约 7.37M 来自单个块 `0x415384`（填池循环）。整函数（文件 I/O + guest 容器/堆）
仍然不做 native；被替换的只是那一段 —— 见 `REQ-01M6RVTSSTQ7DEB1GYCK4NYCN8`。

## 实测（真实 boot，headless，`--max-frames 1 --skip-logo`，每次 fresh `--appdata`）

- 关 `--native-overrides`：wall **33.26 s** · guest-block **24,149 ms** · insns 768,559,249。
- `--native-overrides all`：wall **7.39 s** · guest-block **3,416 ms** · insns 174,445,008。
- 首帧 d3d9 hash `0xD64F5548CAFE92E9`、像素哈希 `0x3FD4EB9B0441484C`、`--dump-frame` BMP 的
  SHA256 两边**完全相同**。
- 启用 `all` 后 guest 墙钟表里**没有 >1s 的函数**（最大 226.9 ms / `sub_4E0760`）。

## 实现要点（保留给后续）

- `NativeOverrideTable`（函数级：call 目标 + 只写 EAX）与 `NativeBlockTable`（段级：block 入口 +
  可写任意寄存器 + 跳到出口 + `replaced_insns` 补账）是**两张表、两种语义**。
- 段级项第一次 `Declined` 就永久停用（前提是静态的；继续每迭代重试只会把成本加回热路径）。
- 函数级项仍是“一次 override = 1 条”的 insns/时钟口径（虚拟时钟相位**明确不做补偿**，用户裁决）；
  段级项按被替代的 guest 指令数补账（否则首帧可能偏）。
- 每项都必须能 `--native-overrides all,-名字` 单独排除；`-名字` 优先于启用项。
- 48AE90 的位深来源是 `ds:dword_5518C0`（配置 `display:TextureBit`），⛔ 不是 DDraw 显示 bpp。

## 验收

1. 上述 5 个函数都有默认进 `all` 的 override（`debug=false`），且都有守卫 + 真实 boot A/B。
2. 首帧像素/hash 与关 override 逐位一致；回退路径不留下锁/半状态；safe mode 未提交 ⇒ 回退。
3. `ctest --test-dir apps/emulator-lift/out --output-on-failure` 全绿（58/58）；新项各有会红的守卫
   （已做变异验证：16bpp 行步进 / flags PF / 段级 insns 补偿）。
