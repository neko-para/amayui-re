# 当前实现总览与交接（会话 / 设备继续）

- id: REQ-01M5BK8QMQYK7XQG707YVMQ202
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 0
- tags: [handoff, engine spec]

## 一句话

2026-10-24 交接（第二十五次）：`apps/emulator-new` 已从 SYSTEM4 启动链走到 TITLE（`0x5264`）并进入输入环；`--skip-logo` 可用。**工作树有未提交改动**（长期纪律：不 `git add` / `commit`）。

## 最近完成

### 显示 bug（等实机复测）
- **影片 logo 黑噪点**：`uploadTexture` 把 `color: -1` 当 `0xFFFFFFFF` 白键 ⇒ 白底/边缘透明。修：`hasColorKey(color)=color>0`。
- **启动 ~1s 白闪**：① `create` 先清一帧；② `releaseTexture` 不再中途 present，改下一帧边界推新快照；`present` 加 `trigger` 诊断；空白槽不画。
- **`plugins/pnpm-priv`** 新增宿主侧 `op=run`：`build-renderer` / `smoke` / `electron`（argv/cwd 写死、`write:true`、`installDir` 校验、`allowRun` 总闸）。宿主插件不热重载 ⇒ 重装 + 重启 DSH。

### 残差 / spec
- 第三批到第二十六批已收口，审计见 `docs/02-engine/spec-audit-2026-10.md` §20–§44。
- 第二十六批：关闭四条“代码确定、只缺观测”残差（帧栈 / 异常重试 / 窗口清理 / 退出码）与 `RES-SAVE-STH-CAPTURE`；修正 `0x3` / `0x6` 装载失败与重试合同（打开失败 = 类别 7、当前帧停在目标槽；头坏 = 退出）；STH 生效实现 = 屏幕捕捉、恒 24 位。
- “真机/采样”29 条已分诊：真 UB 标 `discarded`（建议显式报错），子头变体关闭，像素叠放合并，样本/代码确定项改写为条件合同，输入平台项延后。
- `unknown` 残差 **426**；`discarded` 11。

### S2（命令档真实绘制）
- **S2a-1 完成**：`scene-command-plan` 阶段带表身份 `entries {key,table}`；新增 `scene-command-exec.ts` 纯执行层（默认 `enabled=false` 零 op）；9 条 pure 守卫 + 3 条变异。
- **S2a-2 完成**：`pixi-host` 独立画布 36/37、`emitEntries`、`present(frame)` 先执行 op 再收尾扫描；`commandExecKinds` 缺省空 ⇒ 产品零回退；6 条 pure 守卫 + 3 条变异。
- **S2a-3 完成**：`Machine.stampCommands()` 与 `finishRenderStep()`，session 在快照前盖章、present 后收尾；守卫 +2、变异 +2。
- **S2b-2 完成**：`advanceCommands`（种类 0/3 的 animating/duration、种类 2 mode<0 的 forcedComplete）+ 进度读 forcedComplete；守卫 +2、变异 +2。
- **S2c 完成（结构）**：种类 2 strokes 接到执行层与 Pixi；种类 1 遮罩（rect/flag/mask 投影 + 均匀 alpha；影片结束补反值已接，判据=对象槽空）。
- **S2d 记账**：种类 0/3 走退路时记 `scene-command-effect-unmodeled`；效果技法本身未实现。
- **STH 边界完成**：`core/bmp.ts` 组装 / 解析 BMP，平台只给 `SurfacePixels`；`0x1ae` 先截断建空文件，`0x1af` 先拒 0/1/2；真样本 decode→encode 逐字节可逆。
- **0x3/0x6 状态**：打开失败当前帧停在目标槽 / 子槽、抛类别 7；深度类别 1；运行器不自动重试。
- **S2 开闸入口**：`?commandExecKinds=0,1,2` 经 `parseCommandExecKinds` 传给 `PixiRenderHost`；默认零回退。
- **构建 / smoke**：`pnpm_priv run build-renderer` + `smoke` 均退出 0；截图 `.tmp/emulator-new-electron-smoke.png` 与 demo 场景一致。
- **Electron 开闸参数**：`--command-exec-kinds 0,1,2` ⇒ window-host 拼 renderer URL 查询串；无参数行为不变。
- **存档读侧信封 + 负载结构 + 应用**：`0x1a1` 解析、三池清零 / 按当前 key 重编码、按帧记录重建帧槽（loadScript + tableB/tableA 位置）；解析失败不写 op1。核心 advanceCommands / forcedComplete 已接；效果技法未做。

## 当前门禁

```bash
pnpm test                 # 1068 pass / 0 fail
pnpm test:assets          # 204 pass / 0 fail / 1 skip
pnpm test:mutation        # 361/361 按预期变红
pnpm tools spec validate  # 13/13 全绿，未登记 0
pnpm tools requirements validate # 6/6 全绿
pnpm typecheck            # 六工程全绿
```

## 待处理（按优先）

1. **保存读侧**：`0x1a1` 还原 / 整池重编码；`RES-SAVE-PREAMBLE-BLOCKS` / `RESTORE-PREFREE`。
2. **S2c**：种类 1 遮罩 + 种类 2 strokes；S2d 效果技法。
3. **S2c**：种类 1 遮罩 + 种类 2 strokes 落笔；S2d：效果技法（暂无接口）+ 真实 `clearTable`。
4. **核心写回**：盖章 / 位 16 / 强制完成 / 有动画 / 清表（现全无写者）。
5. **保存读侧**：`0x1a1` 还原 / 整池重编码；v1=3 纹理记录表读侧；STH 边界（core 组装 BMP）。
6. **0x3/0x6 装载失败语义落地**（spec 已改，emulator 未改）：新单 `0x3/0x6 装载失败与重试语义落地`。
7. **STH 边界改造**：新单 `缩略图 STH：BMP 容器搬回 core（实现边界）`。
8. 其它剩余：`RES-SAVE-PREAMBLE-BLOCKS` / `RESTORE-PREFREE`、辅助表 B 领域字段、真实绘制像素采样。
9. audio / text-system / Live2D 仍按用户要求排除。

## 新会话从这读

- 技能：`amayui-spec` / `amayui-emulator` / `amayui-re-engine`。
- 进度：`pnpm tools requirements plan`；审计：`pnpm tools spec report`；本批：`docs/02-engine/spec-audit-2026-10.md` §44。
- S2 设计：`.tmp/spec-draft/batch26/s2/{slice-plan.md,impl-notes.md}`。
- 机制地图以 `data/spec/core/README.md` 为准：`CORE-ISA` / `CORE-POOL` / `CORE-CONFIG` / `CORE-FRAME` / `CORE-OPS` 等。
