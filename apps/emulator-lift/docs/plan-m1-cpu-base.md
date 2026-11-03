# M1 · guest CPU 核心 + §3.1 共享底座（实现计划）

> **本文件是计划与判据，不是进度。** 做到哪一步看 `pnpm tools requirements plan`。
> 上游： [`../README.md`](../README.md)（口径与已定决策） · [`../SCOPE.md`](../SCOPE.md)（用户拍板的偏差） ·
> [`../../../docs/03-emulator/platform-effort.md`](../../../docs/03-emulator/platform-effort.md)（十簇分析与切分顺序）。
> 目标（用户 2026-11-03）：先做到**核心的 CPU 模拟部分**，附带 `platform-effort.md` §3.1 的共享底座；
> 细节可再商量 ⇒ §4 与 §12 里每一条都写了"推荐值 + 否决的代价"，不靠默会。

## 1. 一句话

让 `AGE.EXE` 的**真实机器码**在 guest 地址空间里跑起来：从 PE 入口出发穿过静态 CRT 启动，走到 `_WinMain@16`，
一路上每一次外部调用都落到我们自己实现的基座上；整条轨迹**可复现、可对照、缺口可数**。

★ 它填的是 `platform-effort.md` §1.1 **显式排除**的那一类成本（"lift 本体"）：那份文档把它算成
"把 `.c` 的 3813 个函数复原成可编译源码"（`.c` 路线）；在 `.lst` 路线下，它的等价物就是**这台 CPU 核心**。
那份文档的 10–20 人月**不含**它 ⇒ 本计划的量级（§6）必须加到上游结论上，再谈"lift 路线 vs emulator 路线"的性价比。

### 1.1 它在三条路线里的位置

口径与对照表在 [`../README.md`](../README.md) §1.1（不在这里复述）。对本计划只有三条后果：

- ★ **D1 已裁决**：执行源 = 拆壳件的字节（Wine 的前提是宿主直接执行 guest 的原机器码，本线没有这个前提 ⇒
  执行语义必须在自己手里）。证据见 §3 的 F1 / F2，裁决与文件口径见 §4 D1。
- ★ **不能把 Wine 的 DLL 当 shim**：Wine 的 PE 侧基本是壳，实现在 unix 侧 `.so`
  （PE 模块经 `WINE_UNIX_CALL` / `__wine_unix_call` 转到 `unixlib_entry_t` 函数表，两侧声明见 Wine 的
  `include/wine/unixlib.h`）⇒ 那是宿主原生代码，**S5 的 shim 必须是自己写的 C++**，没有"接一层"的选项。
- ★ **判据（"还是这条线"）**：平台层必须自写且自洽；某块改为嵌入 Wine 系组件
  （典型：D3D9 → wined3d / DXVK）就归第三条路线，**单独立项、不计入本线成本**
  （`platform-effort.md` §3.7 / §5.4）。

## 2. 现状（2026-11-03）

`apps/emulator-lift/` 现在是一个**能编译、能跑 smoke** 的骨架（约 150 行）：

| 有 | 在哪 |
|---|---|
| guest 寄存器 / 标志位 / 内存 / `rd32`·`wr32`·`push`·`pop`·`call`·`ret` + dispatcher | `src/runtime/x86.{h,cpp}` |
| SDL3 初始化 + `--smoke`（不建窗口） | `src/main.cpp` |
| CMake preset ×3 + `emulator-lift-smoke`（CTest） | `CMakeLists.txt` / `CMakePresets.json` |

**空白（本里程碑要填的全部）**：解码器 · PE 载入 · 4 GB 地址空间与 fault 诊断 · shim 注册与调用约定 ·
任何一个 Win32 实现 · `.lst` 索引 · `data/lift/`。

★ 骨架里有两处要**就地订正**（不是"以后再说"，因为它们会让后面的判据假绿）：

1. `X86::mem` 是 16 MiB 连续 `std::vector` —— README §2.6 要的是 **4 GB 预留 + 按需提交 + 未提交即 fault**。
   16 MiB 缓冲会让"越界读"变成**静默返回 0**（`rd32` 现在就是这么写的），而那正是这一层唯一的机械护栏。
2. `X86::call()` 现在直接 `eip = ea` —— 块边界应由 dispatcher 判定；
   `README` §2.2 的"`jmp/call/ret` 只改 `eip` 并回 dispatcher"是**编译期约定**，不是运行期习惯。

## 3. 五条语料事实（它们决定架构，不是背景）

> 2026-11-03 用一次性脚本从 `corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst`（18 MB）现数。
> **S0 之后只认 `pnpm tools lift corpus-report` 的输出**，本节数字即可作废 —— 那时它们由工具算，不再手抄
> （口径：`AGENTS.md` §10"怎么查"）。在此之前，任何一条都能用一次 `Select-String` 复核。

