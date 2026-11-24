# AOT LTO 在 Linux 上未验证（win32 已于 2026-11-18 验：默认 ON，lift_tick 调用 104,750→0）

- id: REQ-01M7M1C59PJ2M4MMF69J4T1YPS
- type: spike
- status: dropped
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- dropped_reason: Linux 不在本仓平台范围：无 Linux CI；本机仅 WSL2，缺无后缀 clang/clang++/ld.lld 与 vcpkg x64-linux 依赖（9 port 现编），/mnt/e 余 23 GB；且 linux-x64 预设用 gcc，与 clang 产出的 LLVM bitcode + -flto 结构性混链。按 AGENTS.md §2（win32 优先、mac 只做兼容性验证）保持默认 OFF（显式 ON 仍可用、configure 有 WARNING）。重开条件：出现 Linux 设备/CI。
- tags: [emulator-lift, lto, linux]

## 结论（2026-11-24 裁决：dropped）

Linux 的 `LIFT_AOT_LTO=ON` **不在本仓的平台上验证**：仓库没有任何 Linux CI；本机（win32）只有 WSL2
Ubuntu 22.04，而 WSL 侧没有这条构建链（无无后缀 `clang` / `clang++` / `ld.lld`；`vcpkg_installed/x64-linux`
不存在，9 个 port 要现场编；`/mnt/e` 只剩 23 GB）。按 `AGENTS.md` §2（win32 优先、mac 只做兼容性验证），
Linux 保持**默认 OFF**；显式 `-DLIFT_AOT_LTO=ON` 仍是"未验证"路径（configure 有 WARNING，不拦）。

## 为什么不是"没做"（2026-11-24 只读盘点）

- `linux-x64` 预设用系统 **gcc**（`CMakePresets.json` 不钉编译器），而 AOT `.obj` 是 clang 产的
  LLVM bitcode、`lift_abi.cpp` 用项目 CXX 带 `-flto` ⇒ gcc 下是 GIMPLE 字节码，**结构性混链**；
  要 ON 必须同时换 clang/lld —— 那是一次从未构建过的新平台 bring-up，不是"跑一遍预设"。
- 判据 2 第 3 条的命令 `bl.*_lift_tick` 是 **mac arm64 专用**；x86-64 上应写 `call.*_lift_tick`
  （照抄会恒得 0，与"真内联"长得一样）。
- 判据 2 第 4 条（60 帧指纹）要 GPU；`--present headless` 无 GPU 也拒绝启动。
- 构建树口径：`linux-x64` 的 `binaryDir` 也是 `out/`，直接按预设 configure 会把 Windows 树改成
  Linux 配置 ⇒ Linux 实验必须另给 `-B`，与 `AGENTS.md` §12.3「构建树只有一个」的字面冲突。

## 重开条件（若将来有 Linux 设备 / CI）

按原来的 1–3 条判据做；判据 2c 改成 `call.*_lift_tick`；判据 2d 若在无 GPU 环境不可执行，就把
"Linux 默认改 ON"的证据限定为 1–3 并写明。重开前先重跑本节的工具链盘点（`wsl -l -v` + `which`）。

## 现状（2026-11-24 / win32）

- `wsl -l -v`：`ubuntu2204` / `v2` / `Stopped`；内核 6.18.58.1；`cmake 3.31.8` / `ninja 1.13.0` /
  `g++ 13.1.0`；`clang` / `clang++` / `ld.lld` / `llvm-objdump` 只有带版本号（`-17` / `-22`）的副本。
- 仓库无 Linux CI：`.github/`、`ci/`、azure/gitlab/circle/jenkins/appveyor/buildkite/drone 全无。
- `/mnt/e` 954 G / 已用 932 G / 可用 **23 G**；`apps/emulator-lift/out` 2.9 GB（Windows 树）。
- win32 已于 2026-11-18 验：默认 ON，`lift_tick` 104,750 → 0、`__text` −3.70%、
  窗口墙钟 −9.25%（5/5）、ctest 107/107（`docs/03-emulator/aot-lto-evaluation.md` §12）。
