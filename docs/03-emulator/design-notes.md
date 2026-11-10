# 实现取舍（design-notes）

> 记 **"我们为什么这么实现"**（原 L3）：**换一份实现之后不必再成立**的东西。每条三件事：
> **取舍 · 备选与为什么没选 · 如果将来要重写什么必须保持不变**（最后一句最重要）。编号 `L3-<域>-NN`。
>
> **边界**：引擎语义（字段含义 / opcode 行为 / 脚本角色）**不在这里** —— 那是 `data/spec/` + `data/ledger/`，
> 走 `AGENTS.md` §6 准入门；Win32/D3D 的**对外合同**在 `contracts-win32.md`；读数在 `observations.md`；坏过什么在 `defects.md`。
> 本文吸收测试审计判为 `DOC` 的 17 个 JS 单元 / 139 例与 3 条 CTest（`.tmp/test-audit/inventory.tsv`），
> 按**可迁移的取舍**归并，不逐文件抄。

---

### L3-scope-01 · 哪些东西**该**在文档里而不该在 test 里
- **取舍**：观察 / 缺陷史 / 实现取舍三类一律进文档；test 只留两类：基建契约，与"调用方看得见的结果"（L1，真源是微软文档）。
- **备选**：给每个观察/缺陷配一条用例（旧形态）。**没选**：改了实现就要改测试，而**恒真断言**与"expected 抄成 actual"照样绿 —— 判据与实测见 `.tmp/test-audit/report.md` §2.1/§5。
- **重写必须保持不变**：一句话判据 —— **"这条断言在换一份实现之后还需不需要成立？"** 不需要 ⇒ 它不该是 test。三问全"是"才写守卫：① 用户可见的错/数据损坏 ② 会不会有人发现 ③ 删了会不会让某个错误静默通过。

### L3-heap-01 · guest 堆：首次适配 + **按偏移有序**的空闲块索引 + 收口
- **取舍**：`HeapAlloc`/`LocalAlloc`/`GlobalAlloc`/`CoTaskMemAlloc`/小 `VirtualAlloc` 全落**同一个** guest 堆（`src/runtime/heap.h:1-8`），元数据在宿主侧。首次适配**只扫**空闲块索引 `free_offsets_`（按偏移有序，`heap.h:77-79`）；`alloc` 切出的 rest 若贴着右邻空闲块就记进 `pending_merge_`，下次 free 收口时处理（`heap.h:80-82`）。
- **备选**：① 每次 `alloc` 扫整张块表（旧实现）；② 空闲链表（best-fit / segregated）；③ 用 `vector` 存块。**没选** ①：实测同一过渡帧块比较 **47.57M → 0.52M**（≈92×）、收口合并 **45.81M → 2,773**（≈16.5k×），而 `allocs`/`blocks`/`zero` **一个都没变**（OBS-13）。**没选** ②：会改变"拿到哪一块"，而 guest 可见的**分配地址**是它自己算出来的。**没选** ③：`realloc` 里 `Block& b` 跨 `alloc()` 用（`heap.cpp:142,162,164`）**靠 `std::map` 节点稳定**才安全。
- **为什么必须保序**：有序索引命中的仍是"偏移最小的那一块"，与扫整张块表**结果相同**（`heap.cpp:90`）；`pending_merge_` 让块表"与旧实现**逐字相同**"（`heap.h:81`）。⇒ "优化前后 `in_use` 能一个个对上"是**设计要求**，不是巧合。
- **重写必须保持不变**：① 一个 guest 堆（free 跨 API 不能炸）；② **分配地址逐位一致**；③ 非法/重复 free **响亮失败**（`heap.cpp:116-121` 打印 + `abort()`）；④ `largestFreeBlock()` 必须能把"总量满"与"碎片化"分开（BUG-01 的判别靠它）。

