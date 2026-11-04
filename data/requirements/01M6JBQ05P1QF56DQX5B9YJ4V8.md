# macOS arm64（16 KiB host 页）上 guest 4 KiB 页提交失败：ctest 3/18

- id: REQ-01M6JBQ05P1QF56DQX5B9YJ4V8
- type: bug
- status: done
- parent: REQ-01M6J7A2EXR03SSTZYAGM69TY0
- verify: apps/emulator-lift/CMakeLists.txt#memory-pages
- repro: apps/emulator-lift/src/runtime/memory.h#static constexpr std::uint32_t kPage = 0x1000u;
- severity: S2
- tags: [emulator-lift, m1, platform]

## 一句话
Apple Silicon macOS（host 页 = 16 KiB）上 `GuestMemory` 按 **4 KiB** 偏移调 `mprotect` ⇒ `EINVAL` ⇒ 除 3 个不碰 guest 内存的用例，`ctest` 全红。Windows（4 KiB 页）不受影响。

## 观测（2026-11-04，macOS arm64）
- `ctest --test-dir apps/emulator-lift/out` ⇒ **3/18**：`smoke` / `mnemonics` / `corpus-mnemonics` 绿，其余 15 红。
- 典型输出：`placeCode(0x00401000) failed: commit(00401000..00402000) 失败`；`setup: 映射节失败：.text`。
- `sysctl -n hw.pagesize` = **16384**；独立 C 程序复现（4 GiB `mmap(PROT_NONE)`）：
  `mprotect(base+0x1000, 0x1000, RW)` = `EINVAL`，`mprotect(base+0x4000, 0x1000, RW)` = `0`
  ⇒ **只有起始地址**要对齐到 host 页，length 不必。
- 源码锚：`apps/emulator-lift/src/runtime/memory.h` 的 `kPage = 0x1000`；`memory.cpp` 的 `platformCommit`
  直接用 `p * kPage` 作偏移。

## 影响
- macOS 侧目前**无法验证任何 guest 语义**（A1–A5 在 macOS 上没有一条能跑）；Windows 侧不受影响。
- 这不是"新代码弄坏的"：M1 的验收一直在 Windows 上做，macOS preset 从 M1 起就存在但没有验收记录。

## 修法方向（未定）
`platformCommit` / `platformDecommit` 的范围按 **host 页**对齐（begin 向下取整、end 向上取整到
`hostPageSize`）；`commit_bits_` 仍可保持 4 KiB 粒度（guest 语义不变，`safe` 模式仍按位图判）。
⛔ 不要把 `kPage` 改成 16 KiB —— 那会改变 guest 的页语义（堆对齐 / `VirtualAlloc` 边界）。

## 怎么复现
```bash
cd apps/emulator-lift
cmake --preset macos-arm64 && cmake --build --preset macos-arm64
ctest --test-dir out --output-on-failure   # 3/18
sysctl -n hw.pagesize                      # 16384
```
Windows 上同一命令为 18/18 —— 平台差异只在 host 页大小。
