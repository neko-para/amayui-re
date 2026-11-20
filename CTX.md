# CTX — 交接快照（2026-11-21 19:05，macOS arm64 · 单树 main）

> 本文件只在"换设备 / 看交接"时重建；平时不维护。真源仍是 git + `docs/` + 需求树
> （`pnpm tools requirements plan`）。
> ★ 本版与 2026-11-20 10:40 那版**不是同一台设备**：那版是 win32 双工作树（`main` + `dev`）；
> 本机是 **macOS arm64、只有 `main` 一棵树**，没有 `dev` checkout。

## 0. 环境 / HEAD / 构建树

| 项 | 值 |
|---|---|
| 设备 | macOS arm64（Apple clang / Metal） |
| 工作树 | 只有 `main`：`/Users/nekosu/Documents/Projects/amayui-re`；⛔ 没有 `dev` |
| HEAD | `f69d70a`（+ 本 CTX 提交） |
| 构建树 | `apps/emulator-lift/out`（**唯一**；RelWithDebInfo；`--preset macos-arm64`） |
| 正在跑 | 无（`.tmp` 引用盘点已完成；结果见 §6） |

## 1. 门禁现状（上次全量，2026-11-21）

- `ctest --test-dir apps/emulator-lift/out`：**134 条 = 131 绿 + 2 skip + 1 红（已知偶发）**
  - skip：`gpu-raster`（mac）· `frame-dump-failure`（MSVC `<io.h>`，POSIX 没补）
  - 红：`plugin-headless` → `plugin/load-save-e2e` 第二道确认 gate timeout
    （`REQ-01M7W2X81F1ASB488EJFQGMQSH`，S2；累计 **6 红 2 绿**，用**改动前 exe** 也红 ⇒ 与 Phase A 无关）
- `pnpm test` **604/604** · `pnpm verify` 500 pass / 0 fail / 2 skip · `requirements --validate` **6/6**
- 需求树：**74 活节点**（doing 20 · open 54 · 未收口缺陷 14）

## 2. 最近提交（本轮全部）

| 提交 | 内容 |
|---|---|
| `f69d70a` | `.tmp` 引用收口：`clean-appdata.sh` / `ui-frames.md` 入库，present-readback 改符号锚 |
| `4d29765` | 测试基建 P1–P5 立单（母单 + 5 子单） |
| `befc1f3` | drop `VB0CWG9X`（M8 旧仓收尾，项目形态已变） |
| `0e0f8e1` | 插件 flake 标题订正（6 红 2 绿） |
| `e75dd04` | 关 present-readback triage；Phase B 改挂；flake 证据 |
| `ca0b877` | **Phase A 落地**（needs 谓词 + dump 拒写旧帧 + 惰性 lastHash） |
| `7f108ac` | 关 6 条旧单 + Phase B/两个发现立单 |
| `4062f05` | **P1 跨平台 subprocess**（`runChild` + 自检 + 3 处迁移） |
| `87805fb` | C6 Vfs 负缓存读数归档 |

## 3. 线状态

### 3.1 AOT / 性能（用户目标 = SAVE05 移动窗口的卡顿）

- 已落地：P `793826b`（boundary −8.9% ns/派发）· A′ `fbe4760`（−5.02%，6/6 同向）·
  C1 `b48fdad`（8 尖峰 959→805 ms，−16%）· C6 `7f47235`（hidden 8 事件 −7.8%，`CreateFileA` 151→38 µs）·
  诊断 `8b015f6`。
- 基线（hidden，mac）：SAVE05 `s05move` 195 帧 / 7.59 s；Present **8.8 ms/帧**
  （readback 7.93 · hash 0.748 · sink 0.117）；lift 24.3% · boundary 21.2% · shim 48.1%。
- 口径：主判据 = **每派发 ns**（wait-closed 窗口）；指纹 = `exit` + `stop @ EA` + `frames` +
  arena 三元组 + `table misses`（`insns`/`lastHash` 不作判据）。
- ⚠ win32 的 6–8x 与 SAVE05 卡顿**未在本机复核**；换回 win32 先跑
  `docs/03-emulator/perf-bench-save05.md` 的配方（hidden 两档同一命令）。

### 3.2 Present 回读（Phase A 完成 / Phase B 待做）

- **Phase A 已落地** `ca0b877`：needs 谓词（renderer sink / `--dump-frame` / freeze / sampler /
  `--frame-log|--profile` / Info frameStats）· dump 拒写旧帧 · 惰性 `lastHash` · `LIFT_PRESENT_EAGER=1` 回退。
  守卫 `present-readback` **7 case / 84 checks**；复审 **9/9 PASS**（W1/W3/W4 已修）。
- A/B（无仪器 600 帧 3 对）：19.15 → 18.82 s = **−1.7%，低于本机 5% 分辨力，不宣称收益**；
  `readback` **未归零**（`Clear` 下一帧重新材质化 ⇒ 成本从 Present 迁到 Clear）。
- **Phase B 待做** `REQ-01M7W2FR9EJEMG2QDMZNG6N7WV`（`Clear` 走 GPU 侧 + guest 后缓冲降为派生）。
  动工前先量三处 `<待核>`：`!dirty`/upload 帧占比 · 引擎是否用 rect `Clear` · CPU 回退是否逐帧触发。
- 文档：`docs/03-emulator/present-readback.md`。

### 3.3 插件运行时（C0–C2c done / C3–C5 open）

- 已入库：C2b-2 `68ac4ae` · C2c `6dc3543`/`23ba007` · docs `da3d845`。父单
  `REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY`（正文已刷新：C2c 已完成，剩 C3/C4/C5）。
