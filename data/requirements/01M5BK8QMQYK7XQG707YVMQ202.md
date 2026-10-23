# 当前实现总览与交接（会话 / 设备继续）

- id: REQ-01M5BK8QMQYK7XQG707YVMQ202
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 0
- tags: [handoff, engine spec]

## 一句话

2026-10-23 交接：`apps/emulator-new` 已从 SYSTEM4 启动链走到 TITLE（id `0x5264`）并进入输入环；TITLE 的 `Version 0.00.0000` 正常；CLI 与 Electron 都支持 `--skip-logo`。**工作树有未提交改动**（长期纪律：不 `git add` / `commit`）。

## 本会话已完成（可复核凭据）

- **Electron `--skip-logo`**：`main.ts` / `protocol.ts` / `session.ts` 传 `showLogo:0`；实跑到 TITLE 时日志无 LOGO 影片。
- **TITLE `Version` 破裂**：根因两条 —— `0x23b` 被误归入 ADV 白名单（实际是 CG 数字条），`0x1f9` 的 `0` 是**无键**且上传按 **32 位 ARGB** 比较。已实现 `core/cg-digit.ts` + `cgDraw23b`、修 `applyColorKey`/renderer；spec 新增 `CORE-OPS-SCENE-CG-NUMBER-23B` 并修正颜色键规则。截图 `.tmp/final-title.png` / `.tmp/fix-ver-big.png`。
- **残差收口**：`RES-OPS-SAVE-READ-FIELDS`、`RES-OPS-SAVE-FILES-STH`、`RES-CONFIG-FIELD-TRIM`，均含 spec/bindings/fixtures 或 pure 守卫 + mutation。
- **复审修正**：`0x2da` 上限 10/先判界；`0x23b` 四表清理与 `digits≤0` 读序；`0x1f7/0x1f6` 四表集合；`0x1ab/0x1ac` “两个文件都尝试、后一步覆盖”；`0x1af` 表面 0/1/2 拒绝；配置字段裁剪/段头 DBCS 边界。
- **绘制顺序**：新增 `CORE-SCENE-DRAW-ORDER`（无命令记录时 key 升序、同键网格先于绘制项）；有命令记录的两档顺序仍为残差。
- **review 方法**：`amayui-spec` §9.2「缺陷后的 spec 归因」+ `docs/02-engine/spec-audit-2026-10.md` 三案例。

- 机制地图仍以 `data/spec/core/README.md` 为准：`CORE-ISA` / `CORE-POOL` / `CORE-CONFIG` / `CORE-FRAME` / `CORE-OPS` 等；本单不复制机制表。

## 当前门禁

```bash
pnpm test                 # 824 pass / 0 fail
pnpm test:assets          # 157 pass / 0 fail / 26 skip
pnpm test:mutation        # 265/266 变红；1 条缺 .BIN 语料无法判定
pnpm tools spec validate  # 13/13 全绿，未登记 0
pnpm tools requirements validate # 6/6 全绿
pnpm typecheck            # 六工程全绿
```

## 待处理（按优先）

1. **约 339 条可静态取证残差**（分诊基线 342；本轮已闭合 3 条，另有复审新增/收窄的残差）（已排除 audio / ADV 文字 / Live2D）。建议顺序：
   `RES-FS-AAI-KEY` → `RES-ALF-MULTI-INDEX` → `RES-TEX-DRAW-CONSUMER` →
   `RES-SCENE-DRAW-ORDER-COMMAND` → `RES-FRAME-LOAD-AUX-ARRAYS` →
   `RES-INPUT-KEYTOTAL-INIT` → `RES-BOOT-GFX-PARAMS` → `RES-POOL-MEMFLIP` →
   `RES-AGF-CHANNEL-ORDER` → `RES-VM-EXC-PROPAGATION`。
2. **57 条 blocked**：本机 macOS 无 Windows/Wine，且部分缺样本 / 需人裁决。类型：窗口外观与真机像素、输入物理映射、存档 UB 观测、跨平台浮点/时钟、BMP 低位深/32bpp 读取。
3. **已知实现/合同缺口（未收口）**：
   - `RES-SAVE-STH-CAPTURE`：`RenderHost.captureTexture` 仍把整份 BMP 放平台侧；合同要求 core 自建头 + 行翻转 + 通道转换。
   - `RES-SAVE-STH-SHORTREAD`：短读行为未规定。
   - `RES-SAVE-STH-SURFACE`：`0x1af` 的 op3 `0/1/2` 拒绝尚未在 handler 实现（spec 已写）；当前实现先读文件再上传。
   - `0x2da` 编号 10 与 T2[0..6] 重叠未建模（`RES-CG-SET-RECORD-INIT`）。
   - `RES-SCENE-DRAW-ORDER-COMMAND`：命令记录族未成篇。
   - audio / ADV 文字 / Live2D 仍按用户要求排除。

## 新会话从这读

- 技能：`amayui-spec`（§9.1 编辑后独立复审、§9.2 缺陷归因）/ `amayui-emulator` / `amayui-re-engine`。
- 进度：`pnpm tools requirements plan`；审计：`pnpm tools spec report`。
- 本单只记交接；具体状态以子单与 `pnpm tools requirements plan` 为准。
