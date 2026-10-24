# 残差第三批：AAI/ALF 索引 · 绘制链 · 池翻页 · 异常三支 · 图形参数

- id: REQ-01M5MPYFP1DCBJT7N0KM66VT8A
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-ops-memflip.test.mjs#★★ 配置非 0 ⇒ 翻页后读写落在另一组缓冲，翻回原值仍在
- tags: [spec residual]

# 残差第三批：AAI/ALF 索引 · 绘制链 · 池翻页 · 异常三支 · 图形参数

## 一句话

把交接单「待处理」里排在最前的残差按**静态取证 → spec 合同 → emulator-new 落地 → 会红的守卫**收口一批；audio / text-system / Live2D 三个模块仍然跳过。

## 已闭合（20 条旧残差）

- 池 / 帧 / 输入 / 异常：`RES-POOL-MEMFLIP` · `RES-FRAME-LOAD-AUX-ARRAYS` · `RES-LOCAL-TARGETS-CONSUMER` · `RES-LOCAL-TARGETS-INIT` · `RES-INPUT-KEYTOTAL-WRITERS` · `RES-VM-EXC-PROPAGATION` · `RES-BOOT-EXC-DIALOG-VALUES` · `RES-BOOT-EXC-RETRY-SCOPE` · `RES-BOOT-EXC-TEXT-CONTEXT` · `RES-ISA-DOMAIN-EXC`
- 索引 / 绘制：`RES-FS-AAI-KEY` · `RES-FS-EXT-LOAD` · `RES-FS-INDEX-AAI-PARTIAL` · `RES-FS-ARCHIVE-AAI-PREFIX` · `RES-ALF-MULTI-INDEX` · `RES-FS-CH-MULTI-INDEX` · `RES-TEX-DRAW-CONSUMER` · `RES-SCENE-DRAW-ORDER-COMMAND`
- 图形：`RES-AGF-CHANNEL-ORDER` · `RES-BOOT-GFX-PARAMS`

## 新写成合同（4 条新规则 + 16 条改写）

- `SPC-POOL-MEMFLIP`（`0x6d`）：六个全局池主 / 备缓冲整体对调；配置键为 0/缺失 ⇒ 显示消息型异常；初始选主组。
- `CORE-FS-EXT-INDEX-KEY`：`*.AAI` = ALF 索引族；头 `+264` 区号；高 8 位按有符号字节；同区号后装载者胜；槽空与越界是不同失败。
- `CORE-OPS-TEXTURE-DRAW-CONSUMER`：有效位 / 空槽上报并跳过 / 夹空静默 / 无渲染目标 / 附加绘制位。
- `CORE-OPS-SCENE-COMMAND-RECORDS`：`0x223` / `0x24d` / `0x24f` / `0x250` / `0x251` 写侧（同键替换、种类、置脏、种类 2 的两条校验后果不同）。
- 改写：`CORE-BOOT-LOOP-EXC`（五类捕获与三支分流）· `CORE-SCENE-DRAW-ORDER`（两档 + 四表收尾扫描 + 负键按表分）· `CORE-INPUT-WORD` / `CORE-INPUT-KEY-DISPATCH`（键总数无初值；`0x100` 读键总数）· `CORE-OPS-FRAME-LOAD-FRAME` / `CORE-OPS-FRAME-LOCAL-SET-7B`（两格 = 本地块进入目标、−1）· `FMT-AGF-PIXELS`（通道 = 输出接口；32bpp 不使用 alpha 段）· `CORE-BOOT-GRAPHICS-INIT`（设备实参表 / 后缓冲三分支 / 两个独立路线位）· `CORE-FS-ID-ZONES` / `CORE-FS-INDEX-STACK` / `CORE-FS-LOOSE-FIRST` / `CORE-FS-ARCHIVE-ROOT` · `FMT-ALF-TOC` · `CORE-VM-LOOP` / `CORE-OPS-ABORT-01` · `CORE-OPS-RENDER-FRAME-STEP`。

## emulator-new 落地

- `0x6d` 两份缓冲 + 配置门；`0x100` 上界改键总数 + UB 诊断；新帧两格 `−1`；AGF 32bpp 只取像素第 4 字节。
- 图形参数全部由配置键推导（PresentInterval 映射 / 虚拟全屏 / 后缓冲三分支 / `display:TextureBit` / MultiSample / 设备建成后的四步）。
- `Host.listDir` + `S?AI` + 扩展索引槽表 + 区号解码 + 名字反查（基础优先、大小写不敏感）。
- 场景命令表 + 五条写侧 handler + 负键按表分（绘制项不再过滤）+ 命令档未建模诊断。

## 守卫

- 新文件：`emulator-new-ops-memflip` / `emulator-new-input-keytotal` / `emulator-new-frame-aux` / `emulator-new-graphics-params` / `emulator-new-aai` / `emulator-new-aai-region.assets` / `emulator-new-scene-commands` / `spec-sites-batch3.assets`；另改 `emulator-new-agf` / `emulator-new-input` / `emulator-new-window` / `emulator-new-scene-order` 的旧断言。
- `tools/mutate-check.mjs` 新增 14 条变异（池翻页 3 / 键总数 2 / 帧格 1 / AGF 1 / 图形参数 3 / 扩展索引 2 / 场景命令 2）。

## 仍开着的（本轮新开 / 收窄）

`RES-POOL-MEMFLIP-DEFAULT` · `RES-EXC-RETRY-FRAME` · `RES-EXC-SKIP-STALE` · `RES-EXC-DISPLAY-KIND` · `RES-EXC-DISPLAY-CATEGORY` · `RES-AGF-ALPHA-32` · `RES-BOOT-GFX-RETRY-ORDER` · `RES-FS-EXT-KEY-RANGE` / `-HDR-FIELDS` / `-TITLE-SOURCE` · `RES-ALF-DUP-NAME` / `-APPEND-ALF-LOCATION` · 纹理消费者 5 条 · 场景绘制 4 条 · 命令记录 4 条。命令档**执行**、`0x1f6` 与命令表、种类 1/3 字段布局仍在残差里（见 `pnpm tools spec report` 与各规则的「残差」节）。

## 判据

- `pnpm tools spec validate` 13/13；`pnpm test` 865 / 0；`pnpm test:assets` 189 / 0（1 skip）；`pnpm typecheck` 六工程绿；`pnpm test:mutation` 见本次收口记录。
- 复审：只读 `data/spec` 的独立 subagent 两遍（S1 9 → 2），第二轮 2 条已修，第三遍跟报见 `docs/02-engine/spec-audit-2026-10.md` §20。

## 怎么查

`pnpm tools spec show <rule>` · `pnpm tools spec report` · `pnpm tools requirements plan` · `tools/test/emulator-new-*.test.mjs` · `docs/02-engine/spec-audit-2026-10.md` §20。