### L3-arena-01 · surface arena 128 MiB 的来历与上限风险
- **取舍**：D3D9 纹理 / 后缓冲 / 深度放**独立的** `GuestHeap` = `0x28000000..0x30000000`（`machine.cpp:107`；`:1418` 注明不与主堆共享）。起点同时是 `cpu_.shim.va_limit`（`machine.cpp:263`），终点是 guest 基址 fallback（`kFallbackBase`，`machine.cpp:165`）。
- **备选**：① 与主堆共用；② 更大（如 256 MiB）；③ 动态扩。**没选** ①：**"纹理没回收"在主堆那行完全看不见**（`apps/emulator-lift/README.md` §4.5），分开才定得出 BUG-01。**没选** ②：会**改 AGERC 的 base**（实测启动行 `AGERC 的 ImageBase 0x10000000 冲突 ⇒ 改用 0x30000000`）⇒ **guest 可见 EA 一起变**，所有锚一起漂。**没选** ③：打破"区间固定 ⇒ 可复现"。
- **上限风险（已知未解）**：128 MiB 只够"**一份**屏工作集 + 余量"（一份 TITLE 装载 = 26 张纹理 / 累计 125.3 MB / 峰值活跃 ≈56 MB，`.tmp/task2-report.md` §2-§4）⇒ 任何"新增一张常驻大纹理"都可能把 `largest-free` 压到请求量以下，而报错文本与"总量满"长得一样。
- **重写必须保持不变**：① arena 独立、区间固定且**不越过 `0x30000000`**；② 摘要必须同时给 `in_use`/`peak`/`largest-free`；③ 失败要报 `w×h / D3DFORMAT / 请求字节 / 最大空闲块`。

### L3-token-01 · 单令牌 → **shim 边界令牌**：为什么"整次 fill 持令牌"不可接受
- **取舍**：`GuestToken`（`src/runtime/guest_token.h`）= 任一时刻最多一个宿主线程持有；**互斥点在每次 shim 调用**（`ShimRegistry::invoke`），feeder 的**整次 fill 不持**令牌 —— guest 指令执行可并行，只有 guest 可见容器（`GuestHeap`/`HandleTable`/`Vfs`/`ComRegistry`/`SurfaceArena`/内存账本）必须在令牌里访问；阻塞前先 `Yield`（⛔ 持令牌等宿主锁）。
- **备选**：① 不修（两线程裸跑）；② **整次 fill 持令牌**；③ 每个容器各加一把 mutex。**没选** ①：实测 `0xC0000374`，约 **25%/轮**（BUG-03）。**没选** ②：整轮墙钟 **61.3–67.1 s → 146.9–287.3 s**，即 **2.4–2.6×（中位）/ 最坏 4.6×**（OBS-15）—— feeder 一次 fill ≈310–450 ms CPU（OBS-14），与主线程相加 ≈ 单核，两线程只能互相等。**没选** ③：六个容器、访问点分散，漏一把就是静默的宿主堆损坏，且没有"谁在持"的观测。
- **为什么 shim 边界够**：guest 共享状态只在两类地方被碰 —— shim 调用与 VM 记账；前者已全部进令牌，后者（`machine.block`）由主线程按块取/放。
- **重写必须保持不变**：① **不得**把令牌扩大到整次 fill；② guest 指令执行保持可并行；③ 可观测三件套 `token : violations= / max-concurrent=` + `first=<site>` + 退出码 4；④ `guest-token-stress`（2 线程 × 200k alloc/free）必须能红。

