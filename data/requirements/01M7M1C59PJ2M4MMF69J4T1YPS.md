# AOT LTO 在 win32 / Linux 上未验证（默认 OFF，显式 ON 未跑过）

- id: REQ-01M7M1C59PJ2M4MMF69J4T1YPS
- type: spike
- status: open
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, lto, win32]

## 要做什么

在 win32 与 Linux 上各验一次 `-DLIFT_AOT_LTO=ON` 能不能真跑起来、真内联（mac 已实测，见兄弟单）：

1. 构建：`cmake --preset windows-x64 -DLIFT_AOT_LTO=ON`（clang-cl + lld-link）与
   `linux-x64`（GNU ld / ld.lld，必要时 `-fuse-ld=lld`）；记墙钟、链接峰值内存、shape 串。
2. 判据（与 mac 同一口径，⛔ 不接受"shape 串写着 -lto-abi 所以开了"）：
   - `lift-aot-ir-target-attrs` 绿（生成物属性与运行库对齐）；
   - `file out/aot/lift_aot.part00.obj` 是 LLVM bitcode；
   - `node .tmp/lto/measure.mjs <binary>` 的 `_lift_tick` 站点相对 OFF 档显著下降（mac 是 −86.4%）；
     ⛔ 若站点不降 ⇒ 链接器静默没跑 LTO 后端 ⇒ 记失败，保持默认 OFF；
   - 60 帧语义指纹与 OFF 档逐位相同。
3. 通过 ⇒ 把该平台的默认值改成 ON，并同步 `AGENTS.md` §12.8 与 `docs/03-emulator/aot-lto-evaluation.md`
   的平台范围；不通过 ⇒ 在本单写清失败命令 / 报错 / 结论。

## 为什么现在不做

本机是 mac（一仓一树），win32 构建与运行都不在场；`AGENTS.md` §2 的口径是 win32 优先、
mac 只做兼容性验证 ⇒ 不拿未验证的默认值去赌 win32 的构建。