| # | 事实 | 后果 |
|---|---|---|
| **F1** | `.lst` **没有机器码字节列**：每行是 `段:EA [符号] 助记符 操作数`，没有 `55` / `8B EC` 那样的字节 | 从文本 lift **拿不到字节**，也就没有"字节视角"做独立对照 |
| **F2** | **1,805 处 `[N BYTES: COLLAPSED ...]`**（1,683 函数 + 122 chunk），合计 **175,141 字节**代码在 `.lst` 里**一行指令都没有**。这 1,805 处**全是** CRT / C++ 运行时（`_memcpy` 202 个直呼站点、`_memset` 185、`_rand` 71、`__allmul` 8），**没有一个 `sub_*`** | 文本路**跑不起来**：`_rand` 就在被执行的代码里（`sub_42CA50` 直呼它）。⇒ **解码器是必需件，不是选项** |
| **F3** | 段表不对称：`.lst` 只出现 6 段（`Section 1/2/3/5/7/8`），**段 4 与 6 缺席**；`seg002` 有 **11,065 行 `db ?`**（未初始化） | **静态数据不能从 `.lst` 复原**。镜像只能认 PE 节表（README §2.6 已经这么定，这里给出证据） |
| **F4** | `.text` 里有 **12,986 行内联数据**（`align`、跳转表 `jpt_*` / `byte_*` 开关表） | **不能顺序线性解码**：块边界由控制流决定，内联表当数据 |
| **F5** | 指令谱很窄：**143 个助记符形态**（含 `rep` / `lock` 前缀形态）覆盖 **299,249** 条指令；前 15 个（`mov` `push` `call` `lea` `pop` `cmp` `add` `jz` `test` `jmp` `sub` `xor` `retn` `jnz` `fstp`）占 **87.5%**，前 40 个占 **97.4%**；只用一次的助记符 15 个、用不到 10 次的 35 个；**x87 = 18,668 条（6.24%，47 个形态）**；MMX/SSE ≈ 0.94k（20 个形态） | 语义工作量**有界且可排序**：先 15 个拿 87%，再按频率往下做，永远知道"下一个最赚的是什么" |

★ F5 是这份计划"敢报量级"的唯一理由：整台 x86 是几千条指令，**这个二进制只用 143 个形态**。
★ F5 是**静态**计数（`.lst` 里出现多少次），不是动态足迹；动态足迹在 S7 的 trace 里现查，
判据是"**执行到的**助记符都必须已实现或崩溃得响亮"，不是"静态表里都实现了"。
★ 顺带得到一条**免费的锚**（2026-11-03 实测）：`.lst` 头部有 `; Input SHA256 : 1937F086…`，
而拆壳件（现已入库为 `corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`）的 sha256 **正好等于它** ⇒
载入器可以用这个值校验"我们执行的字节就是这份语料描述的那份二进制"。
★ 它为什么**不该**换成"给清单条目补一个 pin"：`corpus` 的 #4 规则是**入库件不写 sha256**（交给 git/LFS），
而 `.lst` 头部这个值恰好是**跨两件**的锚（字节 ↔ 语料），比单独给二进制打 pin 信息量更大。这条校验进 A3。

## 4. 决策点（推荐值 + 否决的代价）

> 这些是"要么现在定、要么 S2 返工"的分叉。★ **D1 / D2 均已裁决**（2026-11-03）：
> 执行源 = 拆壳件的字节；执行 = 纯内存解释、默认零生成代码。
> `../README.md` §2.1 / §2.4 已就地订正。**剩下 D3（x87 精度）与 D6（基座口径）待裁决。**

### D1 · 执行源 = **拆壳件的字节**（★ 已裁决 2026-11-03）

★ **用户已确认**：执行源用**拆壳件**的机器码。下面保留"为什么不是文本"的记录 —— 它是**理由**，不是候选方案。

- **具体是哪一份**：清单条目 **`binary/age-sectfix`**，`role: baseline` + `readOnly`，
  **已入库（LFS）**：`corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`
  （`git check-attr` 实测 `filter=lfs`；fresh clone 需 `git lfs pull`）。**这就是默认位置。**
- **定位顺序**（实现口径）：`--exe <path>` → 环境变量 → 仓库内 `dest`
  （`corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`）→ 清单 `origin`（旧仓 `raw-parts/`，只在本机开发时在）。
  都没有 ⇒ **拒绝启动**并打印找过的四处，⛔ 不回退到游戏安装目录里的原版 exe。
- ⛔ **不是** `binary/age-original`（`AGE.EXE__dumped.EXE`，仍是 `external-only`）：同尺寸，但实测
  sha256 = `412393FF…`，与 `.lst` 头部的 `1937F086…` **不同**（两份只差节表 24 字节）；且清单原话是
  "PE 节表未还原，直接分析会丢数据段"。用错这一份 ⇒ "执行的字节"与"用来看的语料"不是同一份。
