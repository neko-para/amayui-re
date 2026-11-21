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

### L3-arena-01 · surface arena 的容量是**自定模型**（256 MiB）与剩余上限风险
- **取舍**：D3D9 纹理 / 后缓冲 / 深度放**独立的** `GuestHeap` = `GuestLayout::arena_base` 起 `GuestLayout::arena_size`（唯一真源 `src/runtime/image.h`；两臂 `runtime/machine.cpp` 与 `aot/aot_host.cpp` 都引用它，⛔ 不再各写一个字面量）。起点同时是 `cpu_.shim.va_limit`（VirtualAlloc bump 区的上界贴着 arena 基址，两臂同一条口径）。
- **容量为什么是 256 MiB（2026-11-20 改）**：真机的上限由驱动 / 显存决定（`external-docs.md` 偏离 #10 自己就这么写）⇒ 只能按"这个 EXE 用得到"给固定区。`saves/SAVE07.DAT`（第二个地图、即将战斗）→「结束回合」→「決定」把 128 MiB 顶穿了：进战斗前 `in_use=119,343,064 B / 340 surfaces`，还要一张 2048×2048 页（含块头 16,777,224 B）⇒ **下限 136,120,288 B（129.81 MiB）**，128 MiB 差 1,902,560 B ⇒ `stop : resource-exhausted @ eip=0047839D`（6/6 稳定）。抬到 256 MiB 后同一配方 4 跑（verify-01/03/04/05，含逐帧窗口截图与 guest 后缓冲 dump）：`region : arena in_use=92,662,952 peak=141,169,728 surfaces=110`（peak = **134.63 MiB**，余量 **121.37 MiB**），`stop` 变成 `stopped @ eip=00412383（正常结局）— input-plan: quit`，战斗界面出现在第 250 帧（`--present-screenshot` / `--dump-frame` 存图）。
- **备选**：① 与主堆共用；② **不抬容量**（在 shim 里补释放 / 把请求改小 / 降精度）；③ 动态扩。**没选** ①：**"纹理没回收"在主堆那行完全看不见** ⇒ BUG-01 定不出来。**没选** ②：像素块的生命周期由 guest 的 `Release` 决定（`ComRegistry` 的 `on_zero` 是唯一收敛点），在 shim 里补释放 = 越界回收别人还在用的像素（真机不会发生）；把请求改小 / 降精度 = 改 guest 可见语义。**没选** ③：打破"区间固定 ⇒ 可复现"。
- **代价（必须一起改的那一处）**：arena 新上界 `0x38000000` 吃掉了旧的 AGERC fallback base `0x30000000` ⇒ fallback 搬到 `GuestLayout::agerc_fallback_base = 0x40000000`（与上界留 128 MiB 余量）。⛔ 只抬容量不搬 base = `loadAgercModule` 的 `fits()` 拒 = **启动即响亮报错**（看起来像"扩容把启动搞坏了"）。守卫 = CTest `region-layout`（`tests/region_layout.cpp`，pure / 顶层无条件注册）：所有命名区间两两不相交 + fallback base 不在任何区间里且 ≥ arena 上界。
- **剩余风险（仍是上限、不是保证）**：① 更大的工作集（更多常驻图集 / 更长的场景链）仍会耗尽 —— 256 MiB 只覆盖"一份屏工作集 + 这一场战斗的装载"，不是"以后都不会再耗尽"；② 128 MiB 时代 arena 同时是"漏回收"的**意外探测器**（BUG-01 就是这么抓到的：退回 TITLE 后 126.7 MB / 最大空闲 5.8 MB）—— 抬到 256 MiB 后一次 16 MB/屏的泄漏要更多屏才炸 ⇒ **今天能红的东西变静默**，对冲 = 换屏 `in_use` 不增长 / 回基线的守卫（`tests/option_roundtrip.cpp`、`tests/d3d9.cpp` 的 `texture-bound-then-guest-release`）不能省。
- **重写必须保持不变**：① arena 独立、区间固定（基址 / 大小只从 `GuestLayout` 读，⛔ 不许在 cpp 里另写字面量）；② arena 与 AGERC 的 fallback base **不相交**；③ 摘要必须同时给 `in_use`/`peak`/`largest-free`；④ 失败要报 `w×h / D3DFORMAT / 请求字节 / 最大空闲块`。

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
> ⚠ 2026-11-16：这条取舍的**载体**换了 —— 入口 `--interaction-graph` 已退役，配方现在是
> `scripts/recipes/*.plan`（行写法，`runtime/recipe.h`）；`node` / `when` / `action` / `next` / `expect`
> 那套**节点语法**早已随顺序模型删掉。下面「只保留行写法、不换 YAML/TOML/JSON」的结论仍然成立。
- **取舍**：配方输入**只保留行写法**（当时是 `node` / `when` / `action` / `next` / `timeout` / `expect`；
  今天见 `runtime/recipe.h`）。
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

### L3-mdct-01 · 语音起播的 FFT/MDCT：**数值容忍档**（追"1 ulp + 可差分"，不追逐位）
- **取舍**：`sub_4E0B40`（libvorbis `mdct_backward`）整函数 native 化 —— 纯核逐行照抄 xiph vorbis
  **v1.0.1 `lib/mdct.c` v1.32**（`mdct_backward` + `mdct_butterflies` + 四个蝶形 + `mdct_bitreverse`），
  中间值用 `double`、落盘 `float`；guest 入口只做 9 条准入（`init` / `n` / 幂次 / `log2n` / `in` / `out` /
  `trig(n+n/4)` / `bitrev(n/4)`），失败一律回退解释器。判据 = **真 guest 字节的解释器臂当 oracle**
  （`tests/native_override_assets.cpp` 的 `mdct/*`，`max|Δ| ≤ 1e-6 * peak`；实测 1.0e-7…1.5e-7）。
