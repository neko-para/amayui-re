# CTX · mac arm64 上要做的事（2026-11-16 交接）

> 这份是**待办清单**，不是口径文档。口径看 `AGENTS.md` §12 / `apps/emulator-lift/README.md` / `docs/03-emulator/`
> （`input-plan.md` · `observations.md` · `defects.md`）。**进度真源仍是 `pnpm tools requirements plan`**。
> 上一版（2026-11-13）的 P0–P6 待办**不在本文保留**——它们都在需求树里，用上面的命令查。
> 下面所有读数都是 **win32-x64 / RelWithDebInfo** 的实测，**只作 arm64 对照基线**；⛔ 不要跨机器比墙钟。

---

## A. 我请你做的事（只有人能做的 4 条）

1. **切机器前先提交**。工作树现在有 **43 项**未提交改动（`git status --porcelain` 现为 43 行；
   最近提交 `9998381 feat: opt build speed & fixes`）。不提交就得整包搬过去。
2. **跑一次人工对照**（端到端配方只差这一档）：
   ```
   ./out/emulator-lift-aot --skip-logo --present window --max-frames 600 \
     --game-dir <安装树> --appdata <每次全新空目录> --region-log
   ```
   到 TITLE 后**手点一次「Load Data」**，把那次收尾的 **`region` 三数**（`in_use / surfaces / heap_blocks`）发我。
   单：`REQ-01M7GA4B8T9JXFDAYK46YHNH7K`。
3. **`--cursor-warp` 真的挪宿主光标**这件事我没自证过（win32 上只证到状态行 `cursor-warp: on`）。
   跑一次 `--present window --cursor-warp`，看光标有没有被挪——顺便验 mac 分支。
4. **arm64 上重验一次读档路径**（你在 win32 上已确认正常）。

---

## B. arm64 第一条命令序列（先确认地基）

```bash
cd apps/emulator-lift
cmake --preset macos-arm64          # Ninja / RelWithDebInfo
cmake --build --preset macos-arm64
ctest --test-dir out --output-on-failure
cd ../.. && pnpm test && pnpm verify && node tools/requirements.mjs --validate
```

**win32 基线**（arm64 要重新报，别套用）：`ctest` **102/102**（104.9 s）· `pnpm test` **552/552** ·
`pnpm verify` **500/500** · `requirements validate` **6/6** · `git diff --check` exit 0。

⚠ mac 上已知会 SKIP 的：`cli-time-limit` / `cli-dump-frame` / `frame-dump-failure`（Windows 专有）、
`gpu-raster`（dummy 视频建不出 GPU device）。**跳过要如实记成跳过，⛔ 别把"没跑"说成绿。**

---

## C. 逐项回归（本次会话动过的 7 个面）

### C1 · IR 拆 8 份（`LIFT_AOT_SPLIT=8`，仍 `-c -O2`）
- **win32**：构建 228 s → **80.8 s**（lift-gen 58.64 / clang 段 21.63 / 链接 0.54）；
  `objdump -d` 2,613,209（split8）vs 2,619,129（split1）= **−0.226%**；`.ll` 合计 134,075,853 B（8 份 / 16.0–18.1 MB）。
- **arm64 要验**：① 8 份 clang 真的并行；② `ctest -R lift-aot-split-manifest` 过；
  ③ ★ **`.ll` 的 8 份应与 win32 逐字节相同**（IR 是 target-independent）——最便宜也最强的跨架构判据：
  比对 `out/aot/lift_aot.split.txt` 里每份的 `sha256 / bytes / functions`。
  ⇒ **先回 win32 把这份清单存下来**（`out/` 不入库，切机器就没了）。
- 回退：`-DLIFT_AOT_SPLIT=1`（一行，不用回滚源码）。