- **校验**：载入时 PE 的 sha256 必须等于 `.lst` 头部 `; Input SHA256 :`
  （实测相等，见 §3 末尾那条免费锚）。不符 ⇒ 拒绝启动。
  ★ 入库后这条校验**没有失效**，反而更重要：LFS 保证"字节没被换掉"，`.lst` 头部保证
  "拿来执行的字节与拿来看的语料是同一份" —— 两件事，别互相当替代。
  ★ **它已经有机械守卫了**：`tools/test/age-sectfix-artifact.test.mjs`（`@env assets`）比对
  入库件 sha256 与 `.lst` 头部（`.lst` 缺席时 `t.skip`，不假绿）；运行期同一条校验在 S1 落。
- **为什么不能是文本**：F1（`.lst` 无字节列）+ F2（一部分 CRT / C++ 运行时被 `COLLAPSED`，
  而 `_memcpy` / `_rand` 这类被直呼的函数正在其中）。
- **`.lst` 的新定位**：符号 / 覆盖 / 对照真源（`EA → 指令文本` + **指令边界**）；
  `disasm-fn` / `disasm-at` 的用法不变，它只是不再是"执行源"。
- 判据档位（**入库不改变它**）：对着真实二进制的判据仍落 **assets 档** —— 分级判据是"**要 LFS 资产**"，
  不是"文件在不在仓里"（`AGENTS.md` §5）。**纯档夹具仍用自造字节 blob**，但理由变了：
  为了让夹具可读、可最小化，而不是因为缺文件。
- ★ 清单已经随入库一起订正（`pnpm tools corpus set` 是唯一写入口，不要手改 JSON）：
  `storage: lfs` · `role: baseline` · `dest` 指向入库件 · `origin` 去掉 `machine-local`
  （#4 规则：**入库件不许标它**）· `consume` / `note` 改为"执行源"口径。
  `pnpm tools corpus validate` 保持 8 通过 / 0 失败 / 1 警告（与本轮之前同形）。

### D2 · 执行 = **纯内存解释**（★ 已裁决 2026-11-03）

★ **用户已确认**：直接**纯内存解释**，**默认不产出任何生成代码**。

- 执行 = 运行时解码 + **块缓存**（`table[EA]` 的条目在首次执行时填入）。没有构建期的生成物，
  也就没有"生成树与字节不一致"这类守卫要养。
- 保留 README 的三件事：块是执行单位 · 覆盖走清单 · 生成物不入库（此时**根本没有生成物**可言）。
- 慢不是路线问题：先测（S8 的 `bench` 出吞吐数）；不够再考虑块内联 / 热块原生替换 ——
  那是**后续里程碑**，⛔ 不进 M1。
- 裁决理由：约 30 万条指令生成 C++ 是 10⁶ 行量级（编译时间与内存不可控），而解码器已经是必需件（D1）
  ⇒ 生成只是把"解码"从运行时搬到构建期，**一条语义都不用少写**。
- 将来若真要"把某段块离线导出"（便于对照，或把某个函数固化成夹具），那是**调试工具**，
  ⛔ 不是执行前提，也不改默认路径。

### D3 · x87 的 80 位扩展精度

**问题**：x87 寄存器是 **80 位**（64 位尾数 + 15 位指数 + 显式整数位），C++ 没有能装它的整数-可移植类型。
而我们的三个 preset 里 **有两个拿不到 80 位**：

| preset | `long double` 实际宽度 |
|---|---|
| `linux-x64`（GCC / Clang, x86-64） | **80 位** ✓ |
| `windows-x64`（clang-cl / MSVC ABI） | 64 位（= `double`） |
| `macos-arm64`（Apple clang） | 64 位（= `double`） |

⇒ 用 `long double` 会变成**同一份代码三个平台两种行为**，那正好违反"夹具要在三个 preset 上都绿"。
只靠 `double`（53 位尾数）会在三种地方露出：① 连续运算（每步舍入到 53 位 vs 保留 64 位）·
② **比较**（`fcom` / `fucom`）在临界值上翻转 ⇒ 走另一条分支 · ③ `fistp` / `fstp dword` 变成
**两次舍入**（80→53→24），.5 边界上差 1。

**量级（现查，见 F5）**：x87 共 18,668 条（6.24%），47 个形态；其中**算术 / 载入 / 比较**占绝对多数。

★ **一条必须写清的边界**：x87 的**超越函数**（`fsin` / `fcos` / `fptan` / `fpatan` / `fyl2x` / `f2xm1` /
`fprem`）在 Intel 文档里就是**"implementation defined"**，任何实现（含真机、含 Wine）都拿不到"逐位"。
本作语料里这族**合计约 10 条**（`fsin` 1 / `fcos` 2 / `fpatan` 2 / `fyl2x` 3 / `fprem` 1 / `fscale` 1）
⇒ 它们可以**用宿主 libm + 登记 1 ulp 偏差**，不需要软浮点。

