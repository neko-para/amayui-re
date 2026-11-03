# emulator-lift —— 从 `.lst` lift 的 guest-state 模拟器

> 本文件只写**不变的口径**（源 / 执行 / 宿主 / 覆盖 / 边界），**不写进度与计数**：
> 进度看 `pnpm tools requirements plan`（本线 `REQ-01M6GJMENJ8XEK4ZPTA768YBPW`）；
> 数据与方法看 `docs/03-emulator/platform-effort.md` §4.1、§5.5–§5.7。
> ★ 本文件只钉口径与落点；实现范围 / 进度看需求树。

## 1. 它是什么 / 不是什么

- **是**：把 `AGE.EXE` 的 `.lst` 转成"带 guest 状态的 x86 执行器" + Win32/GDI/DDraw 形状层，
  宿主用 **SDL3**（macOS Metal / Linux Vulkan / Windows D3D12）。
- **不是**：`apps/emulator-cxx` 的延续（那是按 spec 重写）；也**不是**把 `.c` 编成 64 位原生源码
  （32→64 是移植，不是机械翻译）。
- **边界**：`data/spec/` 只作调试对照，**不是本项目的真源**；本项目的真源是
  **`.lst` + 覆盖清单**；⛔ 不拿 emulator-cxx 的 core 当 shim。

## 2. 四个已定决策

### 2.1 源：`.lst` 是执行源，`.c` 是阅读器

- `.c` 的 32→64 面（`__thiscall` / `__stdcall` / 指针截断）是**系统性移植**，不是改几个类型；
  `.lst` 的 guest-state 不碰宿主指针。
- `.c` 的缺函数 / 位宽产物是**小补丁 + 逐处核对**，不是主路径。
- ⇒ 执行源 = `.lst`；`.c` 只在"这段在干嘛"时读，结论回 `.lst` 核。
- 判据与现查命令：`docs/03-emulator/platform-effort.md` §5.6 · `pnpm tools disasm-fn summary`。

### 2.2 执行：按块寻址，不按函数

- 存在**跨函数 `jmp sub_*`**、**function chunk**、**跳转表 / 间接 jmp** ⇒
  "一个 `sub_` 一个 C 函数 + 内部 `goto`"不成立。
- 执行单位 = **basic block**；`jmp` / `call` / `ret` 只改 `eip` 并回 dispatcher；C 函数不跨函数 `goto`。
- **函数** = 入口块 + 内部块 + 外部入口块（chunk）的**聚合**，只用于报告与覆盖；
  它**不是**执行单位。
- 数据见 doc §5.7；块表 / 调度表的接口见 `src/runtime/x86.h`。

### 2.3 宿主：SDL3，不直接调 AppKit

- SDL3 提供窗口 / 事件 / 音频 / SDL_GPU；macOS 走 Metal（MSL 源码），Linux Vulkan，Windows D3D12。
- ⚠ **"不依赖 AppKit" = 我们的代码不直接调**；SDL 的 macOS 后端内部仍用 Cocoa/AppKit
  （只要要原生窗口就绕不开）。
- SDL **只替换宿主层**；Win32 消息 / GDI DC / DirectDraw 表面仍由本项目自己写。
- 分层：`lifted guest blocks → Win32/GDI/DDraw 形状层 → SDL3 → Metal/Vulkan/D3D12`。

### 2.4 覆盖：清单 + 调度表

- 生成物：`guest_<EA>` / `bb_<EA>`；覆盖：`ovr_<name>`；桩：`stub_trap`；
  运行时 `table[EA]` 合并三者（块级表；函数级只是聚合视图）。
- **清单** = `data/lift/overrides.json`（格式见 doc §5.7）：
  `{ ea|range, kind: replace|stub|hook, impl, boundary, reason }`。
- 粒度：**平台 shim**（API 级）/ **函数级** / **子系统级**（视频推荐：`video::Session` + 其余 MF 包装 `stub`）。
- 生成物**不入库**：由 `.lst` + 生成器 + 清单确定性重建；`stub` 必须 trap，不许静默 no-op。

### 2.5 线程模型：业务单线程，只有服务线程

- 证据（`.lst`）：`CreateThread` ×2（DirectShow 异步读文件 → `StartAddress`/`sub_402330`；
  Sound → `sub_4B5AA0`）、`__beginthreadex` ×1（MF 回调 → `sub_47B200`）、`timeSetEvent` ×1（多媒体定时器）。
  `_WinMain` 的 `CreateSemaphoreA(..., 1, Name)` 是**单实例守卫**，不是线程；
  `CMemAllocator` 的信号量是 DirectShow 分配器；`Concurrency::...LoadLibraryAndCreateThread` 只有定义、无调用引用。
- **游戏逻辑（VM / 脚本 / 场景 / 存档）在主线程**；音频线程会改音频状态（临界区），
  视频线程只处理样本 / 缓冲。