### C2 · 闭包闸并进生成那一趟（`--closure-gate`）
- **win32**：单独那次 `--check` 的 48.6 s 已并掉；`.ll` / pin / table **逐字节相同**。
- **arm64 要验**：① 产物与 win32 同（用 C1③ 那份清单）；② **坏闭包仍硬红**：在 `.tmp/` 造坏副本
  （⛔ 别动入库件 `aot/closure.txt`）⇒ 构建必须失败并打出同一行汇总。

### C3 · BUG-24 跳表闭包（审计 + 区间排除 + 表项立 leader）
- **win32**：`bb_41B92D` 出现、表首 `bb_41B974` 消失；62 张 `.lst` 表可派发目标 **577 → 605**（缺 28 → 0）；
  关档（`--no-jmptab-exclude --no-close-jmptab-items`）的 `.ll` 与改前**逐字节相同**。
- **arm64 要验**：`ctest -R lift-jmptab-dispatch`（win32 46.95 s）；审计行应满足
  `items_nondispatch=0 · block_starts_in_range=0 · covers_protected=0 · passes=2`。

### C4 · `EnumSystemLocalesA` 续延桩
- **win32**：`locale-ext` 新用例 `enum-locales-no-cache` 修前红（12 failures）→ 修后绿；
  `shim :` 三数 `registered=385 / bound-stub-ea=385 / next-slot=385` **修前修后不变**。
- **arm64 要验**：`ctest -R locale-ext`；以及读档路径真跑（见 A4）。

### C5 · 删掉 AOT 的 `--native-overrides` + 逐帧日志改 opt-in
- **win32**：净删 566 行；缺省 stdout **388 → 85**；`--native-overrides` / `--lst` 是**未知参数 ⇒ exit 2**；指纹逐位同。
- **arm64 要验**：那两个参数仍 exit 2；缺省 stdout 行数；`ctest -R 'aot-arm-shape|cli-aot-flags'`。

### C6 · `region` 表改 opt-in（`--region-log`）
- **win32**：缺省 stdout **85 → 57**；`--region-log` 加回那 28 行且与改前**逐字节相同**。
- **arm64 要验**：两条行数；`ctest -R aot-arm-shape` 的第 ③ 条断言。
- ★ 反讽但重要：`region` 三数是目前**唯一稳定、随屏而变**的屏幕指纹（`OBS-20261116-04`）⇒ **别把这块整段删掉**。

### C7 · 输入投递层（S1 + S4 + S2′）
- **win32**：11 个投递点全部接上；`machine.cpp` / `aot_run.cpp` 里新层 5 个词 **0 命中**；
  指纹逐位同；300 帧交错 A/B 中位 **−0.51%（噪声内）**；`objdump -d` +152 / +169 行；`lift_aot.obj` 0 变化。
- **arm64 要验**：`ctest -R 'input-plan-read-points|cli-aot-flags'` ·
  `node --test --test-isolation=none tools/test/input-plan-shape.test.mjs` ·
  `node tools/mutate-check.mjs --match 'input-plan-shape'`（⛔ 别跑全量）。
- 设计口径：`docs/03-emulator/input-plan.md`（**已复审修正**）。`.tmp/aot/input/design.md` 有错，⛔ 别照它实现。

---

## D. 只在 mac 上才验得到的平台点

1. **`--cursor-warp` 的 mac 分支是存在的**：`src/runtime/host_cursor.cpp` 的 `__APPLE__` 走
   `CGWarpMouseCursorPosition` + `CGAssociateMouseAndMouseCursorPosition`
   （注释写明**不需要辅助功能授权**；warp 后必须复位关联，否则"脚本挪完光标玩家推不动"）。
   ⇒ 期望 `cursor-warp: on`，**不是** `unsupported`（`Unsupported` 只给 Linux / 其它平台）。
2. **HiDPI：point vs pixel**。mac 上窗口尺寸是 **point**；若 Retina 缩放，guest 1280×720 → 屏幕坐标的换算
   可能与 win32 不同。相关纯函数在 `src/runtime/cursor_map.h`（`cursor-map/*` 已钉住 image 尺寸敏感）。
   ⇒ warp 落点偏了先查这一层，别先怀疑输入投递。