- **备选**：① 像 `sub_4CCC60` 那样走 SoftFloat 逐位 —— **没选**：一包 4096 个 x87 操作，softfloat
  每个操作都要构造 `Ext80` + 舍入，会比解释器还慢，换来的只有"可逐字节 A/B"，而用户已明确**音频可接受偏差**；
  ② 不复刻循环、直接写 IMDCT 数学式 —— **没选**：窗口/折叠约定藏在 `trig` 表里，重建约定的风险高于照抄，
  且 oracle 之外没有任何额外收益；③ 整段 `sub_4E2A30`（`mapping0_inverse`）native 化 —— **没选**：那要把
  Huffman + 码本解码（同一窗口实测 251 ms，比 MDCT 更大）一起重写，是另一个量级的活，另立单。
- **偏差的真正来源（⛔ 不是 "x87 80-bit vs double"）**：跑这条链的是 **feeder 线程**
  （`AudioEngine` 从 `kSub4B6C30` 起跑、并把 override 表拷过去；`audio_cpu_.fpu.reset()` 把控制字设回
  **0x037F = 80-bit**），所以寄存器段是 80-bit；guest 的 `REG_TYPE` 虽是 `float`，编译后**只在溢出槽**
  （`.lst` 0x4E02F7 / 0x4E0308 / 0x4E0626 的 `fstp [ebp+var_*]`，dword）才舍入到 32-bit。
  实测评据：把夹具精度改成 53-bit 重跑，七个用例的 `max|d|` **一个数都不变** ⇒ 主导项是溢出槽。
- **为什么中间值用 `double` 而不是 `float`**：`float` 会把**所有**中间量压到 24-bit（比 guest 差一个
  量级）；`double` 只有寄存器段的 53-bit 差异（1e-16，远小于溢出槽的 1e-7）且不复现溢出点 ⇒
  实测 `max|Δ|/peak` = 1.0e-7…1.5e-7（n=64…8192，容差 `1e-6`，余量 ~6.7×）。⚠ 夹具的 `fpu.control` **保持 bare Machine 缺省
  0x037F**，别改成 0x027F：那是主线程 CRT 的精度，被 feeder 的 `fpu.reset()` 盖掉了。
- **重写必须保持不变**：① `in == out` 的**原地**语义（真实调用点 `0x4E2EAA` 两个参数是同一个缓冲）；
  ② 输出**不依赖 `out` 的旧内容**（实测：预填 `777.0f` 与 `-999.0f` 两臂逐字节相同 ⇒ 原地调用安全）；
  ③ 读边界：`trig` 最远到 `n + n/4`（`mdct_bitreverse` 从 `trig+n` 起走 `n/4`），`bitrev` 取值域
  `[0, n/2-2]`（真表范围：偶数项 `[2, n/2-2]`、奇数项 `[0, n/2-4]`，最大值 `n/2-2` 在 `[0]`）
  ⇒ `mdct_bitreverse` 只在**已写过的那半段**里读（⚠ 编一个含负值的假表会让它读未初始化区、输出随机
  —— 实测踩到过；准入因此取真表范围而不是更宽的"内存安全界"）；④ 9 条回退都在**任何写之前**；
  ⑤ 判据的 oracle 必须是**解释器臂**（⛔ 不是手写参考实现：那会让"两边都错"看不出来）。
- **⚠ 落地时踩到的两个假绿（比实现本身更值得记）**：
  ① **准入判据凭直觉收紧 ⇒ 生产永远回退、而夹具全绿**：`bitrev` 的真表是 `((~acc)&mask)-1`，
     取值 `[0, n/2-2]` 且 **`[0]` 就是最大值**。一度写成 `[0, n/4)` ⇒ 每个 n 立刻 decline（`all` 里这项
     `hits=0`），而夹具表落在窄区间内 ⇒ 两臂差分照样绿。⇒ 判据的边界只能按**真实读取**推
     （`x0 = out+n/2+bit` 读 `x0[0]/x0[1]` ⇒ `[-n/2, n/2-2]`），且**夹具表必须覆盖真表的范围**。
  ② **数值判据要单独数 NaN/Inf**：`if (d > maxdiff)` 对 NaN 恒 false ⇒ 一个 NaN 输出能让整条
     `max|Δ|` 判据静默通过（实测：纯核末尾写 `out[0] = NaN`，旧判据一位不变、全绿）。现在的做法是
     逐值 `isfinite` 计数并断言 0。
- **已知残余**：`EAX` 登记为 0（guest 结束时它是 `out` 派生指针的残留，Hex-Rays 因此猜成 `float *`；
  唯一调用点 0x4E2EAA 之后不读它）、不复刻 x87 的 PE/UE 粘性位、相位偏移照旧；
  可用 `--native-overrides all,-sub_4E0B40` 隔离。

### L3-ftol-01 · `__ftol2_sse`：**小方案**（一次 softfloat 截断 + 一次 f32 转换），不逐条重放
- **取舍**：`__ftol2_sse`（`0x4E82C0`，MSVC 的 x87 → int64 截断辅助）做成函数级 native override。
  入口 28 B 是 `cmp ds:dword_55F2E8,0 / jz 0x4E82F6`；本线 `IsProcessorFeaturePresent` shim 恒 0
  ⇒ guest 永远走 `0x4E82F6 = __ftol2`（117 B）。实现 = `x87ToI64(st0, truncate=true)` +
  **一次** `x87RegToMem(st0, scratch, 4)`（那 4 字节 = guest 的 `0x4E8301 fst dword [esp+18h]`，
  PE/UE/OE 也从这里出）+ 弹 1 层 + tag/状态字同步。
- **为什么不需要"照 33 条指令重放"**：`0x4E8305 fistp` 按 CW 的 RC 就近取整，`0x4E8319..` 的修正环
  （`add 7FFFFFFFh` + `sbb/adc 0`，借 `D9 1C 24 fstp dword [esp]` 的 f32 符号位）把它拉回**向零截断**
  ⇒ **最终值对 RC 无关**。硬约束只剩两条：① 值必须走 **softfloat** 的 trunc（宿主 `(int64)(double)`
  在 `|x| ≥ 2^53` 差一位，越界还会给 `0x80000000` 而不是 `0/0x80000000`）；② 复刻 `fst dword` 的副作用。
