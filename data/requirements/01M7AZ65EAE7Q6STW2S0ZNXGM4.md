# D3D9 alpha test 整批拒绝（以前 CPU 逐像素做）⇒ 该批一个像素都不画；且 LineCaps.ALPHACMP 仍在声明（违反"不能表达就清位"）

- id: REQ-01M7AZ65EAE7Q6STW2S0ZNXGM4
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/d3d9.cpp#d3d9/caps-claims
- done_reason: 按 2026-11-14 的裁决走备选路（清位 + 说清后果），⛔ 未实现 alpha test：kCapsLineClaim 0x0E → 0x06（不再声明 ALPHACMP，依据是 d3d9.h 已裁决的'不能表达就清 0'）；d3d9/caps-claims ④ 从 caps_bit ⇔ suppressed 的双向锁死改成两件独立事实（caps 里没有 ALPHACMP + ALPHATESTENABLE=1 的 Draw 返回 D3DERR_INVALIDCALL、skipped_draws+1、原因进 skipped_reasons、具名位 bit30），修前红实测：caps 临时改回 0x0E ⇒ expected 0x0, got 0x1（108 checks / 1 failure），改回 0x06 后 0 failures；noteLinePointCapsOnce() 的'声明与行为对齐'改成实话（打印 ZTEST|BLEND(0x06)）；后果写清：docs/03-emulator/defects.md 的 BUG-23 + README 能力表（'整批拒绝 ⇒ 该图层整块消失，不是变半透明'，排障看 unsupported bit30 / skipped top:）。读数依据 OBS-20261114-09：boot→TITLE 600 帧里 GPU 不支持 alpha test 0 次命中；重开实现的条件写在正文（任何真场景出现 bit30 或该原因 ⇒ 改回 bug 并按首选路的两臂判据实现）。
- tags: [emulator-lift, d3d9, gpu, alpha-test, contract]

## 判据（首选：把 alpha test 做出来）

1. `apps/emulator-lift/tests/d3d9.cpp#d3d9/caps-claims` 的 ④ 改成**两臂**：`ALPHATESTENABLE=1 + ALPHAFUNC=NEVER` 的线 ⇒ Draw 返回 `D3D_OK`、`gpuCoverage().queued >= 1`、后缓冲**0 个新像素**（`countNonBackground()` 不变）；对照臂：同一条线 `ALPHAFUNC=ALWAYS` ⇒ `D3D_OK`、`queued >= 1`、**像素出现**（`countNonBackground()` 增加）。⇒ 两臂同时绿 = "alpha test 真的在**逐片元**起作用"，而不是"整批被拒"。
2. `apps/emulator-lift/tests/gpu_batch.cpp#gpu-batch/skip-ledger` 的第二段（`ALPHATESTENABLE=1` 那段）改为：Draw `D3D_OK`、`skipped_draws == 0`、`skipped_reasons` 里**没有** `"GPU 不支持 alpha test"` 这条原因。
3. `kCapsLineClaim` 与行为一致：`d3d9/caps-claims` ①…⑦ 仍全绿（`ALPHACMP` 位与"逐片元挡住"的见证同向）。

## 判据（备选：先承认没实现，只求不再说谎）

1. `src/runtime/d3d9.h:217-219` 的 `kCapsLineClaim` 从 `0x0000000E` → `0x00000006`（去掉 `ALPHACMP(0x08)`），注释改成"ALPHACMP 不声明：alpha test 未实现（整批拒绝）"；
2. `d3d9/caps-claims` ④ 从 `caps_bit ⇔ suppressed` 改成显式断言"caps 里**没有** ALPHACMP，且带 alpha test 的批返回 `D3DERR_INVALIDCALL`"；
3. `src/runtime/d3d9_pipeline.cpp:3735-3750` 的 `noteLinePointCapsOnce()` 那句"LineCaps 的声明现在与行为对齐"必须同步改（它今天已经是假话）。

## 现状证据