3. **`--sound`**：AOT 臂一律"接受 + 一次性说明 + 不出声"（本臂无音频管线，票 `REQ-01M7G7GPBG9BK2360WSG6DBFNP`）。
   mac 上应**仍是这条说明**，不是静默。
4. **资产与安装树**：`links/install` / LFS 资产 / 两份宿主字体要在 mac 上到位；缺了 `assets` 档会跳过。
5. **mac 本机能做 GPU**：`--present window` / `--present hidden` 都能建 Metal device；
   但 `SDL_VIDEODRIVER=dummy` ⇒ 无 GPU ⇒ **拒绝启动**（测试 `LIFT_SKIP(77)`）。`--present headless` = **隐藏窗口**。

---

## E. 已知未做（都已在需求树里，别重复开单）

| 单 | 内容 |
|---|---|
| `REQ-01M7G7GPBG9BK2360WSG6DBFNP` | AOT 音频管线：feeder 在 AOT 臂怎么跑（真出声） |
| `REQ-01M7GA4B8T9JXFDAYK46YHNH7K` | AOT 自动到「加载存档」屏：缺一份经验证的按键/坐标配方 + 人手对照 |
| `REQ-01M7GAMJQ26DHZSPJQTXZ88S7D`（**BUG-26**） | 同一输入两臂结果不同：`click 1070 480` 在 AOT 生效、在解释器无反应 |
| 输入后续（未开单，见父单正文） | `GetMessageA` 空队列由计划供给 · 两臂共有事件 emit · 旧 `harness_script` 路径删除 · `Reads`/`Insns` 门 · 干净的"无动作对照"（`noop` 动作） |

★ **建议优先 BUG-26**：如果两臂对同一输入给出不同结果，那所有"解释器能跑通 / AOT 跑不通"的结论
都可能是**输入路径差异**而不是引擎差异。三个候选原因（两臂 guest 时间步长差约 15× / 解释器 `bounded-no-sample` /
**两臂走了两条不同施加路径**）都还没定。

---

## F. 坑（这次踩过，别再踩）

1. **到 TITLE 的任何自动化都必须带 `--skip-logo`**。不带时逐帧 hash 在 **frame 64** 就冻死
   （`0xBE8BCF5F5F55D3F5` 连续 337 帧）⇒ 之后的按键打在**静止的 boot 帧**上。
   ⚠ 所以之前那份"300 帧指纹逐位相同"里，`lastHash` 其实是**冻结的 boot 帧**，不是标题画面。
2. **判据别选错**：`stop @` 在 `--max-frames` 下两侧恒同（停在 Present 桩）⇒ **无证明力**；
   `lastHash` 每次 run 都不同（real-time + 动画屏）⇒ **不是指纹**；
   **能用的是收尾 `region` 三数**（需 `--region-log`，两侧都要带）。
3. **`--input-plan` 表达不了"同一份计划去掉动作"**（只有门没动作 ⇒ 解析报 `缺动作行`、exit 2）
   ⇒ 对照只能用"不给 plan"或"惰性点击"两档。
4. **⛔ 不跨机器比墙钟**（`AGENTS.md` §12.6：同机 / release / 交错 A/B ≥3 对 / 中位）；本机分辨不了 <5%。
5. **⛔ AOT 臂没有 native override 了**：`src/aot/` 里那套机制的标识符是**禁用词**
   （`tools/test/aot-arm-shape.test.mjs` 钉住 33 个）。
6. **守卫的"红"要分级**：如果红只因**编译不过**（断言用了新增 API），它没证明运行期行为 ⇒ 报告里标"证明力弱"。
7. **需求树只能经 CLI 写**：`node tools/requirements.mjs --set … --write`；⛔ 不手改 `data/requirements/*.md`。
8. **⛔ 不要 `git add` / `commit`**：提交时机由你定（本条长期有效）。