- ★ **已裁决（2026-11-03 用户）：(a) 用 Berkeley SoftFloat 3e 的 `extF80` 做算术内核**，不自己写。
  - **为什么是它**：`extF80` 就是 x86 80 位扩展；`extF80_roundingPrecision`（32 / 64 / 80 位）与
    `softfloat_roundingMode`（`near_even` / `min` / `max` / `minMag`）正好是 x87 控制字的 **PC / RC** 两档；
    `softfloat_exceptionFlags` 对上状态字的异常位。许可 **BSD-3-Clause**；纯整数实现 ⇒ 三平台逐位一致。
  - **vendor，不进 vcpkg**：上游 registry 没有这个 port（本机 2887 个 port 里只有 `mpfr` / `gmp` /
    `boost-multiprecision` / `intx` 一类），而我们依赖的是它的**逐位行为** ⇒ 必须**钉死字节**。
    落点 `apps/emulator-lift/third_party/berkeley-softfloat/`，附 `README.md`
    （照 `apps/emulator-cxx/third_party/pl_mpeg/README.md` 的模板：上游 / 作者 / 许可 / 本仓 sha256 /
    为什么 vendor / 怎么升级）。
  - **它不提供的、仍要自写**：8 寄存器栈 + tag word + TOP 指针 + `fxch` / `ffree` / `finit` +
    栈上溢/下溢的 C1 + 条件码；以及**超越函数**（`fsin` / `fcos` / `fpatan` / `fyl2x` / `fscale`，
    本作合计约 10 条 ⇒ 宿主 libm + 登记 1 ulp）。
  - ⚠ **已知边界**：SoftFloat 不复现 x87 的 **pseudo-denormal / unnormal** 表示（历史怪癖）；
    本作 x87 全在正规值路径上，命中概率极低，真要复现就在我们那层包判断。
- **oracle 只作离线差分**：`linux-x64` 的 `long double` 本来就是 80 位 ⇒ 差分夹具零依赖；
  只有需要**跨平台**跑差分时才考虑 MPFR（LGPL-3.0-or-later + GMP），且它只进测试、不进运行时。
- ⛔ **否决**：(b) 全用 `double`（要登记偏差，且**比较分支**可能翻转）；(c) 只在 `linux-x64` 用原生
  80 位、其它平台 `double`（三平台三种结果，比偏差更糟）。

★ **D3 不阻塞 M1a**：S4 在 M1b，而走到 `_WinMain` 的路径几乎不碰 x87
（CRT 启动是整数 + 少量 `double`；18,668 条 x87 主要在渲染 / 物理 / 动画里，属 M2 / M3）。
⇒ 可以**先批 S1/S2/S3/S5/S6/S7 开工**，到 S4 之前再定；若选 (b)，S4 的夹具判据要从"位精确"降成 ULP 容差。

### D4 · 三级验收阶梯

`L1` 入口 → **首次 shim 调用**（`--until shim`）· `L2` → **`_WinMain@16`（EA 0x4BA890）**（`--until ea`）·
`L3` → 第一次"要窗口 / 读文件 / 读注册表"。
**M1 只要求 L2**；L3 是 M2 的起点（M2 才做窗口 / DDraw / GDI，`platform-effort.md` §4 第 2 步）。

### D5 · 线程：guest 单线程

`../README.md` §2.5 已定；`SCOPE.md` 把音频判 silent、视频判 skip ⇒ 两条服务线程都不该被创建。
M1 里 `CreateThread` / `CreateTimerQueueTimer` / `SignalObjectAndWait` 一律**登记 stub**（打日志、返回失败或空句柄）。
若 CRT 启动真去起线程 ⇒ **记录并开单**，不在 M1 里做线程（那会推翻 §2.5 的单线程口径，得先有证据）。

### D6 · "§3.1 基座"的口径 = **T0 语义七族**

§3.1 的那 56 个导入是按**散布度（≥5 caller）**分的，**不是按语义** —— 里面混着 GDI32 的 7 个
（`SelectObject` / `GetTextMetricsA` …，属**字体簇**）与 d3dx9 的 6 个（属 **D3D9 簇**）。
M1 只收它的 T0 语义部分，七族：