### L3-gpu-01 · 版本快路径 + 整幅哈希：**不能砍**
- **取舍**：先看**版本号** —— `d3d9.cpp:1067` 在 `UnlockRect` 发新序列号、`:1021` Lock 置 0、`:1077` RT 绑定置 0；后端记住"**已录进命令缓冲**的版本"（`d3d9_gpu.cpp:1294-1302`）⇒ 版本相同就不哈希、不重传。版本变了**且脏区够大**才整幅哈希（`:1313-1323`），哈希相同就只更新版本。
- **备选**：① 每次变更整幅重传；② 只靠哈希；③ 只靠版本。**没选** ①：实测 TITLE 的 `ID3DXSprite::End` 曾因每次 draw 完整 upload 到 **118–206 ms/次**（`d3d9_gpu.cpp:33`）—— 砍掉快路径就是"拿 0.64 ms 换 206 ms"。**没选** ②：整幅哈希正是历史 0.64 ms/条里的固定成本之一（`:1306-1307`），小脏区故意不哈希。**没选** ③：`Unlock` 后没改像素的情况真实存在，哈希能把重传降成 0。
- **为什么"版本"能代表内容**：D3D9 的不变量是"纹理像素**只在 LockRect 期间**由 guest 写"（`d3d9.h:345`）⇒ `UnlockRect` 是唯一可判定的写入点 ⇒ 版本是内容变更的**充分**信号。
- **重写必须保持不变**：① 版本 ≠ 内容（否则等于砍快路径）；② 版本变了但像素没变 ⇒ **0 次重传**；③ "先全部写、再全部画" ⇒ **0 次切段**（反向边界，必须由守卫钉住）；④ ⛔ 不许用 guest 像素指针 / 状态块 EA 当缓存键（两者都会被 arena / guest 堆复用，`d3d9_gpu.cpp:1287-1289`）。

### L3-gpu-02 · 脏矩形 × 切段的判据：**先脏区、后切段**，顺序不能反
- **取舍**：每次内容变更三步 —— ① 算脏区（`gpuContentDirtyRegion`，纯函数 `gpu_raster.h:253`）；② 只与**本段**已入队、且采样同一张 GPU 纹理的 draw 求交（`gpuTexelRectsIntersect`，`gpu_raster.h:60`）；③ 相交才**切段**（`d3d9_gpu.cpp:1349-1367` 的 `segments_`/`need_cut`），而切段**只把命令缓冲切开一个 copy 边界**，⛔ 不是提交。
- **备选**：① 上传全推到帧末；② 合并上传但**不判交**；③ 先切段再判脏区。**没选** ①：写前入队的 draw 会看到新内容（两个方向同时错）。**没选** ③：白切一刀。**没选** ②：合并时机让更早的 draw 看到新内容。设计目标：提交数 **136/帧 → 2/帧**（画 + 读回），切段仍 ~135 段但每段只传一个字形矩形。
- **四条红线**（"看起来能省、实际破坏语义"）：(a) depth **一帧只清一次**（`d3d9_gpu.cpp:668,722`；BUG-06）；(b) 颜色 pass 用 `LOAD` 不用 `CLEAR`；(c) `SDL_UploadToGPUTexture` 的 `cycle=true` **只能给整幅**（`:643` + `:1184-1185`；BUG-05）；(d) 同一命令缓冲里**不许重写同一块 staging**（命令在提交后才执行，两次 upload 读该内存的**最终值**）。
- **重写必须保持不变**：① **"写"与"提交"不是一回事**（切段 ≠ 提交）；② 采样顺序 = 调用时刻的 guest 内容；③ 脏区**未知就按全幅**（Lock 无 RECT / `generation==0` 的未知写者）；④ ⛔ 上传不能推到帧末；⑤ 读回/换 target 必须**提交**而不是切段。