- **闸门 4 + 1 条**（全部在任何副作用之前；不做窄档）：`[0x55F2E8] == 0` **且已提交**（非 safe mode 的
  `rd32` 读未提交页会静默给 0）· `st(0)` 非空（判据是 `empty(phys(0))`，⛔ 不是 `empty(0)`）·
  x87 空闲槽 ≥ 2（`8 − used ≥ 2`，方向别写反）· 栈 scratch `[align16(esp−0x24), +0x20)` 已提交 ·
  **越界边界**（见下）。
- **越界边界那一条为什么存在**：guest 的 `fistp` 只在**取整值**放不进 int64 时落不定值 + IE；
  我们的 trunc 判据是"**截断值**放不进"。两者只在 `x ∈ (2^63−1, 2^63)` 上分叉，而该区间内唯一可表示的
  ext80 值是 `2^63−0.5`（区间内 ulp = 0.5）：guest 就近成 2^63 ⇒ `0/0x80000000` + IE，trunc 给
  `0xFFFFFFFF/0x7FFFFFFF`。⇒ 那一格交解释器（纯档用例 `ftol/decline-no-write` 的
  `boundary-2^63-minus-half`；同一条边界上 `x = 2^63−1` **必须**接管，否则这条闸门就宽到挡正常值）。
- **返回值通路**：本项是**唯一**写 `EDX` 的函数级 override（`EDX:EAX` = 64 位截断值，`2^31..2^63` 是
  低位回绕、**不 clamp**；`|x| ≥ 2^63` / NaN / ±Inf ⇒ `EAX=0, EDX=0x80000000`，⛔ 不是 `0x80000000/0`）。
  `native_overrides.h` 的契约文字因此从"只有 EAX 一条返回通路"改成"按 ABI 写返回值"，并写明
  "写 EDX 必须在所有闸门之后"。
- **oracle 是解释器，不是真机/SDM**：解释器的 `fistp` 走 `exact=false` ⇒ **不置 PE**，`fld`/`fild` 也
  不置 C1 ⇒ 按 SDM 去"补"PE/C1 反而会把 `fpu.status` 逐位判据弄红。判据 = 与**解释器**逐位一致。
- **已知偏差（登记，不是缺陷）**：① 时钟相位（函数级只记 1 条指令；`clock->tick` 在 thread 模式的
  feeder 里是 no-op，pump 模式下设备与 guest 计时同源 ⇒ 跳 tick 是一致加速；本项**不补 k**）；
  ② guest 那 20 B 栈 scratch 里我们只写 `[E+18h,E+1Ch)`：`[E+10h,E+18h)`（按 RC 就近的 int64）与
  非零支的 `[E,E+4)`（`f32(x−n)`）不复刻 —— 它们是 caller 栈帧以下的死 scratch；③ 被消费掉的物理槽
  残留字节不同（guest 留 `x−n`、native 留 `x`），tag 已置 empty ⇒ 不是 guest 可观察状态；
  ④ 不复刻 C0..C3 以外的 **EFLAGS**（ABI 上 call-clobbered）与 x87 未屏蔽异常 trap（本线不投递）。
- **已知不覆盖（登记，不修）**：`0x4B5545 jmp __ftol2_sse`（`sub_4B5500` 的 qsort 比较器尾跳）——
  dispatcher 只在上一条是 `call` 时拦（`call_target`），所以这条**既不 `+hits` 也不 `+fallbacks`**；
  另有 `0x4BC539` / `0x4BC680` 直连 `__ftol2`（真消费 `EDX:EAX`，不经 `0x4E82C0`）。
  ⇒ "hits 与 385 个 call 站点吻合"只是**下界**（`0x4E82C0` 实际有 386 个入口）。
- **收口读数（按 audio mode 分开读，⛔ 别混）**：两种模式下 hit 记在**两张不同的表**上 ——
  ① `--audio off` / virtual（pump 模式：`sub_4B6C30` 从**主线程**被嵌套调用）⇒ 命中的是主表，
     读启动摘要里的 `native-ovr:` 那一行（`__ftol2_sse` 的 hits / fallbacks）；
  ② 真 SDL 设备（thread 模式）⇒ feeder 用的是 `audio_overrides_ = *cpu_->native_overrides` 的**表拷贝**
     （`audio.cpp` 的 `runFeederGuest`）⇒ `native-ovr:` 里这一项**恒 0**，要看 feeder 自己的计数：
     `LIFT_AUDIO_TRACE=1` 时它逐次打 `[audio] feeder fill … ov=<n>`（feeder 表里各项 hits 之和）。
  ★ 两边的读数**不可互相印证**（同一份 hits 只会落在其中一张表上），所以收口必须先声明用的是哪个
  mode；`dumpSummary` 目前**不**包含 override counts（那是音频线的落点，不在本项范围）。
  ⚠ 读数不进 test（每台机器的数字都会变），这里只登记"怎么读"。

### L3-cursor-warp-01 · 宿主光标 warp：**减 guest 窗口原点、不乘 pixel density**；OS 回灌在 motion 入口按窗口像素 FIFO 吞掉
- **取舍**：`--cursor-warp`（缺省关）让 guest 的 `SetCursorPos` 除了改虚拟光标，还经系统接口
  （Win32 `::SetCursorPos` / macOS `CGWarpMouseCursorPosition`）真实移动宿主光标。它只加宿主副作用：
  **除 ③ 的返回值外**（开着但挪不动 ⇒ `FALSE`）guest 可见状态逐位不变 —— ①关 / ②挪成功 两种情况
  都不变，③ 是裁决的例外（返回值本身就是 guest 可见状态；⛔ 不假装成功）。契约见
  `contracts-win32.md` 的 L1-MSG-05。平台原语在 `runtime/host_cursor.cpp`
  （单 TU 按 `#ifdef` 选实现），几何在 `runtime/cursor_map.cpp`（与鼠标事件入**同一份** letterbox）。