- ⇒ guest VM 保持**单线程**；服务线程要么宿主化（音频回调 / 视频解码），要么串行化；
  ⛔ 不把 guest 代码放多线程。音频 silent（`SCOPE.md`）时，声音线程可整条省掉。

### 2.6 内存模型：静态镜像 + guest 堆

- **静态数据不是"逐变量硬编码"**：把 PE 的 `.text` / `.rdata` / `.data` / `.idata` 按
  `imageBase + RVA` 映射进 guest 内存；`.c` 里的 `dword_XXXX` 只是 guest 地址，不是 C 变量。
  IAT 槽填成 guest thunk 地址（dispatcher 映射到 shim）。
  - 固定 `imageBase`（0x400000）⇒ 不重定位；若换基址，按 `.reloc` 重定位。
  - 若不能带 PE：把 section 字节抽成 blob 映射（仍是"段镜像"）；⛔ 不要逐变量 `constexpr`。
- **堆是 guest 地址空间里的分配器**，不是宿主 `malloc` 返回宿主指针：
  - `GuestMemory`：**一整块 4 GB guest 地址空间**（`mmap` / `VirtualAlloc` 预留、按需提交；
    不写不占物理内存）；`rd32/wr32` = `base + guestAddr`。
    ⛔ **不做稀疏页表**：64 位宿主上 4 GB 虚拟预留很便宜，平坦映射更快；只有 32 位宿主 /
    需要受控 fault 时才值得。
  - `GuestHeap`：在 `[heapBase, heapEnd)` 里做 free-list / size-class；返回 **guest 地址**。
  - Win32 形状：`GetProcessHeap` / `HeapCreate` / `HeapAlloc` / `HeapFree` / `HeapReAlloc` / `HeapSize` /
    `LocalAlloc` / `GlobalAlloc` / `CoTaskMemAlloc` → **同一个 guest 堆**（保证 `free` 跨 API 兼容）；
    `VirtualAlloc` = 在预留区内 commit（`mprotect` / `MEM_COMMIT`）；`VirtualFree` =
    `madvise(DONTNEED)` / decommit；`VirtualProtect` = `mprotect`（可 no-op，记偏差）。
  - CRT：lifted 的 `malloc/new` 仍走 guest CRT → 最终落到上面的 `HeapAlloc`；若替换 CRT，
    则 shim 的 `malloc/free/new/delete` 也必须用同一个堆。
- **布局（建议）**：image `0x00400000` 起；stack `0x70000000` 起（向下）；heap `0x10000000` 起；
  TEB/PEB `0x7FFD0000` 起；IAT thunk 页单独一段。**整块 4 GB 预留、按需提交**；物理只占 touched 页。
- **对齐 / 清零**：`HeapAlloc` 8 字节、`VirtualAlloc` 64 KB；`HEAP_ZERO_MEMORY` / `LMEM_ZEROINIT` /
  `VirtualAlloc` 清零；`realloc` 保内容。`fs` 段基址指向 guest TEB（SEH / TLS 用）。
- ⛔ guest 槽里不许出现宿主指针；⛔ 不把宿主 `malloc` 的返回值当 guest 地址。

## 3. 目录（落点）

```text
apps/emulator-lift/
  CMakeLists.txt
  CMakePresets.json
  vcpkg.json
  README.md          ← 本文件
  SCOPE.md           ← 显式偏差（用户拍板才改）
  src/main.cpp       ← CLI / 入口（smoke）
  src/runtime/x86.h  ← guest 状态 + 块表接口（最小）
  src/runtime/x86.cpp
  # 后续：src/runtime/{win32,gdi,ddraw}.cpp · src/overrides/ · tools/lift/
```

## 4. 构建（vcpkg **跨目录复用**）

- toolchain 指向 `../emulator-cxx/vcpkg/scripts/buildsystems/vcpkg.cmake`；本目录的 `vcpkg.json`
  只声明自己需要的包（清单真源是 `vcpkg.json`）。
- ```bash
  cd apps/emulator-lift
  cmake --preset macos-arm64
  cmake --build --preset macos-arm64
  ctest --test-dir out --output-on-failure
  ```
- 其它平台预设：`windows-x64` / `linux-x64`（同一个相对 toolchain 路径）。
- 生成物（`out/`、`vcpkg_installed/`）由本目录 `.gitignore` 排除。

## 5. 真源

| 想知道 | 看哪 |
|---|---|
| 源选择 / 32→64 / 缺 C 的量化 | `docs/03-emulator/platform-effort.md` §5.6 |
| 块寻址 / chunk / 跳转表 | 同文件 §5.7 + `src/runtime/x86.h` |
| 宿主层 / 不依赖 AppKit 的口径 | 同文件 §5.5 |
| 可接受偏差（菜单 / 音频 / 视频 / Live2D / 3D） | `SCOPE.md` + 同文件 §4.1 |
| 覆盖清单格式 | 同文件 §5.7 |
| 需求与进度 | `REQ-01M6GJMENJ8XEK4ZPTA768YBPW` · `pnpm tools requirements plan` |