### L3-gdi-01 · 度量缓存的键 = **描述**（字号, bold, 字体路径），不是 HDC
- **取舍**：键 = `(clamp 后像素字号, bold?1:0, 字体路径)`（`src/runtime/font.cpp:163,444-463`）；只缓存 **FT 派生段**（ascent/descent/height/两条 leading/ave/max/overhang），`weight/italic/underline/strike` 每次从 `LOGFONT` 覆盖（**不进键**）；合成档**不进缓存**（键会少一维）；失效点只三处 `setSyntheticForTest`/`fontSetConfig`/换源（`:294,300,307`）；上限 64 项、到上限**整表 clear**（不做 LRU，条目天然个位数）。
- **备选**：① 键 = `HDC`；② 键 = `(字号)`；③ 不缓存。**没选** ①：引擎在**同一个**屏幕 DC 上先用常量参考字体量、再改用按尺寸算出的字体量（`.tmp/task3-report.md` §2）⇒ 20px 的量度被复用给 28px ⇒ `tmAscent`/`tmHeight` 各少 8px ⇒ 整行上移 8px。**没选** ②：`-Bold.ttf` 是**另一份 face**，ascent 与 advance 都不同 ⇒ 误差逐字横向累积。**没选** ③：每次 3 次 `FT_Load_Char`（实测 111 µs/次，BUG-07）。
- **为什么"描述"完备**：`metrics` 的唯一输入集就是 `{像素字号, bold, 字体路径, synthetic}`（`font.cpp:325-364`）；DC 的"选择史"不是输入 ⇒ 键取描述就**天然**不需要 `SelectObject`/`DeleteObject`/`DeleteDC` 的失效钩子。
- **重写必须保持不变**：① 键必须覆盖**全部**输入维度（今天还欠 `lfWidth`/`lfEscapement`/`lfOrientation`，加维度前缓存会串）；② 合成档与真字体**不许互相复用**；③ 换 `--font` 必须清表。

### L3-text-01 · 基线补偿的**唯一位移点**在 `gmptGlyphOrigin.y`，不在 `TextOutA`
- **取舍**：`baseline_lift`（常量 2）**只**加在 `gmptGlyphOrigin.y`（`src/runtime/font.cpp:356-360`），`tmAscent` 仍是 FT 真值（补偿**不**写进度量，`SCOPE.md:111`）。
- **备选**：① 把 `tmAscent − 2` 写进度量；② 在 `TextOutA` 里再减一次。**没选** ①：会破三条既有合同（`tests/gdi_font.cpp:228` 的 `tm_height == ascent + descent`、`tests/gdi.cpp:432-434` 的合成 22/20/2、`SCOPE.md:111`）且 `gmptGlyphOrigin` 必须自洽（`SCOPE.md:112`）。**没选** ②：抬两次。**已知代价（有意接受）**：引擎正文走 **GGO + guest 自己合成**（`TextOutA` 只 10 站点 / 2 caller）⇒ 本补偿对正文**完全无效**，与对照实现是"同常量、不同效果"（BUG-08）—— 这是**产品裁决**，不是遗漏。
- **重写必须保持不变**：① 补偿只在一个位移点、且**不写进 `TEXTMETRIC`**；② `gmptGlyphOrigin` 自洽；③ 要动就写进 `SCOPE.md` 并同步三条既有断言，**不许**偷偷改常量。

### L3-state-01 · 引擎态快照 / 状态分区 / 地址空间：一套"可复现"的实现口径
（吸收 `emulator-snapshot` 8 例 · `emulator-state-partition` 5 例 · `emulator-address-space` 12 例 · `emulator-pool-regions` 10 例）
- **取舍**：快照 = "同样状态给同样字节" —— 键必须**全带**、顺序必须**规范化**（不依赖容器遍历序）、稀疏必须**保真**（没写的格不许被"补 0"抹平）；"每个可变字段归一类"做成**门禁式**约定（新增状态必须表态归属，否则快照会悄悄漏带/多带）。地址空间与池是两份视图、**必须同源**。
- **备选**：① 快照按内存转储；② 只存"有意义的字段"。**没选** ①：同状态两次跑会因容器序不同而字节不同 ⇒ 不能当 A/B 基线。**没选** ②：漏字段**不报错**。
- **重写必须保持不变**：① 同状态 ⇒ 同字节（含顺序规范化）；② 稀疏保真（未写 ≠ 0）；③ 新增可变字段必须表态归属；④ 池与地址空间两份视图同源。★ **引擎语义本身**不在这里（`data/spec/` + `data/ledger/`）。

