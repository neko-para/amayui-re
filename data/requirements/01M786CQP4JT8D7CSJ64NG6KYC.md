# Windows 慢机：assets 档 e2e 固定超时太短 ⇒ save-graph / agerc-ui-state / feeder-seg-e2e 红，close-confirm & quit-graph 偶发

- id: REQ-01M786CQP4JT8D7CSJ64NG6KYC
- type: bug
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: apps/emulator-lift/tests/save_graph.cpp#save-graph
- severity: S2
- tags: [emulator-lift windows test-timeout]

## 现象

本机（Windows 11，RelWithDebInfo，HEAD `fe3cd30`）全量 `ctest` **93 项：首轮 4 红**；
其中 `close-confirm`、`quit-graph` **复跑通过**（偶发）。

## 证据（`out-win`，逐条可复跑）

- **`agerc-ui-state`**：`marker-timeout: waited=TITLE.BIN last=SYSTEM4.BIN waited_ms=30000`
  ⇒ 子进程 30 s 只走到 `SYSTEM4.BIN`。
- **`feeder-seg-e2e`**：子进程 `detail: interaction: timeout · 节点 step3 等 script GAMESTART 超过 30000 ms
  （frame 1..12, insn 156457266..159246974）` ⇒ **30 s 只 Present 12 帧（≈0.4 fps）**。
- **`save-graph`**：`no-sample · step8 等 script FELLOW 超过 5000 ms（frame 1009..1009,
  insn 326714236..395121279）` ⇒ 窗口内 **0 次 Present**，却执行了 **6840 万条指令** ⇒ 引擎在**同步长活**里。
  同类先例：`REQ-01M72GW002QVRSG5J5WJ0KB1KF` 记过"退出时同步写存档 ⇒ 4.1 s 无 Present"。
- 共同点：三条都用 **`--raster cpu`（software 光栅）+ 真窗口** 跑交互图。

## 诊断

1. `agerc-ui-state` / `feeder-seg-e2e`：**慢机 + `--raster cpu`** ⇒ 帧率 ~0.4 fps ⇒ 固定 30 s 的 marker 窗口不够。
2. `save-graph`：帧"停住"**不是慢**，是引擎在同步长活里**不 Present**；5 s 的节点窗口在慢机上不够。
3. 两条 `done` 单的 anchor 在本机**偶发**（`close-confirm` / `quit-graph`）⇒ 也属"固定超时 vs 慢机"。

## 影响面（★ 要点）

| 测试 | 挂在哪张单 | 本机结果 |
|---|---|---|
| `save-graph` | `REQ-01M77ZJYSPX5V21QEB89W51815`（**done**）的 `verify` | **红** |
| `quit-graph` | `REQ-01M77MC1374HXC2JR7E0ZYSHQJ`（**done**）的 `verify` | 首轮红、复跑绿（**偶发**） |
| `close-confirm` | `REQ-01M72JFZ87DJNT52DW4HB8SEV6`（**done**）的 `verify` | 首轮红、复跑绿（**偶发**） |
| `agerc-ui-state` | **没有挂在任何单上** | **红** |
| `feeder-seg-e2e` | **没有挂在任何单上** | **红** |

⇒ 这三张 done 单在**这台 Windows 机上无法复现它们的 verify**；而按仓库口径（`AGENTS.md` §2 **win32 优先**），
"done" 的判据在这台机器上应当成立。

## 要做（三选一，⛔ 不许放宽断言）

1. **超时按实测帧率自适应**：先量 1 s 的帧数，把节点 timeout 定成 `N 帧 / k·秒`，而不是写死 5000/30000 ms；
2. **给 `--raster cpu` 的 e2e 加能力闸门**：GPU 可用时用 `--raster gpu`，否则 `LIFT_SKIP`
   （skip 原因必须进输出，且写清"慢机跳过"）；
3. **降级为本机可跑**：把这三条标成 `@env assets` 的"本机可跑"，并在对应单里注明
   **"不允许作为唯一证据"**（`AGENTS.md` §11 对冻结点已有同类口径）。

## 判据

- 任一方案下，本机全量 `ctest` **93/93 绿**（或明确 `SKIP` 且理由在输出里）；
- ⛔ 不许改 `save-graph` / `agerc-ui-state` / `feeder-seg-e2e` 里的**语义断言**来变绿；
- 若选方案 3，必须同时把"本机跑不了"写进那三张 done 单的正文（⛔ 不静默降级）。

## 另外两条与读数有关

- 本轮 `quit-graph` 的 A/B 双臂都正常（`stop : stopped @ eip=004EE0EA` + `ExitProcess`）⇒ 该单的语义没问题，是**超时**问题。
- `feeder-seg-e2e` 的 `bov=` 已经在涨（`首条 bov=86 → 末条 bov=4576`）⇒ 段级 override **在 feeder 臂上确实生效**，
  失败点只是交互图的 30 s 窗口 —— 别把它误读成"段级没生效"。

## 状态（2026-11-13 · 本机 macOS，按用户口径暂不投）
- **诊断层过时**：本单写"三条都用 `--raster cpu`"；2026-11-13 复核：`tests/save_graph.cpp:184`、`tests/quit_graph.cpp:185-186`
  已是 **gpu**；仍用 cpu 的是 `agerc_ui_state.cpp:155` / `close_confirm.cpp:168` / `feeder_seg_e2e.cpp:211`（另有 `mip_e2e.cpp` 两处）。
- **根因归子单**：三条 cpu-raster 的红由 `REQ-01M78BF1FNNH6K8N8GEY6GQYXX`（退休 CPU 光栅、用例改 gpu）一并解决；
  该子单 `REQ-01M786ZPJX7S8N5YC9Q9JWTAH3` 已 dropped 并入父单。
- **4f1bc7e 把 step8 的 5 s 提到 60 s**：与本单"⛔ 不许调大超时"冲突 ⇒ 这条留给 Windows 机器上按实测帧率裁决
  （自适应 or 承认 60 s 是余量），本机不复现。
- **处置**：只在本机（macOS）不投；等 Windows 机器，届时按上面三条重写判据。
