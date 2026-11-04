# 测试 / 变异基建的假绿口子（finish 0 断言 / LIFT_SKIP 吞失败 / mutate 全跳过 exit 0）

- id: REQ-01M6JFZFXE17DV6E6K174YE06X
- type: bug
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/conformance_cases.cpp#test-pf-even
- repro: apps/emulator-lift/tests/check.h#inline int finish(const char* suite)
- severity: S2
- tags: [emulator-lift, m1, tests]

## 一句话
测试 / 变异基建有若干**假绿口子**：`finish()` 不检查"0 条断言"；`LIFT_SKIP` 会把之前的失败吞成 Skipped；`mutate-check` 在**所有条目都不可验证**时仍 exit 0；无资产机器上 5 个 assets 用例是**不注册**（ctest 15/15），输出里看不出"没跑"。

## 落地（2026-11-04）
- `tests/check.h`：
  - `finish()` 在 `g_checks == 0` 时返回 1（空夹具不再算绿）；
  - `LIFT_SKIP` 在 `g_failures > 0` 时返回 1（⛔ 不许把已发生的失败吞成 skipped）。
- `tools/mutate-check.mjs`：**全部条目都"无法判定"** ⇒ 退出码 **4**（旧实现 exit 0）；
  JSDoc 的退出码表补 4。
- `apps/emulator-lift/README.md`：改正"缺资产返回 77"的说法 —— assets 用例在缺资产时**不注册**
  （`ctest` 用例数变少），注册后 env 缺失才返回 77。
- 新增整数夹具（`conformance`）：
  - `test-pf-even` / `or-pf-odd`：PF 在整数运算**第一次**有断言（唯一旧断言在 x87 fcomi）；
  - `rep-movsd-df`：DF=1 分支第一次被执行（源 `[esi]` → 目标 `[edi]`，之后两者各 -4）。
- **由此抓到一个真 bug**：`exec.cpp` 的 `parityEven` 直接返回 0x6996 的位，而那张表是**奇**校验
  ⇒ PF 一直取反。已修（`== 0`），并加变异条目。

## 判据 / 变异
- `conformance#test-pf-even`：`test eax,eax`（0x03，两个 1）⇒ PF=1；`or eax,0`（0x02，一个 1）⇒ PF=0。
- `conformance#rep-movsd-df`：DF=1 时 `edi/esi` 各减 4、目标写 `[edi]`（不是 `[edi+4]`）。
- 变异：`parityEven` 改回 `!= 0` ⇒ `conformance` 红；串操作忽略 `cpu.df()` ⇒ `conformance` 红。
- PF 修正后 `boot-trace` 的 **insns 不变**（60652），hash 从 `D06091AF5DD19647` 变为
  `E0379BF37C2DFAF3`（终态 eflags 里的 PF 现在是对的）；run1/run2 与两份 trace 仍逐字节一致。

## 门禁（本机 macOS arm64）
- `ctest` **22/22**；`LIFT_SAFE_MEMORY=1 ctest` **22/22**；`conformance` **183 cases / 383 checks / 0 failures**。
- `pnpm test` 1159 pass / 0 fail；`requirements validate` 6/6；`lift check` 绿。
- `git diff --check` 干净。

## 保留
- `finish()` / `LIFT_SKIP` / mutate 退出码没有专门的"反向夹具"（要造一个 0 断言的二进制才能验）；
  它们本身是基建守卫，逻辑改动已逐行复核。
- assets 用例仍未在缺资产机器上注册（只是 README 说清了）；"注册并 SKIP 77"要改 5 个测试的文件存在性检查，
  留作以后的体验改进（不阻塞本单）。