### L3-model-01 · 引擎数据区域模型 / 指令迭代 / 数值族：把"机械可读"与"语义"分开
（吸收 `emulator-model` 15 · `emulator-iterate` 4 · `emulator-numeric-ops` 5 · `emulator-numeric-purity` 4 · `emulator-engine-scalars` 23+2 · `emulator-frames` 12）
- **取舍**：① **布局常量集中一处**（偏移/步长/池基点只活在格式层，语义层零偏移）；② 指令迭代的**唯一判据是 argc**（长度 = `4 + argc*8`）—— argc 对了整条流都对，错了**全线错位**；③ "纯数值族"要有**可机械判的纯度判据**；④ 引擎标量的"没写过 = 0"必须可判。
- **备选**：① 偏移散落在实现各处；② 靠"看起来对"验迭代；③ 只靠命名把 family 归类。**没选** ①：换一次导出全漂、锚不可再校验（`AGENTS.md` §6）。**没选** ②："大部分脚本看起来都对，只在嵌套/多次调用时错开"。**没选** ③：命名会撒谎（实测 `case 0x1F7: // 拆离绘制键` 的注释已窄于 spec）。
- **重写必须保持不变**：① 偏移只在格式层；② argc 是唯一边界；③ 纯度判据可机械判；④ **语料里 0 次出现 ≠ "这条指令没语义"**（那是判据 `UNUSED_IN_CORPUS`）。★ 这批单元的**引擎语义部分**（帧记录步长、表相邻性、标量清零口径）按 §6 归 `data/spec/` / `data/ledger/`，本文件只留"为什么这样分层"。

### L3-frame-01 · 帧记录的形状：**锚在语料基址 + 偏移**，不抄模型偏移
（吸收 `engine-frame` 7 例；对应 BUG-13）
- **取舍**：帧记录的读法一律写成"**语料基址 + 偏移**"（`.tmp/anchor-design.md:28-36` 那 5 行读地址链）；形状模型把同一条记录锚在另一个基址（`0x5D894` = 语料基址 + 0x14，其实是"脚本体堆缓冲"字段）。
- **备选**：直接抄模型的记录基址与字段偏移。**没选**：**整体错 0x14 字节**，而读错的字段在大多数帧上"看起来合理"（BUG-13 的形态）。两个视图**都对**，差别是"记录从哪里算起"；我们选"语料基址"是因为它同时是 `cur` 与身份格的锚点，一处基址覆盖三个字段。
- **重写必须保持不变**：① 记录基址/步长/字段偏移只在一处定义；② 守卫同时钉"基址 / 记录大小 / 不许把记录之外的 Engine 级标量当 per-cur 字段读"；③ 换视图（重新导出）时**只重建 `EA → 行号` 映射**，不改锚。