| 族 | 内容（Win32 形状；语义按微软文档，不涉及本作私有语义） | 为什么在 M1 |
|---|---|---|
| 同步 | 临界区（**必须可重入**）/ 事件 / 信号量 / `WaitForSingleObject` / `Interlocked*` / `Sleep` | CRT 启动就要；`platform-effort.md` §3.1 驱动因素 1 |
| 内存 | `Heap*` / `Local*` / `Global*` / `VirtualAlloc` 系 → **同一个 guest 堆**（README §2.6） | 同上 |
| 文件与路径 | `CreateFile` / `ReadFile` / `SetFilePointer` / `FindFirstFile` 族 / 目录 / 临时文件 / 模块文件名 | "能读配置、能读存档"的最小前提 |
| 计时 | `timeGetTime` / `QueryPerformanceCounter` / `GetLocalTime` / `timeSetEvent` | **要可复现 ⇒ 虚拟时钟**（§8.3 的确定性判据靠它） |
| 错误码与 TLS | `GetLastError` / `SetLastError`（线程局部）/ `Tls*` / `Encode/DecodePointer` | §3.1 驱动因素 2 |
| 码页与字符串 | `MultiByteToWideChar` / `WideCharToMultiByte` / `GetACP(=932)` / `LCMapStringW` / `wsprintfA`（varargs） | 引擎全是 CP932 路径；**这是唯一要引第三方码表的地方**（`libiconv`，见 §8） |
| 进程 / 模块信息 | `GetModuleHandle` / `GetProcAddress` / `LoadLibrary` / `GetCommandLineA` / `GetStartupInfoA` / `GetVersionExA` / `GetSystemInfo` | 静态 CRT 启动的直接依赖 |

**不含**：GDI32 的 7 个与 d3dx9 的 6 个（留给 M2 字体簇 / M3 D3D 簇）。
权威名单 = `pnpm tools platform census` 的导入表 × `platform-effort.md` §3.1 的族划分，**本文件不抄全量清单**（抄了必漂）。

## 5. 验收（M1 怎么算完成）

五条，全部机械可判；每条都写清"在哪一档"（分级规则见 §8）。

| # | 判据 | 档 |
|---|---|---|
| **A1** | `ctest -R conformance` 全绿（表驱动的指令语义夹具，代码块是**我们自己造的字节**，不碰游戏）；再加一遍 `LIFT_SAFE_MEMORY=1` 的全绿（每次 guest 访存都查提交位图） | pure |
| **A2** | 语料里出现的**每个助记符**都能映射到规范名，或被显式判为"数据 / 伪指令"：**未映射 = 0**。（IDA 的 `retn`/`jz`/`setnz` 与规范的 `ret`/`je`/`setne` 是别名，映射表要显式） | pure |
| **A3** | **解码对照**：⓪ 先校验 PE 的 sha256 == `.lst` 头部 `; Input SHA256 :`（对象是 `binary/age-sectfix`，见 §4 D1）；① 对 `.lst` 里每一条非 collapsed 指令行，在**同一 EA** 解码真实字节 ⇒ 长度 == 下一行的 EA 差；② 助记符 == 规范名。**不一致 = 0**；不一致时打印 EA + 两侧文本 | assets |
| **A4** | 跑到 **L2**（`--until ea 0x4BA890`）成功；**同一条命令跑两次，trace 逐字节相同**（虚拟时钟 ⇒ 不许有真时钟进 trace） | assets |

★ **A4 的"输入清单"必须显式**（2026-11-04 实测发现）：guest 的命令行就是 `"<--exe 传进来的那个路径字符串>"`，
所以**换个路径写法就是另一条执行路径**。同一台机器上实测到达 `_WinMain@16` 的指令数：
相对路径 **38,456** · 绝对路径 **44,184** · 测试里那种带 `../../` 的 87 字符路径 **53,134**（单调于路径长度）。
⇒ ① "同一条命令跑两次逐字节相同"只对**固定输入**成立，这是正确的口径，但引用任何指令数时必须连路径一起给；
② 若将来要做**跨机** trace 比较（两台机器的仓库路径不同），必须先归一化 exe 路径 / 环境块 / 时钟，
否则比的是两件不同的事。trace 自己的输入清单应包含：exe 路径字符串 · guest 命令行 · 环境块 · 虚拟时钟起点。
| **A5** | **基座缺口 = 0**：trace 里出现过的导入 ⊆ (实现 ∪ `stub` 带 reason)；**七族里的导入不许是 `stub`**，除非写进 `SCOPE.md` 的偏差表 | assets |

★ **A3 是整份计划里最强的一条判据**：它把"我们的解码器"钉在 30 万条**独立产出**的指令边界上
（IDA 的边界 vs 我们的解码）。除了它，没有任何东西能在第一周发现"解码器在某条指令上与 IDA 不一致"。
★ 它也是**F1 的正面用法**：`.lst` 没有字节，恰恰使它成为一份与字节**不共享错误来源**的独立投影。

## 6. 切片

切成两批：**M1a 先打通垂直切片**（架构验证点），**M1b 补广度**。宁可先拿 87% 的指令跑通整条链，
也不要先把 143 个形态的语义写全再发现架构错。