- 已知 flake 见 §1（独立缺陷单，未 skip、未改）。

### 3.4 测试基建（P1 done / P2–P5 open）

- P1 `4062f05`：`tests/support/subprocess`（Win32 `CreateProcessW` / POSIX `fork+execv`、文件捕获、
  超时 991、argv UTF-8）+ `support-child-proc`（37 checks）+ 迁移 `plugin_e2e` / `cli_aot_flags` /
  `aot_audio_wiring`（mac 真跑 329 / 13 checks）。
- 母单 `REQ-01M7W7RSC34YJTNMK3ESYVXZY2`；P2 `…SCMK7`（7 处 spawn）· P3 `…SCW4Z`（temp/appdata/
  saves/paths/skip）· P4 `…SD3VJ`（BMP/pixel）· P5 `…SDATY`（注册/死件）。
- 裁决 D1–D5 在母单正文（后端 std/POSIX；`check.h` 不搬；`plugin/load-save-e2e` 不拆；saves 显式列表；
  stress 负控留 `LIFT_STRESS`）。

### 3.5 需求树清理（本轮）

- 关 6：`G8XBDTX7` / `6ZPBD7TE` / `AMXBANYT` / `XQ9TTXPG` / `DXYZM3CJ` / `2ZEKJHHQ`。
- drop `VB0CWG9X`；刷新 `9J4T1YPS`（win32 已验，Linux 留 open，用户裁决先不动）与 `WW0HH3HY`。
- 新单：Phase B · `requirements` 位置参数静默退回 plan（`REQ-01M7W2WKFA21NJ0YRYRP950V5X`，S3）·
  插件 flake（S2）。审计报告 `.tmp/req-audit/report.md`（临时区；结论已进单）。

## 4. 下一步（按顺序）

1. **P2**：7 处 spawn 迁 `runChild`（一文件一 commit；判绿见单）。
2. **plugin flake 10 连跑**：独立窗口、不抢 CPU；仍偶发再二分「30 s gate」/「C6 负缓存」。
3. **Phase B**：量三处 `<待核>` + 用户点头「放弃帧边界权威」后再动。
4. **win32 复核**（换设备时）：全量 ctest + SAVE05 / 6–8x + `AMXBANYT` 的 win32 读数。

## 5. 常用命令

```bash
cd apps/emulator-lift
cmake --preset macos-arm64 && cmake --build --preset macos-arm64
ctest --test-dir out --output-on-failure            # 134 条；已知 1 红见 §1
ctest --test-dir out -R 'present-readback|aot-overlay'

cd ../.. && pnpm test && pnpm verify
node tools/requirements.mjs --plan
node tools/mutate-check.mjs --match present-readback   # 只跑这一条（⛔ 不跑全量）
```

- 需求写入只走 `--set` / `--add`（**位置参数形式会静默退回 plan**，见 `REQ-01M7W2WKFA…`）。

## 6. `.tmp` 现状与"只在 `.tmp` 里"的引用（2026-11-21 盘点）

- **已搬进入库位置（本轮）**：
  - `apps/emulator-lift/scripts/clean-appdata.sh`（原 `.tmp/save05-bench/clean-appdata.sh`；去掉硬编码
    `/Users/nekosu`，仓库根由 `git rev-parse` 解析）。
  - `docs/03-emulator/ui-frames.md`（原 `.tmp/ui-qjs/frames-research.md`；C1–C14 / U1–U8 的语义真源，
    7 处引用已改指）。
  - `docs/03-emulator/present-readback.md` 的消费者 / 7 个读点段已改成**符号锚**（原唯一出处是
    `.tmp/present-readback/evaluation.md` §1-2）。
  - `REQ-01M7M1C59PJ2M4MMF69J4T1YPS` 的 Linux LTO 判据改内联 `llvm-objdump` 命令
    （原引用的 `.tmp/lto/measure.mjs` 已不存在 ⇒ 判据原本无法执行）。
  - `docs/03-emulator/perf-bench-save05.md` 的 hidden 档读数已补（`hpa1` 195 帧；原先写"没有读数"）。
- **证据型（结论已进单/文档，文件仍留作当时读数）**：`.tmp/present-readback/` · `.tmp/save05-bench/` ·
  `.tmp/perf-reassess/` · `.tmp/test-infra/` · `.tmp/req-audit/` · `.tmp/ctx-audit/`。
- **运行时派生区（删了无害）**：`.tmp/ui-bake/` · `.tmp/emu-ctl/` · `.tmp/mutate-check/` ·
  `.tmp/lift-plugin-load-save-*`（e2e 私有 appdata 遗留）· `.tmp/appdata/`。
- **盘点全文**：`.tmp/ctx-audit/report.md`（989 处引用 / 95 处现存 / 808 处指向已不存在的 win32 时代路径）。
  A 类 2 条已清零；剩余 4 条不确定项（`deleteobject-profile/report.md` 的竞态读数、
  `plugin-runtime/c2c-decisions.md` 的 D-C2C-2…6、`perf-reassess/p2-design.md`、`overlay-profile/slice3-report.md`）
  见报告 §4 —— 清 `.tmp` 前若要保留，按报告动作 ② 摘进对应文档。
- 另：`src/plugin/*` 等 24 处注释指向**已不存在**的 `.tmp/plugin-runtime/{c1-design-review,c2-decisions,c2-survey}.md`
  （施工图丢失，不影响构建）—— 见报告 §3；要补就把"为什么"写回 `docs/03-emulator/plugin-runtime.md`。
