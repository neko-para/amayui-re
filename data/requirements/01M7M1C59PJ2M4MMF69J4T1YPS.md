# AOT LTO 在 Linux 上未验证（win32 已于 2026-11-18 验：默认 ON，lift_tick 调用 104,750→0）

- id: REQ-01M7M1C59PJ2M4MMF69J4T1YPS
- type: spike
- status: open
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, lto, linux]

## 要做什么

在 **Linux** 上验一次 `-DLIFT_AOT_LTO=ON` 能不能真跑起来、真内联（mac / win32 已实测，见兄弟单与下节）：

1. 构建：`cmake --preset linux-x64 -DLIFT_AOT_LTO=ON`（GNU ld / ld.lld，必要时 `-fuse-ld=lld`）；
   记墙钟、链接峰值内存、shape 串。
2. 判据（与 mac 同一口径，⛔ 不接受"shape 串写着 -lto-abi 所以开了"）：
   - `lift-aot-ir-target-attrs` 绿（生成物属性与运行库对齐）；
   - `file out/aot/lift_aot.part00.obj` 是 LLVM bitcode；
   - `node .tmp/lto/measure.mjs <binary>` 的 `_lift_tick` 站点相对 OFF 档显著下降（mac 是 −86.4%）；
     ⛔ 若站点不降 ⇒ 链接器静默没跑 LTO 后端 ⇒ 记失败，保持默认 OFF；
   - 60 帧语义指纹与 OFF 档逐位相同。
3. 通过 ⇒ 把 Linux 的默认值改成 ON，并同步 `AGENTS.md` §12.8 与 `docs/03-emulator/aot-lto-evaluation.md`
   的平台范围；不通过 ⇒ 在本单写清失败命令 / 报错 / 结论。

## 现状（2026-11-21）

- **win32 已验（2026-11-18）**：默认 ON（`CMakeLists.txt:413`）；`/Ob2` 只对 `lift_abi.cpp` 后
  `lift_tick` 调用 **104,750 → 0**、`lift_note_write` **39,075 → 0**、`__text` **−3.70%**、
  窗口墙钟 **−9.25%**（5/5 同向）、ctest 107/107 —— 读数在 `docs/03-emulator/aot-lto-evaluation.md` §12。
- **Linux 未验**：本机是 mac（一仓一树），Linux 构建与运行都不在场；`AGENTS.md` §2 只承诺
  win32 优先 / mac 兼容 ⇒ Linux 是"要不要做"的问题，不是"做没做"。
- 本单剩余 = 要么在 Linux 上跑一次 ON 档（判据同上），要么由父级裁决「不做 Linux」后 dropped。