| # | 切片 | 做什么 | 判据 | 量级 |
|---|---|---|---|---|
| **S0** | 口径与数据契约 | 订正 README（D1/D2 定案后）· `data/lift/overrides.{json,md}` + `tools/lift.mjs`（`describe` / `check` / `report` / `corpus-report`）· `pnpm tools lift` 进域地图 | `pnpm tools lift describe` 有输出；`json-docs` 守卫绿（JSON 旁边必须有同名 `.md` 且含"怎么查 / 怎么改"）；README 与实现一致 | 0.5 周 |
| **S1** | GuestMemory + PE 载入 | 4 GB 预留（`PROT_NONE` / `MEM_RESERVE`）· 按需提交 · 区域表 · fault handler 打印诊断（EA + `.lst` 文本 + 寄存器 + guest 返回栈）· 节表 / 导入表 / 入口 · **sha256 对齐 `.lst` 头部** · 栈（1 MB + guard 页）· TEB / PEB | assets：段范围 / 入口 / 导入名集合与 census 一致；**越界访存打印诊断而不是静默 0**；sha256 不符 ⇒ 拒绝启动 | 1.5–2 周 |
| **S2** | 解码 + 块缓存 + 派发 | Zydis → 自有 `Insn`（op + 操作数 + 长度）· 块缓存（懒解码，`jcc`/`jmp`/`call`/`ret`/`int` 收尾）· `.lst` 索引（`EA → 文本`，仅诊断 / 对照）· IDA→规范助记符映射表 | **A2 + A3** | 1–1.5 周 |
| **S3** | 整数与控制流语义 | 前 40 助记符（**静态指令数的 97.4%**）：`mov` `lea` 算术 / 逻辑 / 移位 / `cmp` `test` `setcc` `jcc` `jmp` `call` `ret` `leave` `push` `pop` `mul` `div` `imul` `idiv` `cdq` `cwde` `movzx` `movsx` `rep stosd` `rep movsd` `nop` · **未实现助记符被执行 = 响亮失败**（⛔ 不许 no-op） | A1 | 2–2.5 周 |
| **S4** | x87 + MMX/SSE | **算术内核 = vendor 的 Berkeley SoftFloat `extF80`**（D3）· 自写 8 寄存器栈 / tag / 控制字 / 条件码 · **47 个** x87 形态（共 18,668 条）+ **20 个** MMX/SSE 形态（共 ≈0.94k 条）· 超越函数走 libm + 登记 | A1（位精确夹具）+ 与 `long double` 的差分夹具 | 2–3 周 |
| **S5** | shim 框架 | **每个导入一个唯一 guest 地址**（shim 桩页）⇒ `call [IAT]` 落到 dispatcher 时转成本地函数 · stdcall / cdecl / varargs 弹参与返回值 · guest↔host 字符串 helper · 句柄表 · trace（可过滤、可确定性） | pure：一条"人工 shim"夹具（guest 侧造栈 ⇒ 断言弹参、返回值、栈平衡）；`--trace-shims` 两次运行相同 | 1–1.5 周 |
| **S6** | §3.1 T0 七族 | 按 §4 D6 的七族逐族落地；文件族含 **大小写不敏感路径解析**（`--game-dir` 是唯一根）与通配符 | A5 + 每族一组夹具 | 2.5–3.5 周 |
| **S7** | 跑到 `_WinMain@16` | L1 → L2 的 debug；trace 与覆盖报告 | **A4** | 1–2 周 |
| **S8** | 守卫与收口 | CTest 分级（§8.1）· 变异清单（§8.2）· `bench` 目标（吞吐 MIPS 入报告） | 破坏一处关键常量 ⇒ **指定 CTest 红**；`bench` 有数 | 0.5–1 周 |

- **M1a** = S1 + S2 + S3 + S5 + S6（前三族：同步 / 内存 / 错误码·TLS）+ S7（L1→L2）≈ **7.5–11 人周**
- **M1b** = S4 + S6（其余四族）+ S8 ≈ **4.5–6.5 人周**
- **M1 合计 ≈ 12–17.5 人周（3–4 人月）**，一个人、熟悉本仓、不带外部等待。

★ **M1a 是这条技术路线的 go / no-go 点**：它同时验证 D1 / D2 / S5 三个架构假设，
沉没成本 ≈ 7.5–11 人周（对齐 `platform-effort.md` §4 第 2 步"在这里停手最便宜"的逻辑）。
S4（x87 / SSE）与 S6 的后四族**不改变架构**，所以放在后面。

## 7. 落点