- **为什么必须减 guest 窗口原点**：本线有三套空间 —— G：guest 屏幕（1280×720，`SetCursorPos` 吃这个）、
  C：guest 客户区（`C = G − (win.x, win.y)`，`ClientToScreen` / `WM_MOUSE*` 的 lParam 用这个）、
  H：宿主窗口客户区（letterbox 后的画面）→ 宿主屏幕。呈现的画面只对应 **C**，而 guest 把客户区原点
  放在屏幕 `(win.x, win.y)`（`HostWindow.x/y`，`CreateWindow` 给的 guest 坐标）。所以 G → H 必须先经
  `WindowManager::cursorClientPos`（`host_window.cpp` 里 `cursor_x_ − w->x` 的**现成反函数**，与
  `setCursorClientPos` 严格互逆），⛔ 不要自己写 `- w->x`、⛔ 不要新增字段。
  ★ 漏掉它的后果会被 OS 回灌放大成可观测 bug：warp 之后 Win32 回灌一条 motion ⇒ `mapMouse` 再
  **加一次** `win.x/win.y` ⇒ `GetCursorPos` 比 `SetCursorPos` 偏一整个窗口原点。引擎那两处"屏幕中心"
  调用点后面紧接 `SetWindowPos(hwnd,-1,0,0,0,0,3)`（win=(0,0)）⇒ 真机上**看不见**，只能靠非零原点的
  守卫（`tests/cursor_warp.cpp` 的 `on-calls-once-with-client-coords` / `no-clamp`）抓。
- **为什么⛔ 不乘 `SDL_GetWindowPixelDensity()`**：`SDL_GetWindowSize` 给的是**窗口坐标**里的客户区
  尺寸，鼠标事件的 `x/y` 明说 "relative to window"，`SDL_GetWindowPosition` 也在同一空间（Win32 实现 =
  `ClientToScreen(hwnd,{0,0})`）⇒ 三者同空间，天然不需要 DPI 换算。Win32 侧 SDL 缺省声明 permonitorv2
  ⇒ 窗口坐标与 `::SetCursorPos` 的物理像素虚拟屏是**同一空间**；macOS 侧 SDL 窗口坐标是 **point**，
  `CGWarpMouseCursorPosition` 也吃 point ⇒ 再乘 pixel density 就是**双重换算**（Retina 上偏 2×）。
  ⇒ 全链只缩放 `scale`（画面→窗口客户区），它与 DPI / Retina 无关。
- **为什么 letterbox 的 image 尺寸必须进签名**：present 的 letterbox 跟的是**当前帧**尺寸
  （`SDL_SetRenderLogicalPresentation(…, w, h, LETTERBOX)` / `SDL_SetWindowSize(w,h)`），后缓冲可以是
  15 种适配器模式之一；原先反向的 `mapMouse` 硬编码 1280×720 ⇒ 非 1280×720 时鼠标映射系统性偏移。
  现在 `letterboxFor(window_w, window_h, image_w, image_h)` 是唯一一份，`mapMouse` 与 warp 共用；
  image 取最近一次 Present 的帧尺寸（renderer 路径 `tex_w/tex_h`、直出 `direct_w/direct_h`，还没有帧 ⇒
  `1280×720`）。★ **措辞降级**："修掉了既有的非 720p 偏移"只在 **guest 客户区尺寸 == 帧尺寸** 时成立；
  guest 建 1280×720 窗口、后缓冲却换成别的适配器模式（两者不等）时没观测到过 —— 那时这里把窗口像素
  当帧坐标系，会从"修正"变成"回归"（已知风险，`OBS-20261114-08` 的唯一真机观测是两者相等）。
  不是 720p 的偏移在 `present` 用 `SDL_SetWindowSize` 把窗口设成帧尺寸时本来就不可见。
- **为什么回灌抑制搬到 SDL motion 入口、且是窗口像素 FIFO**：warp 之后 Win32 会把同一次移动回灌成
  SDL motion ⇒ `mapMouse` 把它映射回 guest 客户区 ⇒ `input::mouseMove(Os)` 会反写虚拟位置，还会多 post
  一条带**宿主墙钟**时间的 `WM_MOUSEMOVE`。抑制点原先在 `WindowManager::setCursorClientPos`（单槽 +
  guest 坐标 ±1），复审实测两个洞：① **单槽被后一条 warp 覆盖** —— 同一帧两次 `SetCursorPos` 时第一条
  回灌无人认领，被当成真人移动接受；② **guest 坐标里比挡不住越界值** —— warp 落点经 `mapMouse` 的
  `windowPixelToImage` clamp 到 0..image-1（复审反例 B：1600×720 窗口 + image 1280×720、
  `SetCursorPos(1300,300)` ⇒ 回灌被 clamp 成 1279，差 21 px）。而 `WindowManager` 拿不到 **clamp 之前**
  的窗口像素 ⇒ 抑制点搬到 `present_sdl.cpp` 的 `SDL_EVENT_MOUSE_MOTION` 入口（`runtime/warp_echo.h` 的
  `WarpEchoQueue`）：`hostCursorWarp` 成功后按 `imageToWindowPixel` 的 `wx/wy` 登记（窗口客户区像素），
  motion 到达时 FIFO 匹配（±2 px）、命中即整条丢弃（不 `mapMouse`、连消息也不发）、每条只消费一次；
  **只吞 motion** —— 按键 / 滚轮 / 键盘一律放行，且**不**清期望：第二轮复审又抓到一个同型的窄洞
  （一条在 warp **之前**入队、warp 之后才排空的键事件会把刚登记的期望抹掉 ⇒ 回灌被当真人移动接受），
  所以老化只靠"每条只消费一次 + 容量 8（满丢最旧）"；代价是一条陈旧登记可能吞掉一次 ±2 px 内的真人
  移动（只吞一次，且只在开关开着时存在）。⛔ 不做长期去抖
  （真人的移动必须照旧生效；`tests/present_window.cpp` 的 P3 仍断言真人移动产生消息）。`WindowManager`
  因此回归纯 guest 状态（回灌抑制的字段 / 方法与 `setCursorPos` 里的清理全部删掉）。