### L3-sse-01 · `movsd` 按**操作数形态**分派；MXCSR **只**建模 IE（其余一律不建模）
- **取舍**：① `F2 0F 10 / 11`（SSE2 标量 double `movsd`）与 `A5`（串操作 `movsd`）共用 Zydis 助记符，而 `F2` **不置** Zydis 的 REPNE 属性（实测 `rep=00`）⇒ 前缀拿不到信息，只能按**操作数形态**分派：可见操作数里出现 XMM ⇒ `Op::Sse` + `SseOp::Movsd`（`apps/emulator-lift/src/runtime/decode.cpp` 的 `sseMovsdByOperands()`）。⛔ 不把 `movsd` 登记进 `kSseMap`：那张表按**助记符**查，会把 `A5` 串操作也一起吞成 SSE。② MXCSR 只保留复位值 `0x1F80`（异常全屏蔽 + 就近舍入）并**只**置 IE（bit0，`src/runtime/exec_fpu.cpp` 的 `sseRaiseInvalid()`）；DE/ZE/OE/UE/PE、掩码位、RC、`#XM` 投递**一律不建模**。
- **备选**：① 按前缀分派（`F2` vs `F3`/`A5`）；② 把 `movsd` 塞进 `kSseMap`；③ 给 MXCSR 建完整寄存器模型并投递 `#XM`。**没选** ①：实测 `F2` 不置 REPNE ⇒ 前缀没有可用信息。**没选** ②：会把 `A5` 串操作（语料 `rep movsd` 267 + 裸 `movsd` 12 条，见 `docs/s4-fpu-audit.md` §1）静默改成 SSE。**没选** ③：今天**没有消费者**（见下读数）—— 建了也只是"看起来对"的模型，改坏了没人知道。
- **读数**（2026-11-09，`apps/emulator-lift` 的 `macos-arm64` 预设 = **RelWithDebInfo**，`out-debt` 树）：
  - `rg -n 'ldmxcsr|stmxcsr|fxsave|fxrstor' apps/emulator-lift/src` ⇒ **1 行，且是注释**（`src/runtime/x86.h:188` 的"本仓没有 …"）⇒ **0 个实现**。
  - 同一 grep 打到语料 `.lst`（`corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst`，18,418,951 字节）⇒ **0 行**；`rg -c 'xmm[0-9]' <lst>` ⇒ **0**（语料里一个 XMM 操作数都没有）。
  - 解码器侧：`rg -n 'ldmxcsr|stmxcsr|fxsave' apps/emulator-lift/src/runtime/decode.cpp` ⇒ **0 行** ⇒ 闭集合外的助记符一律 `Op::Other`，执行即 `unimplemented()` **响亮失败**（机制守卫：`tests/dispatch.cpp` 的 `unimplemented-is-loud`）。
  - `rg -n 'mxcsr' apps/emulator-lift/src/runtime/audio.h apps/emulator-lift/src/runtime/audio.cpp` ⇒ **0 行**：`AudioEngine::Context`（`audio.h:200-211`，含 `r[8]/eip/eflags/seg/fpu/xmm`）与 `capture()` / `restore()`（`audio.cpp:506` / `:521`）**没有** `mxcsr`；pump 借用主 `cpu_`（`attach(X86* …)`，`audio.h:100`）。
- **重写必须保持不变**：① `movsd` 的分派按**操作数形态**（⛔ 不按前缀、⛔ 不按助记符查表）；② 串操作 `A5` 永远留在 `Op::Movs`；③ MXCSR 复位值 = `0x1F80` 且只置 IE（`comiss` / `ucomiss` 的 QNaN / SNaN 差别靠它）；④ 未建模的异常**不投递**、也不假装置位。
- **重开条件（三条，任一条成立就重开）**：① 语料或 `src` 出现 `ldmxcsr` / `stmxcsr` / `fxsave` / `fxrstor`（机械触发器：assets 档 `corpus-mnemonics` 会红）；② 要投递 `#XM` 或实现掩码 / 舍入控制（RC）；③ feeder 可达任何 XMM 指令 —— 那时 `AudioEngine::Context` **必须**一起加 `mxcsr`，否则 pump 路径会**静默**泄漏 IE（save/restore 借的是主 `cpu_` 的寄存器）。

### L3-graphfmt-01 · 交互图格式：**保持行写法**；⛔ 不换 YAML/TOML/JSON（要省维护就 table 化解析器）
- **取舍**：`--interaction-graph` 的输入**只保留行写法**（`node` / `when` / `action` / `next` / `timeout` / `expect`）。
  `docs/03-emulator/interaction-model.md` 是唯一 schema；JSON 只是**等价展示**，文件入口留到"生成器落地"时按原计划
  作为**第二入口**加，⛔ 不取代行写法。