```text
apps/emulator-lift/
  docs/                       ← 本目录（计划与分析）
  src/main.cpp                ← CLI：--exe / --game-dir / --until / --trace-shims / --lst / --smoke
  src/runtime/x86.{h,cpp}     ← guest 状态 + 块缓存 + dispatcher（骨架就地演进）
  src/runtime/memory.{h,cpp}  ← 4 GB 预留 / 提交位图 / 区域表 / fault 诊断
  src/runtime/image.{h,cpp}   ← PE 节表 · 导入表 · 入口
  src/runtime/decode.{h,cpp}  ← Zydis 包装 → 自有 Insn（+ IDA 助记符别名表）
  src/runtime/exec/*.cpp      ← integer / control / fpu / sse / string（按 S3/S4 分文件）
  src/runtime/shims.{h,cpp}   ← 注册表 · 调用约定 · trace
  src/runtime/handles.h       ← 句柄表
  src/runtime/vfs.{h,cpp}     ← Win32 路径 ↔ 宿主路径（大小写不敏感）
  src/runtime/clock.h         ← 虚拟时钟
  src/shims/kernel32_*.cpp    ← 七族，一文件一族
  src/corpus/lst_index.{h,cpp}← `.lst` 的 EA → 文本索引（只读、诊断用）
  tests/*.cpp                 ← CTest（conformance / loader / decode-crosscheck / boot-trace）

data/lift/overrides.json      ← 覆盖清单（doc §5.7 的格式：ea|range, kind, impl, boundary, reason）
data/lift/overrides.md        ← 同名说明书（必含"怎么查 / 怎么改"，指向 `pnpm tools lift describe`）
tools/lift.mjs                ← describe / check / report / corpus-report（唯一的 JSON 读入口）
```

★ **生成物不入库**：块缓存只在内存里；`.lst` 索引是运行时快照（不做持久化）。
★ `data/lift/overrides.json` 一落地就进 `json-docs` 守卫的管辖 ⇒ **S0 必须连 `.md` 和
`tools/lift.mjs --describe` 一起交**，否则 `pnpm test` 当场红。

### 7.1 第三方依赖

| 包 | 用途 | 引入方式 | 许可 |
|---|---|---|---|
| `zydis` | 指令解码（S2）——**只解码，不执行**：语义仍是我们的 | **vcpkg**（本机 `../emulator-cxx/vcpkg/ports` 里有 port） | MIT（替代：`bddisasm` Apache-2.0 / `capstone` BSD-3） |
| `libiconv` | CP932 ↔ UTF-16（S6 码页族） | **vcpkg**（有 port） | LGPL（静态链接要照 LGPL 的再链接义务处理；备选是自带一份 CP932 表） |
| **`berkeley-softfloat`** | x87 的 80 位算术内核（S4，见 D3） | **vendor**：`third_party/berkeley-softfloat/` + `README.md`（照 `apps/emulator-cxx/third_party/pl_mpeg/` 的模板） | BSD-3-Clause |
| `sdl3` | 宿主层 | 已有（vcpkg） | zlib |

★ **为什么 SoftFloat 不走 vcpkg**：上游 registry 没有这个 port；更要紧的是**我们依赖的是它的逐位行为**，
包升级换了算术实现就会让位精确夹具无声失效 ⇒ 必须钉死字节（vendor + 本仓 sha256）。

★ 为什么不用宿主的码页设施（Windows 上直接用真的 `MultiByteToWideChar`）：那会让 **Windows 与 macOS
走两套 CP932 表**，"同一份存档名在两台机器上编出不同字节"就变成可能。宁可一套表，也不要这个自由度。

## 8. 测试与守卫

### 8.1 分级（照 `AGENTS.md` §5 的 CTest 规则）

- **顶层注册 ⇒ `pure` 档**：`conformance`（自造字节块）· `dispatch`（块缓存 / 派发）· `shim-abi`（调用约定夹具）。
- **落在 `if(...)` 里 ⇒ `assets` 档**（要游戏安装 / `binary/age-sectfix`）：
  `decode-crosscheck`（A3）· `loader` · `boot-trace`（A4）· `base-coverage`（A5）。
  条件用 `if(EXISTS "${AGE_SECTFIX_EXE}" OR DEFINED ENV{AMAYUI_AGE_EXE})` 一类，
  **判据是注册位置，不是清单文件**。

### 8.2 夹具自己也得会红

- 每条语义夹具必须断言"**至少一个寄存器或内存槽与初值不同**" —— 恒等夹具（`expected` 抄成 `actual`）要能被驱动机械检出。
- 变异自检：注入 5 处已知破坏（一处 flag 计算 / 栈语义 / stdcall 弹参 / 提交位图检查 / 句柄校验），
  确认**指定 CTest 退出码非 0**。形式与 `tools/mutate-check.mjs` 同构（施加 → 跑 → 复原 → 比对全文）；
  若直接复用那个工具，它需要支持"命令类型"的条目。

### 8.3 确定性（贯穿全部判据）

1. **时间**：默认虚拟时钟（`timeGetTime` 是计数器，`QueryPerformanceCounter` 同源）；
   `--real-time` 才接宿主时钟，**且此时 A4 的逐字节断言自动跳过并明说**。