- **为什么第一次成功 warp 后读回一次**：Windows 的 RDP / 锁屏等场景 `SetCursorPos` 可能"成功但没动"，
  返回值得不到真相 ⇒ 第一次成功之后 `hostCursorQueryScreen` 读回，与目标差 >1 px 只 warn 一行
  （含目标 / 实际 / scale / off / window 位置），此后不再查。⛔ 不自动纠正 —— 那会把映射 bug 掩盖成"能跑"。

### L3-tls-01 · SoftFloat 的消费方必须**强制前置** `platform.h`（漏了是**静默**错）
- **取舍**：`third_party/berkeley-softfloat/CMakeLists.txt` 给**消费者**挂
  `target_compile_options(berkeley-softfloat INTERFACE /FIplatform.h)`（非 MSVC 臂 `-include platform.h`；
  用**相对名**，消费者在别的目录编译）。⛔ 不动 vendored `.h` / `.c` 一个字节，⛔ 不把 `THREAD_LOCAL` 退回空。
- **漏了会怎样（这是它必须被守住的原因）**：`source/include/softfloat.h:53-54`（与
  `source/softfloat_state.c:43-44`）有 `#ifndef THREAD_LOCAL ⇒ #define THREAD_LOCAL`（**空**）的兜底 ⇒
  任何 TU 若**先** include `softfloat.h`、没先 include `platform.h`，`extern uint_fast8_t
  softfloat_exceptionFlags;` 被当成**普通静态变量**：**编译 + 链接都过**，运行时读的却是 TLS **模板**
  （库写的是**每线程 TLS 块**）⇒ 库置的位在消费方**看不见**，读到的是垃圾/陈旧字节。
  复审在 macOS 上实测读到 `flags=248`（`REQ-01M773EFSYYE97YKEA6TQEQ53J` 正文）。今天没有这样的 TU，
  但"下一处 include"就会中 —— 没有编译错误、没有链接错误、没有崩溃。
- **实测读数**（2026-11-14，`windows-x64` 预设 = Ninja + clang-cl 22.1.2 + RelWithDebInfo；
  `cmake --build --preset windows-x64` + `ctest --test-dir out -R "softfloat|fpu" --output-on-failure`）：
  - **无**前置 include（本次改动之前 / 把 `INTERFACE` 改成 `PRIVATE`）：链接**成功**；
    `softfloat_raiseFlags(softfloat_flag_inexact)` 之后消费方读 `flags=0`（期望 1）；
    两条宿主线程各写 `0xA1` / `0x5C`，barrier 之后**两边都读到 0x5C**，且
    `&softfloat_exceptionFlags` **同一个地址** `0x00007FF6F5668003`；`softfloat-tls` 11 条断言 7 条红。
  - **有** `/FIplatform.h`：任何 include 之前就看得见 `THREAD_LOCAL = __declspec(thread)` 与
    `LITTLEENDIAN`；`softfloat_raiseFlags` 置的位在本 TU 读得到（`via-library=1`）；两条线程分别读回
    `0xA1` / `0x5C`、地址 `…92C3` / `…8FA3` **不同**；11 条断言 0 条红。
- **备选**：① 在 `softfloat.h` 的空兜底处改 `#error "include platform.h before softfloat.h"`（编译期就炸）。
  **没选**：要动 vendored 字节（本仓纪律：`third_party/**` 只允许改 `CMakeLists.txt`），而且它会**惩罚**
  所有现存 TU 的 include 顺序、把"排序约定"变成编译错误 —— 前置 include 是同一判据的**零侵入**版本。
  ② 靠 code review / 约定"每个 TU 先 include `platform.h`"。**没选**：那是"没有守卫的清单 = 一份 Markdown"，
  漏了是**静默**的（编译、链接、单线程自检全绿）。
- **怎么被守住**：CTest `softfloat-tls`（`tests/softfloat_tls.cpp`，顶层无条件注册 ⇒ pure 档）——
  它的**第一条 include 就是 `<softfloat.h>`**、⛔ 不 include `platform.h`（= 正是要守的那条路径），
  断言三件：① 任何 include 之前 `THREAD_LOCAL` / `LITTLEENDIAN` 已在场；② 库写的位本 TU 读得到；
  ③ 两条线程各写不同值、barrier 之后各读各的、且两个 `&` 不同。变异 = `INTERFACE` → `PRIVATE`
  （`tools/mutate-check.mjs` 的 `ctest:softfloat-tls`）⇒ 当场红。另有 CTest `fpu-thread-flags` 守
  "`THREAD_LOCAL` 本身必须是真的 TLS"（把 `platform.h` 的三档定义改回空 ⇒ 红）。
- **重写必须保持不变**：① vendored 字节不动（要么 vendored，要么改 `CMakeLists.txt`）；
  ② **所有** softfloat 消费者都拿到前置 include（含新加的测试 TU 与将来任何 `-include` 之外的新目标）；
  ③ 前置 include 用**相对名**（绝对路径会把构建绑死在某一台机器上）；
  ④ 带 `<softfloat.h>` 的 TU 一律先看见 `platform.h` 的 `LITTLEENDIAN`（它参与 `extFloat80_t` 的字段顺序，
  漏了不只是 TLS 问题）。

### L3-seh-01 · 非连续异常 + `disposition=1`：**响亮停**，不建模"改抛后的第二次派发"
- **取舍**：`Seh::step` 在 `Level::Kind::Raise` 上读到 `disposition == ExceptionContinueExecution` 且
  **原 record** 的 `ExceptionFlags & EXCEPTION_NONCONTINUABLE` ⇒ **不装载 CONTEXT**，
  直接停成独立停因 `StopReason::NonContinuableException`（`machine.cpp` 的 `pumpSeh`），detail 逐字点名
  `EXCEPTION_NONCONTINUABLE_EXCEPTION / 0xC0000025`、原 record 的 `code/flags`、以及"CONTEXT 有没有被
  handler 改（改了几个字）"（`REQ-01M72GAA7CVKBPP9QRT0PRPBTM`）。