- **整批拒绝**（`src/runtime/d3d9_pipeline.cpp:3821-3824`）：`if (ctx.alpha_test) { *why = "GPU 不支持 alpha test（ALPHATESTENABLE=1）"; return deny(kUnGpuAlphaTest); }`（bit30）。它排在准入的**所有后续检查之前**（后于 target 格式、先于 shade / color-write / 纹理级）⇒ **任何**带 `ALPHATESTENABLE=1` 的 Draw 都整批拒（不只是点/线）。
- **拒批后的处置**：`submitPrimitives` 在准入处 `++draw_calls; noteGpuSkip(...); return false`（`:4018-4025`）⇒ Draw 返回 `D3DERR_INVALIDCALL`、`skipped_draws +1`、具名位 bit30 进 `unsupported :`。
- **以前 CPU 会画什么**（`git show HEAD:apps/emulator-lift/src/runtime/d3d9_pipeline.cpp` 的 `:1581-1597`，随 CPU 光栅一起删掉）：8 个比较函数 + `ALPHAREF` 全都实现过，而且是**逐片元**的（只丢被比较掉的片元，其余照画）。
- **caps 今天声明了什么**（`src/runtime/d3d9.h:217-219`）：`kCapsLineClaim = 0x0000000Eu` = bit1 `ZTEST` | bit2 `BLEND` | bit3 `ALPHACMP(0x08)`。
- **仓库已经裁决过**（`src/runtime/d3d9.h:211-215`）："caps 是**声明**，不是实现集；本线只能**响亮拒绝**的能力必须清位 —— caps 没有'拒绝'动作 ⇒ **不能表达就清 0**" ⇒ 今天 `ALPHACMP` 还在，**直接违反已裁决口径**；这不是"语义未定、等产品裁决"，是**已经裁决过、只是没执行**。
- `tests/d3d9.cpp:2688-2692` 的注释自称"语义未定 …… 本单**不猜**"，与上述裁决**冲突**；而且 ④ 的双向蕴含今天把假话**锁死**（caps 一撤位就红）。

## 为什么不是"设计"

1. **它是一次能力回退，不是范围裁剪**：同一批状态在本改动之前由 CPU **逐片元**正确执行（8 个比较函数 + `ALPHAREF`），改动之后**一个像素都不画**。判据：`git show HEAD:` 里那 19 行 vs 今天的 `deny(kUnGpuAlphaTest)` —— 行为从"按 D3D9 语义过滤片元"变成"整批消失"。
2. **caps 这一侧连"未定"都不成立**：`d3d9.h:211-215` 已裁决"不能表达就清 0"，而 `ALPHACMP(0x08)` 今天既没实现、也没清位 ⇒ 违反已裁决口径，不是等裁决。
3. **"语义未定、保持现状"与同一次改动的另一处注释自相矛盾**：`d3d9_pipeline.cpp:3735` 写着"`LineCaps` 的声明现在**与行为对齐**"，`:3744` 又打印"声明 ZTEST|BLEND|ALPHACMP(0x0E)" —— 改动之后前半句是假话。
4. **它会被误归因成"混合没配好"**：整批不画的表现是"图层整块不见"，与 blend 参数错完全不同；而仅有的两个线索（`unsupported` bit30、`skipped top: GPU 不支持 alpha test`）在默认级别只出一次（`pipeWarn` 同原因只打一次）。

## 玩家会看到什么（精确说）

alpha test **不是**混合：它决定"这个片元写不写"。整批被拒 = **整块几何一个像素都不出现**（不是"边缘不对"、也不是"变半透明"）：colorkey 贴图 ⇒ 整张图消失（立绘 / 特效 / 文字块 / UI 边框成片不见）；树叶 / 发丝镂空 ⇒ 整个四边形消失。⚠ 排障：看到"某个图层整块不见"，先查 `unsupported :` 有没有 **bit30**、`skipped top:` 有没有 `GPU 不支持 alpha test` —— 有就不是混合参数配错。

## 两条路的后果（不能互相顶替）

- **清位**：1 行常量 + 注释 + 改 ④ 判据 ⇒ caps 诚实，但**玩家可见问题不修**（该批仍整块不画）；对引擎影响零（实测只读 `caps+0x58/0x5C`，见 `d3d9_pipeline.cpp:3745` + `shims/d3d9.cpp:16`）。
- **实现 alpha test**（建议）：片元着色器加 alpha 比较（`ALPHAFUNC`/`ALPHAREF` 作为每批常量或 pipeline 变体键）+ 缓存键扩位 + `d3d9_gpu.cpp` 的 pipeline 变体 + `gpuEligible` 删掉 bit30 那条闸 ⇒ **修缺陷**。
- 若短期只做清位，必须把"alpha test 的批整块不画"写进 `docs/03-emulator/defects.md`，并在 README 能力表点名（`README.md:748` 今天只把它列在一串能力名里，没有"整批不画 ⇒ 图层消失"的后果描述）。

