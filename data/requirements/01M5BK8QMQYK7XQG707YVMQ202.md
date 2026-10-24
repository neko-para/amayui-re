# 当前实现总览与交接（会话 / 设备继续）

- id: REQ-01M5BK8QMQYK7XQG707YVMQ202
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 0
- tags: [handoff, engine spec]

## 一句话

2026-10-24 交接（第二十四次）：`apps/emulator-new` 已从 SYSTEM4 启动链走到 TITLE（id `0x5264`）并进入输入环；CLI 与 Electron 都支持 `--skip-logo`。**工作树有未提交改动**（长期纪律：不 `git add` / `commit`）。

## 最近修的两个显示 bug（等实机复测）

- **影片 logo 黑噪点**：`uploadTexture` 把 `color: -1`（无键哨兵）当成 `0xFFFFFFFF` 白键 ⇒ 影片白底 / logo 边缘被清成透明。修：`hasColorKey(color) = color > 0`。
- **启动 ~1s 白闪**：① 窗口先 show、画布后才有第一笔（`create` 先清一帧）；② 释放纹理时用旧快照中途 present（`releaseTexture` 不再 present，改在下一帧边界推新快照；`present` 加 `trigger`）。另加空白槽不画 / 影片覆盖层诊断。
- **`plugins/pnpm-priv`** 新增宿主侧转发 `op=run`：`build-renderer` / `smoke` / `electron`（argv/cwd 写死、`write:true`、`installDir` 校验、`allowRun` 总闸）。宿主插件不热重载 ⇒ 重装 + 重启 DSH。

## 残差批次（第三批到第十七批，均已收口）

明细见 `docs/02-engine/spec-audit-2026-10.md` §20–§42。第十七批：负模数（向零截断余数、负模数 ≡ 绝对值）、按住游标（按帧槽号、释放/重载不复位、第一次 `0x104` 必定义 0）、网格混合档（网格**会**设混合；档 0 = alpha 对）。实现：`Machine.holdCursorBySlot` + 溢出分支 + 守卫 4 条；`RES-SCENE-BLEND-TIER-VALUES` 新开。

第十八批：平键序独立复核 —— 四表身份与顺序原文错位；正确序 **辅助表 A → 辅助表 B → 网格 → 绘制项**；`mergeTableKeys` 修正 + typed 守卫。

第十九批：辅助表 A = **Live2D 模型条目表**（按用户要求排除、不展开）；辅助表 B = 3D 网格模型条目表；`0x344` 写 A 而不是绘制项表。

第二十批：辅助表 B 的七矩阵槽收口 —— 合成后只乘世界阶段，三对基/目标槽 + 时间窗，窗内插值/到期折叠；`+0x1FC` 是累积旋转矩阵。

第二十一批：「区间之下」= 给低档不透明离屏画布垫底；纹理 `[20,30)` = 文本层画布带（层号 0..9 自建 20+i）。两条残差关闭，各劈一条后续。

第二十二批：关闭三条零读者/已定残差（表语义、空白标记、0x30a 小表）；纠正 v1=3 的 20,000 B = **纹理记录表**，读侧依赖、不能写 0。

第二十三批：纹理记录表字段逐格定死（5×u32：文件号 / 颜色键 / 重建门 / 零读者标志 / 空白标记）；纠正主体 `+12`/`+52` 副块“可写 0、读侧不得依赖”。

第二十四批：纹理记录表**写侧**落地（`textureRecords` + 1000×20 B，v1=2 不写）；`0x19e` 真路径携带纹理记录 + `flagTableA/B`；4 条 pure 守卫 + 2 条变异。

第二十五批：「真机/采样」29 条分诊 —— 真 UB 9 条标 `discarded`（建议显式报错）、子头变体关闭、像素叠放合并、样本/代码确定项改写为条件合同、输入平台项延后。`unknown` 441→430。

## 当前门禁

```bash
pnpm test                 # 1024 pass / 0 fail
pnpm test:assets          # 203 pass / 0 fail / 1 skip
pnpm test:mutation        # 338/338 按预期变红
pnpm tools spec validate  # 13/13 全绿，未登记 0
pnpm tools requirements validate # 6/6 全绿
pnpm typecheck            # 六工程全绿
```

## 待处理（按优先）

1. **真实绘制 S2+**：离屏画布 36/37 的建 / 切、表面拷贝、逐像素遮罩、`strokes` 送 pixi、效果技法、进度插值、`clearTable` 真实清表。
2. **保存读侧**：`0x1a1` 的负载还原 / 整池重编码；`rec[259]/[260]` 已能写。
3. **`createDevice` 忽略 `backBufferWidth/Height`**；`shaderTier` 映射未取证。
4. **尾扫链优先级 / 平键序命名**：batch17 发现 `448>438>428(网格)>408(绘制项)` 与正文“网格→辅助B→辅助A→绘制项”的名字可能不符，属 `CORE-SCENE-DRAW-ORDER`，需独立复核。
5. 剩余静态项：`RES-SAVE-TRAILER-FIELDS` · `RES-SAVE-STH-SHORTREAD` · `RES-SAVE-SLOT-SPACE` · `RES-SCENE-BLEND-TIER-VALUES` · `RES-SCENE-COMMAND-KIND0-ALPHA` / `-WIPE-BOUNDARY` / `-EFFECT-EQUIVALENCE` · `RES-SCENE-DISPLAY-FIT-SCALE-DIRECTION` · `RES-BOOT-GFX-LOGSINK-PATH-DATA` / `-SINK2` · `RES-TEX-...-OOB` / `-SELFTARGET` / `-BELOW-RANGE`。真源 `pnpm tools spec report`（unknown≈443）。
6. 需要真机 / 真样本：负键绘制项、同键叠放、键总数启动值、32bpp AGF、遮罩极性、自引用/越界、种类 0 合成 alpha、缩放方向、STH 短读。
7. audio / text-system / Live2D 仍按用户要求排除。

## 新会话从这读

- 技能：`amayui-spec`（§9.1 编辑后独立复审）/ `amayui-emulator` / `amayui-re-engine`。
- 进度：`pnpm tools requirements plan`；审计：`pnpm tools spec report`；本批：`docs/02-engine/spec-audit-2026-10.md` §34。
- 机制地图以 `data/spec/core/README.md` 为准：`CORE-ISA` / `CORE-POOL` / `CORE-CONFIG` / `CORE-FRAME` / `CORE-OPS` 等。
- 本单只记交接；具体状态以子单与 `pnpm tools requirements plan` 为准。
