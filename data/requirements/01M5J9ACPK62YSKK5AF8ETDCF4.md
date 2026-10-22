# 未成篇 opcode 分诊与合并（0x000–0x3FF，除 audio/text-system/input）

- id: REQ-01M5J9ACPK62YSKK5AF8ETDCF4
- type: req
- status: open
- parent: REQ-01M59B6Y8VGCZJN79XVQGHB390
- tags: [spec ops]

# 未成篇 opcode 分诊与合并（0x000–0x3FF，除 audio / text-system / input）

## 一句话

把 0x000–0x3FF 段"真实脚本在用、spec 尚未成篇"的 opcode 做一次分诊，并按可判定的合同
分批成篇 + 在 `apps/emulator-new` 落地；audio / text-system / input 三个子系统本轮**只登记不深挖**。

## 已完成（2026-10-23）

- **分诊**：全量静态足迹 = 游戏根松散 `.BIN` + `_extracted` 共 564 支脚本；三段分配表见
  `.tmp/assignments/ops-{000,100,200}.md`（过程材料，不入库）。
- **0x000–0x0FF**：13 条草稿 / 14 个 opcode **已合并**进 `data/spec`（`60-ops-frame` /
  `60-ops-array` / `60-ops-numeric` / `60-ops-scalar` / `60-ops-wait` / `60-ops-list` / `70-vm`）；
  `spec validate` 13/13；新增 `tools/test/spec-ops-000-sites.assets.test.mjs`。
- **0x000–0x0FF 实现**：`0x63` / `0x60` / `0x74` / `0x7f` / `0xd0` / `0xa1` / `0xa2` / `0xa3`
  已在 `apps/emulator-new` 落地（`0x8c` 原本就有），新增 `tools/test/emulator-new-ops-000.test.mjs`。
- **草稿待合并**：
  - `.tmp/spec-draft/ops-100/`：15 条 / 27 个 opcode（执行区间、场景变换、状态字、存档族、RNG…）；
  - `.tmp/spec-draft/ops-200/`：15 条 / 28 个 opcode（场景 setter 族、纹理、浮点算术、CG、L2D…）；
  - `.tmp/spec-draft/objects/`：CORE-OBJECTS 5 条 —— **已合并**为 `45-objects.md`；
  - `.tmp/spec-draft/save/`：FMT-SAVE 5 条 —— **已合并**为 `65-save.md`（与 `60-ops-save` 重复的
    `SLOT-OPS` 已删）；
  - `.tmp/spec-draft/containers/`：CORE-CONTAINERS 4 条 —— **全部已合并**进 `40-pools.md`
    （`ELEMENT` 先按台账纪律追加更正 `KN-01M5JBPQTZ080A4J100K575P1V` 再合并）；
  - `.tmp/spec-draft/fs-boot/`：CORE-FS / CORE-BOOT 残差（未合并）。
- **第二批合并**（2026-10-23 续）：`ops-100` 13 条 / `ops-200` 13 条（L2D 与 text 邻接各留残差）、
  `objects` 5 条、`save` 5 条、`containers` 4 条；新增 `45-objects` / `60-ops-save` / `65-save` 三章。
  条目 142 → 182，未成篇 opcode 431 → 381。
- **第三批取证收口**：三个只读取证 subagent 补上了第二批剩下的 S1：
  RNG 第 30 位与门同格且服务分支不清位；场景对象只有一张 map、默认记录初值、位 0 生产者；
  FILLTEXTURE 的表项 / 颜色公式 / UB；containers ELEMENT 的台账更正。
- **FMT-SAVE-CODEC 成篇**：`65-save` 新增校验 / 变换 / LZSS / 主体前缀布局规则；
  条目 192 → 193；复审 S1 4 条全部关闭（复核“无 S1”）。
- **场景 10 条 setter 成篇**：`0x21d/21e/21f/230/231/233/234/235/239/242` 已合并；
  条目 182 → 192；`RECORD-LOOKUP` 收窄到 7 条；0x21d 更正为“整份复制”。
  独立复审 4 条 S1 全部关闭，复核结论“本范围无 S1”。
- **第三轮独立复审**：S1 14 条，已处置 10 条（表身份 / 0x220 四格 / 0x1f6 范围 / 默认记录 /
  RNG 门死锁 / 65-save 参数块与头 / 悬空指针 / 40-pools 正文节 / 999 界）；
  **第四轮跟报复审：本批 S1 清零**（0x220 位 1 已指向计时窗；FMT-SAVE-BODY 已标不闭合）。
  剩余为非 S1 缺口：场景 10 条 setter 的字段组、65-save 的字节布局 / 校验和、381 个未成篇 opcode、fs-boot。
  详见 `docs/02-engine/spec-audit-2026-10.md` §18。
- **emulator-new 落地（2026-10-23 续）**：44 个已有 spec 合同但缺 handler 的 opcode 已实现
  （浮点 4、memcpy、纹理尺寸、状态字 / 相邻格 / 状态格 3、格 X/Y 3、模式位 3、
  1000 项表 2、CG、场景清空、三元向量 4、3D 变换 5、退出 1、清第 12 位 1、
  执行区间 2、层号落格 1、乱数启用 1、本地块进入 / 返回 2、场景 setter 8），
  新增 `emulator-new-ops-100-200.test.mjs`；handler 111 → 155，可执行脚本 133 → 135。
- **排除项**（只登记、不写合同）：audio（`0xb4`–`0xc7`、`0x2f6`–`0x2ff`…）、
  text-system（`0x6e`/`0x6f`/`0x196`/`0x1d2`…）、input（`0x72`/`0x0fb`/`0x108`…）。

## 判据

- 每条合并的规则：`spec validate` 13/13；站点 EA 有 assets 守卫；正文自包含。
- 每条落地 handler：`pnpm typecheck` 绿；有 `@env pure` 合同守卫；未实现 opcode 仍**响亮失败**（不 no-op）。
- 每批合并后重算 `audit.gaps[]`（`hasHandler = 已写 + gaps` 必须精确划分）。
- 不把 `.c` 行号 / 助记符当证据；锚只认 EA。

## 下一批

1. 场景 10 条 setter 的字段组逐条成篇（当前只有 `RECORD-LOOKUP` 共享前置）；
   补 `+0xAC/+0x12C` 两个矩阵的生产者与位 0 生产者穷尽性。
2. `fs-boot`：合并其残差收窄项（`06-window` 三句冲突需先裁决）。
3. 继续 `ops-unwritten` 里非 audio/text/input 的残差族（场景过渡 / 点列 / 0x0ff / 0x004 / 0x086 等）。

## 怎么查

`pnpm tools spec report` · `pnpm tools spec chapters` · `.tmp/spec-draft/*/README.md` ·
`pnpm tools requirements plan`。