- **为什么不能照字面"装载 CONTEXT"**：`CONTEXT` 是**抛出瞬间**的快照（`Eip` = `call RaiseException` 的
  下一条）⇒ 装载它 = 从 raise 的下一条接着跑 = **异常被静默丢弃**。本单的现场就是这样一路 `leave;ret`
  落进 `_Xout_of_range` 尾部 padding 的 `int3`，摘要写成误导性的"软件中断（M1 不建模中断表）"。
  ★ 判据是**旗标**不是"CONTEXT 改没改"：本机 x86-32 原生探针（2026-11-14）里，`flags=0x1` 且 filter
  **改了** CONTEXT，系统**照样**改抛 0xC0000025；`flags=0x0` 才允许"改 CONTEXT + 返回 1"继续跑。
  这同时解释了 MSVC 的 `catch` 为什么走 `_JumpToContinuation`（0x4E7007，**不返回**）而不是返回 1。
- **备选**：① 照字面装载 CONTEXT（= 修前，缺陷单的病因）；② **建模改抛**：造一条
  `code=0xC0000025 / flags=1 / ExceptionRecord→原 record / nargs=0` 的新 record，再从 `fs:[0]` 链头
  走一遍完整派发；③ 只记一行日志然后继续往下找 handler。
  **没选** ①：静默丢异常，且把"没人接"伪装成"跑到别处崩了"。**没选** ③：`disposition=1` 的语义
  是"接管了"，继续找等于把**已接管**的异常再派发一次（guest 可能已经拷过 catch 对象 / 改过状态）。
  **没选** ②（本单）：它要把状态机推进到"宿主发起的新 raise"（第二层 record、`ExceptionAddress`、
  `fs:[0]` 不变、以及 guest 那侧 `__CxxFrameHandler3` 对**非 C++ 异常**的 `__except` 搜索 / 可能的
  `_UnwindNestedFrames`）—— 那是**另一条**判据面（判据 2 的观测点：`0x4137B7` 附近 / `_CallCatchBlock2`），
  而且今天**没有可复现的现场**（触发路径已被字体单修掉）⇒ 先修判据 1，把它单列。
- **重写必须保持不变**：① 任何 `NONCONTINUABLE + disposition=1` 的路径都**不许**落到"装载 CONTEXT
  继续跑"（判据是旗标，⛔ 不要加"CONTEXT 没改才算"的补丁）；② 停因必须与"链上没人接"
  （`unhandled-exception`）**分开**，detail 必须能指名道姓到 `0xC0000025`；③ 宿主只读 guest 登记节点的
  `[pRN]` / `[pRN+4]`，⛔ 不写 `[pRN-4]` / `[pRN+8]` / `[pRN+0xC]`（`_JumpToContinuation` 读 `[pRN-4]`、
  handler 读 `[pRN+8]` 的 try 状态字）—— E 组逐字守着这三个字；④ 重开"建模第二次派发"的条件 =
  出现消费者（可复现的真机现场，或需要让 guest 的 `__except` 接住 0xC0000025）。

### L3-crc-01 · CRC-32 两变体：表**从 guest 读**；`n <= 0` **接管**、`INT32_MIN` **回退**
- **取舍**（`REQ-01M7BY4D1ZWA4HB97T8M8TVEYQ`，实现 `src/runtime/native_crc32.{h,cpp}`）：`sub_436D00`（反射，
  表在 `this+0x400`）与 `sub_436D50`（非反射，表在 `this+0`）挂成两项**函数级** override。纯核只有一个
  `crc32Bytes(table, buf, n, reflected)`；**两张表都从 guest 的 `[this, this+0x800)` 读**（512 个 dword），
  实现里**一个多项式常量都没有**。`[this+0x800] != 1`（表还没建）⇒ 回退，由解释器跑 `sub_436BD0` /
  `sub_436C60` 建表 —— ⛔ 不在 native 里"顺手建表"。
- **为什么不硬编码多项式**（`0x4C11DB7` / `0xEDB88320`）：表是**引擎状态**，不是常量。硬编码只在"`this`
  永远是刚建好的那两张标准表"时才对；一旦哪天有第三条路径重建表（或第三个变体出现），native 与 guest
  就会**静默分叉**，而 CRC 分叉的症状是"存档写出去、读回来对不上"—— 没有异常、没有日志、没有断言会红。
  读 guest 那份内存的代价是每次命中一次 1 KiB `memcpy`（payload 是 KB～MB 级 ⇒ 常数项可忽略），
  换来的是"表怎么变都对"。
- **为什么 `n <= 0` 是接管而不是回退**：`n <= 0` 时 guest 只做 `or eax,-1 / dec edx / js / not eax` ——
  一个字节都不读、一个字节都不写、没有 fault 的可能 ⇒ 没有任何"只能由解释器表态"的东西；
  回退只会白付一次整函数重放（`.lst` 0x43758A / 0x4375A6 / 0x437842 / 0x437858 四个调用点，
  `save-confirm` 窗口实测 d50 4 刀 / d00 4 刀）。
  ★ **但 `n == 0x80000000`（INT32_MIN）不在这条规则里**：`0x436D29 dec edx` 把它变成 `0x7FFFFFFF`
  （SF=0）⇒ `0x436D2A js` **不跳** ⇒ guest 真的跑 2^31 次。所以实现把 `n` 先折成"循环次数"
  （负 ⇒ 0，**除了 INT_MIN ⇒ 0x80000000**），0 才接管；INT_MIN 落到 G4（`[buf, buf+2^31)` 跨 4 GiB /
  未提交 ⇒ 回退）。需求单正文的"`n <= 0` ⇒ 循环 0 次"在这一格上不成立，**按机器走**。
- **备选**：① 硬编码两张标准表（快一点、少一次读）；② `n <= 0` 一律回退（最简单）；③ 连"建表"也在 native
  里做（一次接管整段）。**没选** ①：上面那条静默分叉。**没选** ②：白付热路径上的整函数重放，且
  "接管 0 次循环"是个**可判定**的语义（不是近似）。**没选** ③：建表 = 重新引入多项式，正是 ① 的错，
  而且建表那两支要写 512 个 dword + 一个标志（多一类零写风险）。