- **备选**：YAML（yaml-cpp）/ TOML（toml++）/ JSON（nlohmann，emulator 共用 baseline 里**已有** 3.12.0）。**没选**，三条读数：
  - 换格式只删得掉**词法层 ~110 行**（`interaction_graph.cpp` 1154 行的 9.5%）；剩下 ~250 行是**闭集合的语义校验**
    （未知键 / 重复键 / `next` 不存在 / `click center` 无框…），换格式照样得写；而标准库**不给行号**：
    nlohmann 对重复键**静默取最后一个**、未知键**直接放行** ⇒ 现有 13 条 `origin:行:` 断言里 12 条是语义错，
    其中两条的失败类会**无声消失**。
  - 最大的一次真实改动（`e4a5457` 加一个按钮词）动了 7 个文件 +292/−30，**解析器只占 41 行（14%）**，测试占 194 行
    —— 维护成本不在词法，在"闭集合语义 + 用例"。
  - **64 位 hash 是硬伤**：`0x94CB492BF7F1AEEB` = 10721743791077830379 > INT64_MAX 且 > 2^53。JSON 里 `0x…` 非法；
    十进制在 nlohmann 精确，但 **JS `JSON.parse` 会变成 …830000（差 277）**；TOML 的 int64 装不下
    （Python tomllib 接受 / toml++ 拒绝 ⇒ 同一份文件不同实现语义不同）。唯一安全写法是**带引号的 `"0x%016X"` 字符串**。
- **重写必须保持不变**：① 解析错误带 `origin:行:` 且**逐条**（13 条断言）；② trace 逐行可 diff；
  ③ hash 一律**当字符串**走（复用 `parseUnsigned` 的溢出检查）；④ 真源只有一份 —— 将来加第二入口时两边**共用同一张闭集合表**。
- **重开条件（四条全真才值得换）**：图以**生成物**为主（>50%）· schema 长出**嵌套** · 有**第三方**消费者 · **行号**不再是硬约束。
  今天的读数四条全假（3 份手写图、schema 是单层、无外部消费者、13 条行号断言）。

### L3-mutate-01 · 变异自检的超时：等待循环必须**异步**（POSIX 上 zombie 不会被同步循环收尸）
- **取舍**：`tools/mutate-check.mjs` 的 `runTimedCommand` 用 **async** 等待（`await sleep(25)`）+ `events`/`exit` 语义；
  超时后用 `killProcessTree` 收整棵；**wrapper 崩了（`gone`）也要收树**，Windows 上靠 **wrapper 写下的子进程 pid**
  （`taskkill /T /PID <ctest>`），POSIX 上进程组已经够。
- **备选**：保持 `sleepSync`（`Atomics.wait`）同步等待，只把 deadline 收窄。**没选**，因为同一根因还带着两个缺陷：
  ① 正常结束也白等 2 s（trivial 命令 2094 ms；81 条 CTest × 3 次调用 ≈ 486 s 推算）；
  ② wrapper 在写退出码前死 ⇒ 被误报成 `timedOut`（`timeoutSec:1` 实测 6036 ms）。
- **为什么同步不行（机制，2026-11-10 实测）**：`Atomics.wait` **阻塞事件循环**，而 libuv 的 `uv__chld → waitpid`
  是事件循环里的 watcher ⇒ 被 SIGKILL 的孩子停在 **zombie**（`exitCode`/`signalCode` 恒 null、`'exit'` 在解除阻塞那刻
  才触发），而 `kill(pid,0)` 对 zombie **仍然成功** ⇒ "等它消失"永远看不到消失。
  同一原因让 `restoreOnSignal` 在这之前是**死代码**（`Atomics.wait` 与 `spawnSync` 期间 SIGINT 根本不派发）。
- **重写必须保持不变**：① 超时**不等它自己结束**（墙钟预算 = `timeout + 余量`，⛔ 不是"睡够 sleep 时间"）；
  ② 超时/崩溃**都要收整棵进程树**（只杀壳会留下继续烧 CPU 的孤儿）；③ 还原 + 全文比对 + journal 的写序不变；
  ④ `--help` 里 `2 = 还原失败 / 内部错误`，⛔ 内部错误不许落 0。
- **已知残余**：`restoreOnSignal` 里"杀掉在飞 wrapper"**没有机械守卫**（要真 CTest + 真 Ctrl-C）；Windows 侧未实测。
