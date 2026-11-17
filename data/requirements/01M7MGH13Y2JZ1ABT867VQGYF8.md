# 闭包漏扫未对齐立即数（D26）：0x004F80E0 是手工补的，下次 census 会抹掉

- id: REQ-01M7MGH13Y2JZ1ABT867VQGYF8
- type: req
- status: done
- parent: REQ-01M7CZQZ5RR0WV8CQE2ZEKJHHQ
- verify: apps/emulator-lift/tests/closure_imm32.cpp#closure-imm32/manual-root-removed
- tags: [emulator-lift, aot, closure]

## 一句话

`aot/closure.txt` 是生成物（`closure_build` 的 D14/D15/D17/D26 扫出来的根）。`0x004F80E0`
（`EnumSystemLocalesA` 的回调 EA）只以 `push $0x4F80E0` 的**未对齐立即数**出现在 `0x4F822B`
⇒ 旧的 D26 只扫 4 字节对齐 dword，抓不到它，当时是手工补了一行根。

## 修法（本轮，根治）

- 新 `apps/emulator-lift/src/aot/closure_scan.h`：`scanInstructionImmediates()` 扫**可执行节里任意偏移**的
  `68 imm32`（push）/ `B8..BF imm32`（mov r32）/ `C7 /0 imm32`，候选**并入** D26 的 `jumptab_seeds_`，
  与对齐 dword 的候选**并集**后走 `offerSoftRoot` 同一套收/丢判据（⛔ 不新增 SoftKind、⛔ 不给某个 EA 开特判）。
- 生成器与守卫（`tests/closure_imm32.cpp`）**共用同一份扫描实现** ⇒ "守卫绿"才等于"生成器收得到"。
- `aot/closure.txt` 的 `0x004F80E0` 手工根**已删除**，仍由扫描收进（派发表有 `LIFT_AOT_ENTRY(0x004F80E0u,…)`）。

## 读数（2026-11-17 / mac arm64）

- 站点 20482（push 7462 / mov 13020）· 值落在可执行节 969 · 不同候选 665（**仅本扫描发现 608**）·
  D26 `soft-roots=667`。
- 同一条扫描还收进了 `0x00513B90`（Quit 的 EH handler chunk）与 `0x0041389A`（catch 续延）——
  见 `REQ-01M7HR6N2D971KV88J1JR6TPG7`。
- 变异：`++i → i += 4` ⇒ `closure-imm32/candidates` / `dispatch-table` / `emitted-blocks` 共 9 处红；
  还原后默认档 106/106 绿。

## 判据

- 删掉手工根后守卫必须绿（手工根已删，现在就是这个状态）；
- 把"任意偏移"退化成"4 字节对齐" ⇒ 守卫必须红（变异已实测）。