- **重写必须保持不变**：① 表**只能**从 guest 读，⛔ 不许出现多项式字面量；② `[this+0x800] == 1` 是接管
  的**必要条件**（失败必须回退，且回退前**零写** —— 差分会把"回退前写过的标志"修回去，所以那条守卫
  只能在 `tests/native_override.cpp` 的 `crc/decline-no-write` 里，不能靠两臂差分）；③ `n == INT_MIN`
  ⛔ 不许当成 0（`dec/js` 的 SF 语义，`crc/int-min-declines` 钉住）；④ 两个变体**不是同一个 CRC**
  （`"123456789"`：反射 = `0xCBF43926` = CRC-32/ISO-HDLC，非反射 = `0xFC891918` = CRC-32/BZIP2）
  ⇒ ⛔ 不要写"两个变体应当给同一个值"的断言；⑤ 守卫 = `native-override-assets` 的 `crc/*`（解释器臂 =
  同一 EA 的真字节 oracle）+ 变异条目（`crc >> 8` → `crc << 8`）。

### L3-sprite-01 · sprite 采样自己正在写的 RT：**跳过这一条 + 记账**（不是整批 / 整帧）
- **取舍**：`d3dx9::compositeBatch` 在逐条目合成前判"要采样的纹理像素基址 == 本批要写的颜色目标像素基址"（`tbase == base`）⇒ **跳过这一条 sprite**、记具名位 43（`kUnSpriteRtFeedbackLoop`，进 `unsupported :`）、`skipped_reasons` 记 `sprite: 采样纹理是正在渲染的 RT（反馈环）`、默认级别一行 `spriteWarn`。⛔ 不读旧像素、⛔ 不丢整帧。
- **为什么 sprite 需要自己的一条闸**：sprite 的合成**不经** `gpuEligible`（`d3dx9_sprite.cpp` 直接调 `GpuRaster::rasterQueue`）⇒ draw 路那条 `kUnRtFeedbackLoop`（bit28）管不到它；而 `REQ-01M79V2TVJH98BKH0C0WQV2FET`（sprite→RT）之后，"同一批 sprite 采样自己正在写的 RT"变成**新可达** ⇒ GPU 读上传缓存 / guest 未读回，两个来源都可能是旧像素，且看不出来。
- **备选**：① 放过去（继续画）；② 整批跳过；③ 整帧丢弃；④ 复用 draw 路的 bit28。**没选** ①：静默旧像素正是本单要消除的那类错。**没选** ②③：一批里各条目采各自的纹理，`tbase == base` 是**逐条目**属性 ⇒ 丢掉正确条目（②）或把"一条坏 sprite"放大成"整帧空/黑"（③），与 `REQ-01M7A74YG08R65NJBFXF7DEJDK` 的"跳过而不是回退/整帧重来"口径冲突。**没选** ④：复用会让"draw 路命中"与"sprite 路命中"在位图 / 摘要里分不开，两条守卫互成恒真。
- **重写必须保持不变**：① 判据只有一份（`compositeBatch` 的 `tbase == base`），⛔ 不在 `gpuEligible` 里再判一遍 sprite；② 触发时**像素零写入** + 账上一定看得见（具名位 + `sprite_skipped` + 原因），⛔ 不许静默；③ 采**别的**纹理必须照常入队（反向条款，防过度收紧）；④ 位号 43 不与 28 合并（合并 = 两条守卫互成恒真）；⑤ 守卫 = `gpu_batch.cpp` 的 `d3d9p/sprite-feedback-loop`（CTest `gpu-batch-order`），变异 = 判据换成永不成立（`tools/mutate-check.mjs`）。

### L3-sprite-02 · `ID3DXSprite` 的 stage-0 采样器：**Begin 写文档值 / End 恢复 / Draw 用 device 当前值**
- **口径（MSDN 原文，不是推断）**：`ID3DXSprite::Begin` 的 "Sampler States" 表（只有 **stage 0** 的行）会
  自己写 `ADDRESSU/V=CLAMP`、`MAG/MIN=ANISOTROPIC`（`TextureFilterCaps` 无 `MAG/MIN` `FANISOTROPIC` ⇒ `LINEAR`）、
  `MIPFILTER=LINEAR`（无 `MIPFLINEAR` ⇒ `POINT`）、`MAXMIPLEVEL=0`、`MIPMAPLODBIAS=0`、`MAXANISOTROPY=MaxAnisotropy`；
  `ID3DXSprite::End` "restores the device state to how it was before Begin was called"；`Flush` 不恢复
  （"Device states remain as they were after the last call to Begin"）；`D3DXSPRITE_DONOTSAVESTATE` =
  "not to be saved or restored"（**写仍然发生**，只是 End 不恢复）。
  `ID3DXSprite::Draw` 本身**不设任何 state** ⇒ 实际采样器 = **device 当前的 stage 0**：Begin 刚写过，
  guest 可以在 Begin 之后用 `SetSamplerState` 覆盖（引擎每个 `DrawTexture` 正是这样：EA
  `0x4A30FB/0x4A310C/0x4A311D` 设 `MIN/MAG=LINEAR`、`MIP=NONE`）。
- **取舍**：采样器状态**落在设备的采样器表**（`d3d9::writeSamplerState`，`GetSamplerState` 读得到），
  sprite 只把 "pre-Begin 的 8 项" 存进自己的状态块（`kSpTexStages=1` × 32 B）供 `End` 恢复；
  `compositeBatch` **每批读一次** device 的 stage-0（`d3d9::readSamplerState` + `gpuSamplerFor`，
  与固定功能 `gpuEligible` **同一份** `gpuAddressFor`/`gpuFilterFor` 映射）。⛔ 不建 sprite 的影子采样表，
  ⛔ 不在 sprite 里写第二张 "D3D9 值 → GpuSampler" 常量表。
- **为什么（这条修了什么"偏"）**：修前 sprite 路**一个 sampler 都不写**、硬编码 `Clamp/Point/Point/mip-on`
  （`GpuSampler{}` 的缺省）⇒ 与真机的有效采样差 **MAG / MIN / MIPFILTER** 三项：
  ① `Point` + texel 角 UV（`u=left/tw`）在非 1:1 或小数位移时把内容吸到 texel 网格（≤0.5 texel/轴，
  两轴同时），`Linear` 保持连续；② `mipmap=true`（`max_lod=1000`）在缩小绘制时
  会采更粗的派生级 ⇒ **更糊**；真机（guest `MIP=NONE`）只采 level 0。