2. **`cpuid` / `rdtsc`**：返回**固定常量**描述（虚构 CPU），不接宿主 CPU —— 否则同一台机器换个 CPU 结果就变。
3. **随机**：guest 的 `_rand` 是**二进制里的代码**（在 `.lst` 里被 collapsed，但字节在 PE 里），
   它跑起来就是确定的；**宿主侧不许引入随机源**。
4. **文件枚举**：`FindFirstFileA` 的顺序由宿主的目录顺序决定 ⇒ 排序后返回，并在 trace 里记一次"已排序"。

## 9. 风险与停手点

| # | 风险 | 早期信号 | 处置 |
|---|---|---|---|
| R1 | **SEH / C++ 异常**：MSVC 2017 的 `_CxxThrowException` 走 SEH 展开（`.xdata` handler），真做全展开是大工程 | 启动路径上遇到 `RaiseException` / `RtlUnwind` | M1 只做"未处理异常 = 响亮失败 + 完整诊断"；若**成功路径**也用异常做流程控制，单独开单再议 |
| R2 | **大小写不敏感 VFS 与通配符**：`FindFirstFileA("*.*")`、8.3 短名、尾随点 | 读到一半"文件不存在" | 先只实现游戏实际用到的形态，缺口由 trace 暴露；⛔ 不静默忽略 |
| R3 | **x87 精度**（D3） | 坐标 / 透明度出现 1e-15 级漂移、或 UI 位置整体偏 | soft 80-bit + 差分夹具；兜底走 SCOPE 登记 |
| R4 | **PPL / Concurrency 线程池**：CRT 或游戏启动时创建线程 | 出现 `CreateThread` / `CreateTimerQueueTimer` 调用 | D5：登记 stub + 开单；单线程口径要改必须先有证据 |
| R5 | **性能**：解释器吞吐不够游戏帧预算 | S3 后跑 `bench` 拿 MIPS 数 | 先测再议：块内联（threaded code）→ 热块原生替换。**M1 不写 JIT** |
| R6 | **量级本身**：12–17.5 人周是"熟悉本仓的人"的量级 | M1a 超过 **11 人周**（区间上界）仍未到 L2 | **停手复盘**：说明 CPU 核心比预估贵，回到 emulator 路线重新评估（与 `platform-effort.md` §4 第 2 步同一口径） |

## 10. M1 明确不做

⛔ 窗口 / 消息泵 / 菜单（M2）· ⛔ DirectDraw / GDI 字体（M2）· ⛔ D3D9 / D3DX / Live2D（M3）·
⛔ 音频 / 视频（SCOPE 已判 silent / skip）· ⛔ 线程与真并发（D5）· ⛔ 完整的 SEH / C++ 展开（R1）·
⛔ JIT / 动态翻译（R5）· ⛔ 64 位 guest（本作是 32 位 PE）· ⛔ AGERC.DLL（独立，随时可做）·
⛔ 用 `apps/emulator-cxx` 的 core 当 shim（README §1 的边界）·
⛔ **把 Wine 系组件当 shim**（理由与判据见 §1.1：那会退化成第三条路线）。

## 11. 对上游结论的影响（必须回写）

1. `platform-effort.md` 的 **10–20 人月不含 lift 本体** ⇒ 加上 M1 的 3–4 人月，总价约 **13–24 人月**；
   而 §4 的「停手点 A（≈3–5 人月）」那条判据要把 CPU 核心算进去，否则停手点来得太晚。
2. `AGENTS.md` §11 现在写着"**唯一在维护的模拟器是 `apps/emulator-cxx`**"，而这条线已经在维护
   （`docs/03-emulator/README.md` §7）。✓ 事实没说错，但**那句话会让下一个人把"新工作只落 `apps/emulator-cxx`"
   用过头** —— 要订正成"两条线"并说清判据（S0 做）。
3. `data/lift/overrides.json` 一落地就是"自己的结构化数据" ⇒ 进 `json-docs` 守卫的全仓扫描（S0 一并处理：
   同名 `.md` + `pnpm tools lift describe`）。

## 12. 待裁决（按顺序回答即可开工）

★ **已裁决**（2026-11-03 用户）：
- **D1** = 执行源用**拆壳件的字节**（`binary/age-sectfix`，已入库 LFS）—— 见 §4 D1。
- **D2** = **纯内存解释**，默认零生成代码（运行时解码 + 块缓存）—— 见 §4 D2。
- **D3** = x87 算术内核用 **Berkeley SoftFloat 的 `extF80`**（vendor，不进 vcpkg）—— 见 §4 D3。

余下：

1. **D6**：M1 的基座口径 = T0 七族，不含 GDI32 的 7 个与 d3dx9 的 6 个 —— 同意？
2. **切片要不要现在就进需求树**：本文件只有判据，没有状态；
   把 S0–S8 登记成 `data/requirements/` 的节点（挂在一个新的 "emulator-lift · M1" 下）之后，
   `pnpm tools requirements plan` 才开始回答"到哪一步了"。
