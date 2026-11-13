# 停用 --raster cpu：实测 ~0.5 fps 无可观测价值，且被三个资产档 CTest 踩到（改 gpu 或 skip，不许调大超时）

- id: REQ-01M786ZPJX7S8N5YC9Q9JWTAH3
- type: req
- status: open
- parent: REQ-01M786CQP4JT8D7CSJ64NG6KYC
- tags: [emulator-lift present raster]

## 用户裁决（2026-11-12）

**`--raster cpu` 停用。** 理由：这个开关过慢，只能证明"非常初始的渲染"，**现在没有任何价值，还会不停地被踩到**。

## 实测证据（Windows 11，RelWithDebInfo，HEAD `fe3cd30`）

- 1280×720、`--present window --present-hidden` 下，`--raster cpu` 只有 **~0.5 fps**：
  `graphs/se-eof.graph` 的 30 s 窗口内只出 **12–16 帧** ⇒ `detail: 节点 step3 等 script GAMESTART 超过 30000 ms`。
- 对照 `--raster gpu`：同一图 **600 帧 / 27 s**、`pres: sink_max=3.3 ms`。
- `--raster cpu` 下 `tokenhold main max` 直接变成 **394–486 ms**（`holdhist >50ms` 123/121 次）：
  软件光栅是**一次几百 ms 的单个 shim、没有让出点** ⇒ 读数反映光栅实现，不是要观察的东西。

## ★ 它踩到的用例（按 2026-11-12 实测，`out-win` 全量 93 项 + 定向复跑）

| 测试 | `--raster` | 结果 | 失败签名 |
|---|---|---|---|
| `close-confirm` | **cpu** | **稳定红（2/2）** | 见该测试自己的日志 |
| `agerc-ui-state` | **cpu** | **稳定红（2/2）** | `marker-timeout: waited=TITLE.BIN last=SYSTEM4.BIN waited_ms=30000` |
| `feeder-seg-e2e` | **cpu** | **稳定红（2/2）** | 子进程 `interaction: timeout`（`step3 等 GAMESTART` 30 s、仅 12–16 帧） |
| `save-graph` | **gpu** | 稳定红 | ⛔ **与本单无关**（`no-sample`：帧停在 1009、5 s 内 0 次 Present、6840 万条指令）⇒ 归 `REQ-01M786CQP4JT8D7CSJ64NG6KYC` |
| `quit-graph` | 未指定（默认 gpu） | 偶发（1 红 1 绿） | 与 raster 无关 |

⇒ **三条 cpu-raster 的用例全红**；唯一 gpu 的 `save-graph` 是另一种原因（同步长活不 Present）。

## 要做

1. **拒绝而不是静默变慢**：传 `--raster cpu` 且未给逃生开关 ⇒ **启动即报错退出**并打一行
   "已停用（实测 ~0.5 fps，无可观测价值）；需要历史对照请显式加 `--allow-slow-cpu-raster`"。
2. **清掉依赖它的用例**：`close-confirm` / `agerc-ui-state` / `feeder-seg-e2e` 改 `--raster gpu`；
   若 GPU 不可用 ⇒ `LIFT_SKIP("无 GPU 直出路径")`（理由必须进输出）。⛔ 不许靠调大超时过。
3. **保留能力但降级**：先做 1（留逃生口），观察一周无人使用再把整条 CPU 光栅路径删掉。
4. **清掉临时构建树**：`out-ftol` / `out-mem` / `out-m0` / `out-m01` / `out-tok` / `out-huff` / `out-rep` / `out-inst` /
   `out-verify` / `out-win` 都是历史并发期的产物（`.gitignore` 的 `out-*/` 里）⇒ **本轮起统一用 `apps/emulator-lift/out`**，
   这些目录可以删。

## 判据

- 传 `--raster cpu` 未给逃生开关 ⇒ **退出码非 0 + 一行明确理由**（不再静默慢跑）。
- 三个依赖它的 CTest 在 `--raster gpu` 下**绿**（或无 GPU 时明确 SKIP）。
- ⛔ 不许改这三个测试的语义断言来变绿。

## 关联

- `REQ-01M786CQP4JT8D7CSJ64NG6KYC`：只剩 `save-graph`（gpu、同步长活）与 `quit-graph`（偶发）两件事；
  三条 cpu-raster 的红的**根因在本单**。