- **⚠ 实测边界（2026-11-21，SAVE08→SC0010 配方，两档 scale）**：这条修的是**保真偏差**（sprite 路
  现在真的按 guest 的 `MIN/MAG=LINEAR`、`MIP=NONE` 采样：平稳帧锐度 `17.05 → 15.94`），
  **但它不是**用户报的"切立绘偏 (1,1) + 变糊"的来源 —— 那 2 帧修前/修后仍是
  `(−0.48,−0.44)` → `(−0.48,−0.46)` px、锐度双峰（`15.94` vs `9.99`，进入/离开 chg_px 仍 ~49k）；
  那 2 帧是 `ID3DXEffect` 的 `Fade` + RTT 路（`submits=15 / targets=4`，其余帧 `submits=3`）。
  ⇒ 残余另立 **BUG-38**；⛔ 不要为了消它在本路加半像素补偿。
- **备选**：① 只把 sprite 缺省改成 `Linear/Linear/no-mip`（1–2 行）；② 只读 device 但不让 Begin 写/End 恢复
  （sprite 拉向 pipeline，不是拉向真机，且 guest 不设 address 时会漂成 WRAP）；③ 让 pipeline 也改成
  sprite 的硬编码值（把忠实的那条弄偏）。**没选** ①：仍"不问 guest"，guest 的 `POINT` / `WRAP` 一律被忽略；
  **没选** ②：address 会变成 pipeline 的 WRAP，而真机 sprite 是 CLAMP（Begin 写的）；**没选** ③：反向扩大偏差。
- **重写必须保持不变**：① 文档值写进**设备**（读回守卫走 `GetSamplerState`），⛔ 影子表；
  ② `End` 恢复 pre-Begin 的**整块 8 项**（含 guest 在 Begin 内改过的那一项），`Flush` 不恢复；
  ③ `DONOTSAVESTATE` ⇒ 写仍发生、End 不恢复；④ `compositeBatch` 读 device 当前 stage 0、失败
  （BORDER / MIRROR_ONCE / `MAG|MIN=NONE`）⇒ **记账 + 跳过这一条**（无 CPU 兜底），⛔ 不猜缺省值；
  ⑤ 无纹理条目（1×1 白纹理）不受采样器表达不了的影响（⛔ 不要因为采样器把白块也跳过）。
  守卫 = `d3dx9/begin-sampler-state`（文档值 / 恢复 / Flush 不恢复 / DONOTSAVESTATE）+
  `d3dx9/sprite-sampler-pixels`（2× 放大：LINEAR 混合值 ≠ POINT 纯色，且 guest 改成 POINT 时像素跟着变）；
  变异 = `kSpriteBeginMagMin` 由 `kTexfLinear` 改回 `kTexfPoint` ⇒ 前两者当场红。
- **caps 前提**：`static_assert(d3d9::kCapsMaxAnisotropyClaim == 1u)` 钉住"本线无各向异性" ⇒ 走 MSDN 的
  LINEAR 分支；caps 若将来真的支持各向异性，编译期就红（⛔ 不静默继续写 LINEAR）。

### L3-tl-01 · 顶点处理（T&L / 裁剪 / 状态机）留在 CPU、GPU 只吃**屏幕空间**三角形 —— 是 2026-11 的**分期**取舍，不是"两套实现"

- **为什么**：GPU 光栅化后端是**分期**接的。phase 1 的边界写死在 `REQ-01M6PAXW7EFCERJR6G2EEDN2A0`：
  "GPU 只做光栅，**T&L/裁剪/状态机/失败码留在 CPU**"，理由是当时实测到的墙钟收益全在光栅化
  （100 帧 boot：103.7 s → 31.8 s；瓶颈是 D3DXSprite 的逐像素合成 0.41 s/帧）。
  ⇒ 顶点这一层从来只有**一份**实现（`d3d9_pipeline.cpp` 的 `transformVertex` / `applyLighting` / 裁剪 / `toScreen`），
  所以它不违反"不维护两套实现"（那条是 `REQ-01M78BF1FNNH6K8N8GEY6GQYXX`，讲的是**光栅/像素**层，CPU 光栅已退休）。
- **今天仍成立，因为它是分期留下的位置，而不是被重新评估过的取舍**：CPU 光栅退休时没有人回头动 T&L。
  代价有两条，都要知道：
  ① 顶点/光照/裁剪的墙钟在宿主 CPU（ADV 场景每帧 ~60 笔 sprite × 4 顶点 ⇒ 没有读数表明它占主导，⛔ 不要凭感觉优化）；
  ② **设备 vtable 里没有 `Create/SetVertexShader`**（`+0x16C/+0x170`）⇒ 任何 `vs_*`（202 的 Snow 是 `vs_1_0`，
  3D 场景是 `vs_3_0`）今天**无法执行**，撞到就是 `comUnsupported` 响停。
- **要保住的可观察现象**：D3D9 fixed-function 的顶点语义（变换顺序、光照公式、裁剪、`XYZRHW` 直通、fog 记账）
  与"每笔 draw 的失败码"必须仍然只有一个来源。
- **什么时候该回头动它**：需要执行 `vs_*` 时。两条路，按 ADR `REQ-01M7MNYPDXM2NEQR77DXYZM3CJ` 的预转写口径：
  ① `vs_1_0` 这类**有限固定**的顶点程序 → 构建期转成 C++，仍在 CPU 顶点阶段跑（保持一份实现，推荐先走这条）；
  ② 一般的 `vs_3_0`（战斗/3D 场景）→ 才考虑 GPU 顶点着色 + shader 落后端，那是独立的大单。
  ⛔ 不要为了"看起来更像 GPU 渲染"而在没有顶点着色需求时把 T&L 搬上 GPU —— 那会立刻产生第二份语义实现。