## 复现

```powershell
cd apps/emulator-lift
cmake --preset windows-x64; cmake --build --preset windows-x64 --target lift-d3d9 lift-gpu-batch
ctest --test-dir out -R 'd3d9-gpu|gpu-batch-order' -V     # 看 ④ 今天的期望是 INVALIDCALL
# 游戏侧读数（决定本单紧急度）：alpha test 批在 boot→TITLE 里出现几次
./out/emulator-lift.exe --appdata ../../.tmp/appdata --game-dir ../../links/install --present headless --native-overrides all --skip-logo --max-frames 600 --log-level info --log-cat d3d9 | tee ../../.tmp/alphatest/replay.log
grep -c 'GPU 不支持 alpha test' ../../.tmp/alphatest/replay.log   # 命中批数（>0 = 真踩到）
grep 'unsupported :' ../../.tmp/alphatest/replay.log              # 应有 bit30
```

## 收口锚点（`verify` 候选）

- `apps/emulator-lift/tests/d3d9.cpp#d3d9/caps-claims` —— CTest `d3d9-gpu`（`CMakeLists.txt:1005-1007`，顶层，pure 档，SKIP 77）。
- `apps/emulator-lift/tests/gpu_batch.cpp#gpu-batch/skip-ledger` —— CTest `gpu-batch-order`（`CMakeLists.txt:803`，顶层、**不依赖 GPU** ⇒ 无 GPU 的机器也跑）。这是"原因文本消失/仍在"的机械锚点。
- 辅助：`apps/emulator-lift/tests/gpu_batch.cpp#gpu-batch/reject-accounting`（CTest `gpu-batch-order`）—— 若选择"实现 alpha test"，它里面 bit30 的那一段要跟着改。
- ⛔ 这两条**今天判据就是"必须拒绝"**（`caps_bit ⇔ suppressed`、`skipped_draws == 1`）⇒ 今天是绿，但它恰好**锁着本单要翻掉的行为**；所以本单 `verify` **留空**（现在写进去等于把"缺陷被锁死"当收口凭据、让下一个人以为已经绿），翻面之后才写 `d3d9/caps-claims`（无 GPU 环境则用 `gpu-batch/skip-ledger`）。

## 未证 / 需实测

- **引擎是否读 `LineCaps`**：未证（已有读数只到"只读 `caps+0x58/0x5C`"）。查法：对 `caps+0x54`（`kCapsLineCaps`，`d3d9.h:179`）在 guest 侧找 `mov ...,[reg+54h]`（`--trace-blocks` + `--profile`），或看 AGE.EXE / AGERC.DLL 的 IAT 有无 `GetDeviceCaps`。
- **boot→TITLE 里 alpha test 批的命中次数**：未测（用上面 `grep -c`；读数为 0 ⇒ 本局不触发，仍是潜在雷）。
- 正文里的 `文件:行` 取自 `.tmp/cpu-migration/tickets.md`（2026-11-14 当前工作树），并发改动会让行号漂。
## 2026-11-14 读数与裁决（父级）
- **读数**：boot→TITLE 600 帧 `GPU 不支持 alpha test` **0 次**、`unsupported :` 只有 bit11（`OBS-20261114-09`）；ADV 段没量到（`sn0000.graph` 停在 GAMESTART 前）⇒ 游戏内命中面未知。
- **裁决：先走备选路（清位 + 说清后果）；实现 alpha test 以"有命中证据"为条件。** 理由：唯一量到的 600 帧里没有可观测后果，而实现它要给片元着色器加变体、扩缓存键、改 pipeline 变体键 —— 为假想需求加复杂度；caps 的 `ALPHACMP` 则是**已经裁决过的假话**（`d3d9.h:211-215`），那一行必须先改。
- **备选路收口**：① `kCapsLineClaim` 去 bit3（`0x0E→0x06`）；② `d3d9/caps-claims` ④ 改成"没有 ALPHACMP + 带 alpha test 的批返回 `D3DERR_INVALIDCALL`"（不再双向锁死假话）；③ `noteLinePointCapsOnce()` 的"声明与行为对齐"改成实话；④ `defects.md` + README 能力表点名"整批不画 ⇒ 该图层整块消失"。
- **重开实现的条件**：任何真场景日志出现 `GPU 不支持 alpha test` 或 `unsupported :` 的 bit30 ⇒ 按"首选"那节的两臂判据实现，并把本单改回 bug。
