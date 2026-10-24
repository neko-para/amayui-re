# 当前实现总览与交接（会话 / 设备继续）

- id: REQ-01M5BK8QMQYK7XQG707YVMQ202
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 0
- tags: [handoff, engine spec]

## 一句话

2026-10-24 交接（第十四次）：`apps/emulator-new` 已从 SYSTEM4 启动链走到 TITLE（id `0x5264`）并进入输入环；TITLE 的 `Version 0.00.0000` 正常；CLI 与 Electron 都支持 `--skip-logo`。**工作树有未提交改动**（长期纪律：不 `git add` / `commit`）。

## 残差批次（全部已收口）

第三批到第十五批，每批都有独立复审与门禁；明细见 `docs/02-engine/spec-audit-2026-10.md` §20–§32。关键批次：

- 第三批：AAI/ALF 多索引、绘制链、池翻页 0x6d、异常三支、图形参数。
- 第四批：场景命令种类 1/3、纹理消费者位置 / 颜色 / 附加组、图形重试与旧式实参。
- 第五批：分辨率判据、旧式降级、日志落点、遮罩、源矩形窗口 4.1、0x229 显示适配。
- 第六批：0x199 进入本地块、0x7c 深度、种类 2 全字段、日志路径。
- 第七批：0x102 / 0x103 / 0x104 / 0x105 / 0xfc 按住派发按键族、种类 0 出口、深度 = 统一文件 id。
- 第八批：显示适配精确公式、矩阵互斥乘入门、5.1 视图矩阵门纠错。
- 第九批：命令档执行合同 + S0 规划器。
- 第十批：12 抹除图案条件表 + 纯函数、效果技法内部。
- 第十一批：存档槽范围 / 目录 / 半槽、**0x1a1 是读档不是写档**（纠错）、STH 头。
- 第十二批：STH 尾段归属、`wipePattern` 接规划器（S2 第一步，仍不绘制）。
- 第十三批：存档负载六块身份、局部池、帧记录、740 表 = 场景记录表（纠正"只写表"误判）。
- 第十四批：存档写侧块布局落地（浮点写块 1、tailBase 洞、v1/v2 门控）。
- 第十五批：混合状态闭合（中立值 = 覆盖式写入；种类 0 默认退路例外）、尾段 = 消息窗口命令日志 + 检查点。

## 当前门禁

```bash
pnpm test                 # 998 pass / 0 fail
pnpm test:assets          # 201 pass / 0 fail / 1 skip
pnpm test:mutation        # 331/331 按预期变红
pnpm tools spec validate  # 13/13 全绿，未登记 0
pnpm tools requirements validate # 6/6 全绿
pnpm typecheck            # 六工程全绿
```

## 待处理（按优先）

1. **真实绘制 S2+**：离屏画布 36/37 的建 / 切、表面拷贝、逐像素遮罩、把 `strokes` 送到 pixi、效果技法、进度插值、`clearTable` 真实清表。
2. **保存读侧**：`0x1a1` 的负载还原 / 整池重编码；`rec[259]` 的帧位置表（写侧现在仍写位置本身而非表下标）；引用三池内容；尾段场景表。
3. **`createDevice` 忽略 `backBufferWidth/Height`**；`shaderTier` 映射未取证。
4. 剩余静态 / 需数据项：`RES-SCENE-COMMAND-BLEND-INTERFRAME` / `-KIND0-ALPHA` / `-WIPE-BOUNDARY` / `-EFFECT-EQUIVALENCE` · `RES-SAVE-TRAILER-FIELDS` / `-FRAMEREC`（表填充者）· `RES-SAVE-STH-SHORTREAD` · `RES-SAVE-SLOT-SPACE` · `RES-SCENE-DISPLAY-FIT-SCALE-DIRECTION` · `RES-INPUT-103-NEGATIVE-MODULUS` / `-104-CURSOR-INIT` · `RES-BOOT-GFX-LOGSINK-PATH-DATA` / `-SINK2` · `RES-TEX-...-OOB` / `-SELFTARGET` / `-BELOW-RANGE`。真源 `pnpm tools spec report`（unknown≈446）。
5. 需要真机 / 真样本：负键绘制项、同键叠放、键总数启动值、32bpp AGF、遮罩极性、自引用/越界、种类 0 合成 alpha、缩放方向、STH 短读。
6. audio / text-system / Live2D 仍按用户要求排除。

## 新会话从这读

- 技能：`amayui-spec`（§9.1 编辑后独立复审、§9.2 缺陷归因）/ `amayui-emulator` / `amayui-re-engine`。
- 进度：`pnpm tools requirements plan`；审计：`pnpm tools spec report`；本批：`docs/02-engine/spec-audit-2026-10.md` §32。
- 机制地图以 `data/spec/core/README.md` 为准：`CORE-ISA` / `CORE-POOL` / `CORE-CONFIG` / `CORE-FRAME` / `CORE-OPS` 等。
- 本单只记交接；具体状态以子单与 `pnpm tools requirements plan` 为准。
