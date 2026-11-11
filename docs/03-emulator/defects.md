# 缺陷史（defects）

> 这份文件**替代"每个缺陷配一条用例"**。一条缺陷 = 现象（用户能看到什么）+ 根因（一行）+ 修法（`文件:行`）
> + 何时/谁发现 + **一条**代表守卫（没有就写"无守卫，仅文档"）+ 复现路径。
> ⛔ 不为每个缺陷加回一个用例；⛔ 不写"还剩几条"（会过时）。守卫门槛见 `README.md` §0.1。
> 编号 `BUG-NN` 一经分配不再改；补记在后面追加。这里的**读数**是历史上的观察，不是当前状态。

### BUG-01 · 第二次进 OPTION 必然停（纹理 arena 耗尽，一次往返净泄漏 18.08 MB）
- **现象**：进 OPTION → 右键退回 TITLE → 再进 OPTION ⇒ 停在装载中，纹理缺失/黑块，stderr 一句 `surface 分配失败（5898240 字节）：guest 堆耗尽`。
- **根因**：`ComRegistry::release` 到 0 只 `free(vtable)+free(obj)`、**无析构回调**；arena 像素块只在 iface 的 `Release` shim 里回收，而内部有 11 处裸 `cpu.com->release`（`SetTexture`/`SetRenderTarget` 换绑、`destroyForDevice`、Sprite 批次）⇒ 所有权挂在**调用点**上，"最后一次引用由裸 release 降为 0"时像素块永久留在 arena。
- **修法**：`src/runtime/com.cpp:282-286`（到 0 的唯一收敛点先跑 `on_zero` 再 free）+ `src/runtime/com.h:94,125`；纹理在 `src/shims/d3dx9.cpp:1031,1092` 登记 `d3d9::destroyTextureState`；回收见 `src/runtime/d3d9.cpp:936-943`。★ `IDirect3DSurface9` **不登记**（`GetSurfaceLevel` 的表面复用纹理状态块，登记会提前释放像素）。
- **读数（修前，实测非推断）**：三时刻 arena `in_use` = 108,595,152 → 120,693,568 → 126,672,232 B ⇒ 一次往返 **+18,077,080 B**；退回 TITLE 后最大连续空闲块 **5,814,312 B** < 这张纹理要的 5,898,248 B ⇒ 第二次必失败。
- **何时 / 谁发现**：2026-11-08，静态诊断（`.tmp/task2-report.md` §0-§5）→ 端到端实测定量（`.tmp/req-option-leak-body.md` §"真因"）。
- **代表守卫**：CTest `option-roundtrip/3cycles`（1/2/3 轮往返的 `in_use`/`peak` 不随轮数增长，容差 1 MiB）；回收面单点由 `tests/d3d9.cpp#d3d9/texture-bound-then-guest-release` 钉。两者都做过"短路 `onZero` ⇒ 当场红"的变异。
- **复现**：`--input-script <TITLE→OPTION→右键→再进 OPTION>`（`.tmp/input/opt-cycle2.txt`；帧 120 点入、172 右键回、234 再进、`--max-frames 400`）。

### BUG-02 · 同一个失败被记成"guest 用了没实现的导出"（误诊链）
- **现象**：摘要与 stderr 都是 `=== unimplemented D3DX9 call === D3DXCreateTexture` 并进 `called-unimplemented` 计数 —— 看起来"缺一个导出"，实际是**我们自己**的容量问题。
- **根因**：`d3dxTrap` 是唯一出口：`createTextureState` 的任何失败（含 arena 耗尽）都送进去，它固定打 `"unimplemented D3DX9 call"` + 写 `cpu.stop_why` + `++cpu.shim.unimplemented_calls` ⇒ 两种完全不同的停因在账上长得一样。
- **修法**：`src/shims/d3dx9.cpp:104-125` 新增 `d3dxResourceStop`（独立标题 `=== D3DX 资源耗尽（不是 unimplemented）===`、独立 `stop_why = "D3DX 资源耗尽"`、**不** `++unimplemented_calls`）；`d3dxTrap`（`:90`）只留真缺口；同处补 `w×h / D3DFORMAT / 像素字节`。
- **何时 / 谁发现**：2026-11-08，`.tmp/task2-report.md` §6（归因链逐行取证）。
- **代表守卫**：`option-roundtrip/3cycles` 断言全程**无** `guest 堆耗尽` / `unimplemented` / `D3DX 资源耗尽` 字样。
- **复现**：`.tmp/task2-report.md` §6 的 `d3dx9.cpp:1011-1016` 调用链；stderr 原文见 `.tmp/req-option-leak-body.md` §"症状"。

### BUG-03 · 音频 feeder 与主线程并发跑 guest 代码 ⇒ 宿主堆损坏（0xC0000374）
- **现象**：`option-roundtrip` 跑 2–3 轮时**随机**崩在 `0xC0000374`（宿主 CRT 堆元数据损坏，**无应用帧**），同一脚本有时全绿；实测失败率 1/4 完整测试。
- **根因**：窗口模式 ⇒ 真实时钟 ⇒ 音频喂料起**真线程**（`src/runtime/audio.cpp:151`），feeder 执行 guest 代码（`audio.cpp:1286`）而它的 CPU 是 `audio_cpu_ = *cpu_`（`:1087`）⇒ `GuestHeap::blocks_`（`src/runtime/heap.h:64`）、`HandleTable::entries_`（`handles.h:103`）、`Vfs` 与主线程**同一批对象且无锁** ⇒ 两线程同时 `HeapAlloc` 时 `std::map` 树旋转写坏宿主堆。
- **修法**：`GuestToken`（`src/runtime/guest_token.h`）＝任一时刻最多一个宿主线程持有令牌，互斥点在**每次 shim 调用**（`ShimRegistry::invoke`）：feeder 的整次 fill **不**持，只有它的 shim 调用持（`audio.cpp:1068-1071,1179-1185,1223,1271`）。`--assert-guest-token` ⇒ `token : violations=0 max-concurrent=1`，违规退出码 4。
- **排除项（省得再查）**：`metrics()` 悬垂、`GuestHeap` double free（`heap.cpp:116-121` 是 `abort()` 不是静默）、`SurfaceArena` 指针相撞、`on_zero` 写坏宿主堆 —— 四条都被逐条否证（`.tmp/heapbug3/report.md` §2-§5）。
- **何时 / 谁发现**：2026-11-08；两轮 ASAN 未拿到栈（`.tmp/heapbug2/report-round3.md` §2-§3），第三轮静态审计定到并发（`.tmp/heapbug3/report.md` §1），A/B 见 `.tmp/audiord/*-summary.txt`。
- **代表守卫**：CTest `guest-token-stress`（`tests/guest_token.cpp`，2 线程 × 200 000 次真实 `GuestHeap` alloc/free；把 `acquire/release` 短路成空操作 ⇒ 红）。
- **复现**：`.tmp/heapbug3/report.md` §1 的判据链；无令牌形态的整轮墙钟见 OBS-15。

### BUG-04 · `click` 的按下短于 guest 采样周期 ⇒ 被**静默吞掉**
- **现象**：脚本点「戻る」后画面不动、帧号不推进，看起来"退出卡死"；下游拿假数据继续跑。
- **根因**：房间输入采样是**电平**式、约 20 Hz（OBS-07）⇒ `down→up` 若整体落在两次轮询之间，guest **一次也没看见"按着"**；原脚本只隔 20k 条指令（**0.7 ms**）⇒ 必被吞。
- **修法**：`click` 改成"按下后**等 guest 真的采到**再抬起"（`src/runtime/input_script.cpp:481-505` 的 `releaseDue`：`auto_hold` 按"采样数增加"放行，`max_auto_hold_ms_` 到上限才 bounded 放行；`:516-556` 打 `waited_samples`/`hold_ms`/`post_sample_ms`）。`mouse_down`/`mouse_up` 显式成对写时仍是两条独立动作（间隔由脚本作者负责）。
- **何时 / 谁发现**：2026-11-08，用户报"出房间 9–20 s 不稳定"→ 逐帧 BMP 比对 + 两个对照定因，并撤回两条旧结论（`.tmp/exitstall/report.md` §1、§7）。
- **代表守卫**：CTest `input-injection/click-spans-sample`（`tests/input_injection.cpp:826`，伪造 50 ms 周期消费者，断言 `release_samples > press_samples` 且 guest 侧 `kDownSeenPolls ≥ 1`）。变异（改回"同一 tick 内 up"）⇒ 9 failures，`down-seen=0` 正是用户看到的现象。
- **复现**：`.tmp/exitstall/exit-y-longpress.txt`（长按 220 ms，3/3 成功）对 `.tmp/room/exit-prof2.txt`（20k 条间隔，失败）。

### BUG-05 · `SDL_UploadToGPUTexture(cycle=true)` 用在**子矩形**上传 ⇒ 矩形外被清成未定义
- **现象**：改一块纹理后，同一张纹理上**没被写**的地方变黑或花掉（只在"看起来整幅重新上传过"时出现）。
- **根因**：`cycle=true` 的语义是"丢弃该资源原有内容"，只对**整幅**上传成立；子矩形上传传 true ⇒ 矩形外按契约变成未定义。
- **修法**：`src/runtime/d3d9_gpu.cpp:643` 改成 `SDL_UploadToGPUTexture(copy, &src, &dst, /*cycle=*/u.whole)`；判据写在 `:1184-1185`。
- **何时 / 谁发现**：2026-11-08，GPU 直出/子矩形上传那一轮（`cycle` 语义取自 `SDL_gpu.h` 的轮换条款，见 L3-gpu-02）。
- **代表守卫**：CTest `gpu-batch/subrect-upload`（`tests/gpu_batch.cpp:947` 起的像素断言：矩形内变、矩形外**逐像素不变**）。
- **复现**：`ctest --test-dir apps/emulator-lift/out -R gpu-batch --output-on-failure`（需 dummy 视频驱动，见 `CMakeLists.txt` 的 SKIP 分支）。

### BUG-06 · 深度"每次颜色同步都清" ⇒ 多段渲染下先画的深度写失效
- **现象**：同一帧分几段画的几何，后画的**该被挡住却没被挡住**（先画那段的深度像没写过）；只在"帧内发生过 `Clear(TARGET)` / CPU 回退 / 多段提交"时出现。
- **根因**：`rasterSyncFromGuest` 无条件 `need_depth_clear_ = true` ⇒ guest 每同步一次颜色，下一段的 depth `LOADOP` 就是 `CLEAR`。
- **修法**：`src/runtime/d3d9_gpu.cpp:378-381`（颜色同步**不**置该标志；只有 `Clear(ZBUFFER)` 与"新 target"置）、`:668` 改成 `need_depth_clear_ && first_render_pass`、`:722` 只在真录了 render pass 后清标志。
- **何时 / 谁发现**：2026-11-08，GPU 合批/切段设计的"4 个必须同时改的点"之一（`.tmp/task4-report.md` §3 方案 A(a)）；当时**一个守卫都没有**（全部用例 `depth_test=false`）。
- **代表守卫**：CTest `gpu-batch/depth-segments`（`tests/gpu_batch.cpp:1026`；`:1089` 记着判据"旧实现无条件 `need_depth_clear_=true` ⇒ 这里 z=0.5 会被错误接受"）。
- **复现**：`ctest -R gpu-batch`；变异（去掉 `:668` 的 `first_render_pass`）⇒ `depth-segments` 红。

### BUG-07 · `GetTextMetricsA` 未缓存：111 µs/次，其中 3 次 `FT_Load_Char`
- **现象**：文字多的帧明显慢；`--profile` 里 `GetTextMetricsA` 平均 111 µs/次，单帧被调 561 次。
- **根因**：`FontEngine::metrics` 每次走 `faceForSize`（每次构造 `std::string path` + 两次 string-keyed `std::map` 查找）+ 3 次 `FT_Load_Char`（量 `'M'`/漢/あ 的 advance）量度量；字体是 23.0 MB 的 `Amayui-CN_cnjp.ttf`，裸 `FT_Face` 无 glyph 缓存。
- **修法**：`src/runtime/font.cpp:163` 加 `std::map<std::tuple<int,int,std::string>, HostTextMetrics> metrics_cache`（键 = `(clamp 后像素字号, bold?1:0, 字体路径)`），`:444-463` 命中即返回、`kMetricsCacheMax=64` 到上限整表 clear；`weight/italic/underline/strike` 仍每次从 desc 覆盖（**不进键**）；失效点 `:294,300,307`。
- **口径警告**：111 µs 是**开 `--profile`** 时测的（含 2 次 `steady_clock` + 每次 shim 的 `ShimDef` 值拷贝）⇒ 只用于"哪一段占主"的归因，不是性能成绩（`measurement.md` M-02）。
- **何时 / 谁发现**：2026-11-08，`.tmp/task3-report.md` §1（归因）与 §5-A（合同）。
- **代表守卫**：CTest `gdi-font`（"换字号必须给出对应字号度量、同字体两次逐字节相同"这类**真 GDI 合同**）。⚠ 原来钉 hit/miss **计数**的 `gdi-font-metrics-cache` 属 BUG-14 的反模式。
- **复现**：`.tmp/task3-report.md` §1 的逐段归因；读数来自 `--profile` 的 shim 表（`avg=`）。

### BUG-08 · 基线 +2px 只作用在 `TextOutA` ⇒ 引擎正文**从未生效**（假对齐）
- **现象**：与对照实现（`apps/emulator-cxx`）比，同一行字**低约 2 px**（cxx 有效 ascent 18、lift 报 20），但常量两边都是 2 ⇒ "看起来对齐了"。
- **根因**：`baseline_lift` 的唯一应用点在宿主绘制路径 `src/runtime/font.cpp:356-360`（`gmptGlyphOrigin.y = slot->bitmap_top + baseline_lift`，只被 `src/shims/gdi32_font.cpp:299` 的 `TextOutA` 用到）；引擎正文走 **GGO + guest 自己合成**（`TextOutA` 只 10 站点 / 2 caller）⇒ 补偿对正文无效。cxx 从**每次绘制的最终基线**减（它自己栅格化）⇒ "同常量"≠"同效果"。
- **修法**：**先别改**。要常量一致 ⇒ 已一致（零改动）；要正文墨迹对齐 ⇒ 只能动 guest 可见的 ascent，而那会破 `tests/gdi_font.cpp:228`（`tm_height == tm_ascent + tm_descent`）、`tests/gdi.cpp:432-434`（合成 22/20/2）与 `apps/emulator-lift/SCOPE.md:111`（"补偿**不**写进度量"）⇒ 两条案都要人裁决（`.tmp/task3-report.md` §6）。
- **何时 / 谁发现**：2026-11-08，`.tmp/task3-report.md` §4（常量来源 `apps/emulator-cxx/src/core/text_raster.cpp:647` 的 `lift = 2`）。
- **代表守卫**：CTest `text-baseline-lift`（`assets` 档，需安装树；断言"关掉补偿后墨迹顶正好低 2px"）+ `SCOPE.md:111` 的声明本身。
- **复现**：`.tmp/task3-report.md` §4 的 4 条路径对照；`--baseline-lift 0` 与缺省各跑一次逐帧 BMP 比对。

### BUG-09 · GPU 合批在"纹理内容变化"时退化成**逐条提交**（136 次/帧）
- **现象**：OPTION 装载帧 87.4 ms 里几乎全是提交开销；`--profile` 窗口表里 `ID3DXSprite::End` 一条占大头，而 draw 数并不多。
- **根因**：`ensureTexture` 发现"缓存里已有该纹理 ∧（staging 还没上传 ∨ 队列非空）"就整批 `rasterFlush` ⇒ "写一次字形 → 画一次 → End"变成**每条 draw 一次 submit**（帧内 135 次 + Present 1 次；0.642 ms/submit，成本必然落在 `End` 里）。
- **修法**：把"提交"换成"**切段**"（`src/runtime/d3d9_gpu.cpp:1349-1367` 的 `segments_`/`need_cut`：只比**本段**已入队且采样同一张 GPU 纹理的 draw，纯函数判交 `gpuTexelRectsIntersect`）+ 只传**脏区**（`:1303-1304` `gpuContentDirtyRegion`、`:1368` 起行按 256 B 对齐）+ 版本快路径（`:1298`）。纯函数落点 `src/runtime/gpu_raster.h:60,109,253`。
- **何时 / 谁发现**：2026-11-08，`.tmp/task4-report.md` §1-§3（设计裁决：**先脏区后切段**，顺序不能反）。
- **代表守卫**：CTest `gpu-batch/content-change`（`tests/gpu_batch.cpp:872`，退化到逐条提交就红）+ `gpu-batch/subrect-upload`（像素 oracle）。⚠ 原来钉 `cuts==1 / submits==2` 的**计数**断言属 BUG-14 的反模式。
- **复现**：`.tmp/task4-report.md` §2 的逐 End 推演；`ctest -R gpu-batch`。

### BUG-10 · 让 D3DX 真返回 `E_OUTOFMEMORY` 会换来**更假**的停因（OOM 语义裁决）
- **现象 / 根因**：按真机语义返回 `E_OUTOFMEMORY` 后，guest 确实走了失败分支（`0x48AD0C` 的 `test eax,eax / jnz`），但在 `0x4E8AB6` `throw`（C++ 异常码 `0xE06D7363`）⇒ 撞上**未实现**的 `kernel32!RaiseException`（SEH 展开不在本线）⇒ 停在一个新的 gap 上（exit 2）。
- **修法**：**保留硬停**，但换成独立措辞 + 独立 `stop_why`（`src/shims/d3dx9.cpp:108-125` 的注释把这条实测写死了）。⇒ 这条缺陷的"修法"是**裁决**，不是"改成返回 HRESULT"。
- **何时 / 谁发现**：2026-11-08，`.tmp/req-option-leak-body.md` §"OOM 语义裁决（实测）"。
- **代表守卫**：无守卫，仅文档（可判的那一半"stderr 不得出现 `unimplemented`"已由 BUG-02 的代表守卫覆盖）。
- **复现**：`.tmp/req-option-leak-body.md` §"OOM 语义裁决" 的三步实测记录。

### BUG-11 · PE 载入对坏节 / 坏导入**静默跳过**（半载入仍返回 true）
- **现象**：坏 PE 也"启动成功"，但 IAT 保留 PE 原值、某些节根本没拷进去 ⇒ 后面的"导入缺口 = 0"是**假绿**。
- **根因**：节拷贝在 `raw_offset + copy` 越界时**直接不拷贝**（没有 else）；导入表解析对 `rvaToOffset` 失败 / 坏 thunk 只 `continue`/`break`。
- **修法**：两处都改成"带原因返回 false"（拒绝启动），与 sha256 不符同档 —— `src/runtime/image.cpp:347-352`（要拷的字节越界）与 `:363-364`（**整个 raw 声明区间**必须落在文件里，纯填充越界也拒绝；后者来自 `REQ-01M6KK2C6TKBA88M6EXG1H7E5B`）；导入表侧见 `:242`。
- **何时 / 谁发现**：缺陷单 `REQ-01M6JFZFCQ956CBFMKKQ37PCYK`（`type: bug` / `severity: S2` / `status: done`），来源 `.tmp/m1a-audit/A-runtime.md` 的 F7/F8。
- **代表守卫**：CTest `pe-loader` 的 `#section-raw-out-of-range`（`apps/emulator-lift/tests/pe_loader.cpp`，同时是该单的 `verify:` 与 `repro:`）；边界侧 `raw-range-at-eof-ok` 钉住"虚拟尾部 / 文件内填充 + raw 恰好止于 EOF"仍被接受（⛔ 别退化成误杀）。
- **复现**：`ctest -R pe-loader`；缺陷单正文 `data/requirements/01M6JFZFCQ956CBFMKKQ37PCYK.md`。

### BUG-12 · fault 归因按"最近一次 guest 访存"⇒ 宿主 AV 被报成 guest 访存
- **现象**：宿主自己的空指针解引用，诊断里却出现 `access :` 行（"guest 访问了 0x…"）⇒ 归因指向 guest，排查方向被带偏。
- **根因**：归因用的是 `g_access`（最近一次 guest 访存），而宿主 AV 时它是**陈旧值**；正确判据是**异常地址**是否落在 guest 空间。
- **修法**：`src/runtime/memory.cpp` 的 `vectoredHandler` 先 `inGuestSpace(fault_addr)` 再决定 `guestFault(_Exit(3))` / `hostFault(_Exit(4))`；`LIFT_VEH_PASSTHROUGH` 只对**非 guest 地址**让路（默认关，语义不变）。
- **何时 / 谁发现**：缺陷单 `REQ-01M6JCBYBW8DV0920T28337ACF`；`tests/fault_report.cpp:4` 的文件头写着这条。
- **代表守卫**：CTest `fault-report`（`tests/fault_report.cpp:239-249`：先把 `g_access` **弄脏**再解引用 `0x1`，断言有 host fault 标记、**没有** `access :` 行、退出码 4；反向用例证明 guest 诊断没被弄坏）。
- **复现**：`ctest -R fault-report`（两平台都真跑，没有 SKIP）。

### BUG-13 · 帧记录基址被两个视图锚在不同偏移，整体错 0x14
- **现象**：按某一视图的偏移读"当前帧记录"的字段，读到的其实是另一个字段（如把 `cur` 的槽当记录基址、或把脚本体堆指针当记录起点）⇒ 每帧记录整体错位，而"大部分脚本看起来都对"。
- **根因**：同一份数据有两个互不相同的锚 —— 形状模型把记录锚在 `0x5D894`（= 语料基址 + 0x14，实为"脚本体堆缓冲"字段），而语料里 `cur` 在 `0x5D880`、记录步长 120、身份格在 `+0x50`（= `0x5D8D0`）。**抄模型的偏移加到语料基址上**就错 20 字节。
- **修法**：地址一律按"**语料基址 + 偏移**"写，⛔ 不抄模型偏移（`.tmp/anchor-design.md:28-36` 把 5 行读地址链列全了）；守卫钉"记录基址 / 记录大小 / 不许把记录之外的 Engine 级标量当 per-cur 字段读"。
- **何时 / 谁发现**：2026-11-08，`.tmp/anchor-design.md` §1 表 + §2 的"基址陷阱（会读错一次）"；变异侧见 `tools/mutate-check.mjs` 清单里"记录基址 `0x5D894` 改回 `0x5D880`"那一条。
- **代表守卫**：`tools/test/engine-frame.test.mjs`（`@env assets`，`@kind gate`）。
- **复现**：`node --test tools/test/engine-frame.test.mjs`；对照 `.tmp/anchor-design.md:10,36` 的地址链。

### BUG-14 · **反模式**：用 test 锁住"我们的内部结构"（不是合同）
- **现象**：一批用例能红，但红只说明"实现细节变了"，不说明用户会看到错 —— 于是既不能删（怕丢回归）也不能改（怕红），维护成本全落在"改实现要改测试"上。
- **根因**：把**策略/计数/内部布局**当成断言对象。实测清单（`.tmp/test-audit/report.md` §2.1）：`tests/heap_layout.cpp:76,107`（首次适配拿回某地址 + `scanned <= kRounds*8`）· `tests/input_tick.cpp:43`（`actionScans() <= 16`）· `tests/gdi_font_metrics_cache.cpp:291-292,323-453`（hit/miss 计数）· `tests/gpu_batch.cpp:923-924`（`cuts==1`/`submits==2`）· `tests/profile_window.cpp:321-323,340-341`（分项和 / `calls=1/2/10`）。
- **修法（本轮裁决）**：这类知识进文档、不进 test —— 堆策略与计数 ⇒ L3-heap-01；tick 快路径 ⇒ L3-model-01；度量缓存键 ⇒ L3-gdi-01；合批/切段判据 ⇒ L3-gpu-02；profiler 分项定义 ⇒ `measurement.md` M-03。**只留"换一份实现后仍需成立"的那一侧**：堆的两条真不变量（未提交页读 0 / 越界 fault）、tick 的触发边界、度量的 GDI 合同（换字号必须变）、像素 oracle、未闭合窗口必须告警。
- **何时 / 谁发现**：2026-11-08，测试审计（`.tmp/test-audit/report.md` §2.1、§3、§5）；判据写入 `README.md` §0.1 与 `AGENTS.md` §10。
- **代表守卫**：这一条**故意没有守卫** —— 它是"不该写守卫"的判据本身（自检三问见 `README.md` §0.1）。
- **复现**：`node tools/mutate-check.mjs --list`（看清单里有多少条改的是内部常量）；逐单元分类见 `.tmp/test-audit/inventory.tsv`（`label=DROP-L3` 那批）。
- **现状（2026-11-09 复核，以仓为准）**：上面点名的四件事**都还在仓里** —— `tests/input_tick.cpp:43`（`actionScans() <= 16` 仍在）、`tests/gpu_batch.cpp:923`（`cuts - s0.cuts == 1` 仍在），`tests/{heap_layout,gdi_font_metrics_cache,profile_window}.cpp` 三个文件也都在、且都注册在默认门禁里。⇒ 这条审计的**知识侧**（落 `L3-*` / `measurement.md`）已落地，**测试侧的删除还没做**；要删是单独一条单，⛔ 不顺手删证据（`AGENTS.md` §11）。

### BUG-15 · 出 EU-ROOM 的路径停在后缓冲 surface 的 `LockRect`（未实现），且**退出码 0**（**LockRect 一跳已修 + 有守卫**；端到端仍被新的 SEH gap 挡住）
- **★ 归属更正（2026-11-10，spike `REQ-01M71P87BMTZK4MHT8468BMC3G`）**：这里按 ESC 走的是**退出游戏（关窗）**，
  不是"回到 TITLE"；"出 EU-ROOM"是当时的错误口径（把 BUG-16 的「戻る」与 ESC 混成一件事）。
  `LockRect` 是退出清理里读后缓冲，不是"出房间"的必经点。
- **现象**：在 `EU-ROOM`（房间稳定帧 hash `0x96E51755F00B2E9E`）里按 **ESC**（VK `0x1B`）⇒ 900 条指令后停：`stop: stopped @ eip=004A4F8D`、`detail: unimplemented D3DX9 call（shim com!IDirect3DSurface9::LockRect）`，而**进程 exit 0**。⇒ 自动化会把它当成"任务成功"。
- **根因**：两次叠加。① `src/shims/d3dx9.cpp` 的 `shimSurfaceLockRect` / `shimSurfaceUnlockRect` 对"后缓冲 surface（`state` == 设备状态块）"**明确 trap**（"⛔ 不把锁字段写进设备状态块"）⇒ 房间退出路径要读后缓冲，这里没有实现。② `src/runtime/machine.cpp:1030-1059`：`had_impl` 为真但 shim 自己 `stop_why` 的停机被归成 `StopReason::Stopped`（`ok()` 为真）⇒ **未实现的方法槽被当成正常结束**（本单已把这类停改成 `Trap`，见下面的"现状"）。
- **读数（2026-11-09，macOS arm64 / RelWithDebInfo，`--skip-logo`）**：ESC 在 insn `1,400,000,000` 落下 ⇒ 停于 `1,400,000,900`；同一次运行 `--max-frames 600` 只呈现到第 318 帧（房间稳定帧从第 141 帧起）。对照：同一房间点 **CG 鑑賞**（`1100,150`）会正常切画面（hash 连变 3 帧）⇒ 不是"房间完全不吃输入"。
- **代表守卫**：`d3d9/backbuffer-lockrect`（CTest `d3d9`）· `gpu-batch/backbuffer-lock-barrier`
  （CTest `gpu-batch-order`）—— 见下面的处置。端到端复现 = `--interaction-graph
  apps/emulator-lift/scripts/interaction/room-exit.graph`（任务没走完 ⇒ **exit 5**，stderr 打出断点与最后画面）。
- **复现**：`--game-dir <安装> --skip-logo --audio off --input-script <at frame 130 / click 944 562 / at insn 1400000000 / key_down 0x1B>`（房间入口 = TITLE 的 `Eushully-chan Room` 按钮，中心 `944 562`）。
- **现状（2026-11-09，`REQ-01M6WW9R6SJ7P8XA4522XCR48A` 落地后）**：退出码这一半**已修** —— 这类停因现在归
  `StopReason::Trap`（摘要 `stop      : unimplemented-slot`，**非 ok**）⇒ 没有交互图时 **exit 2**
  （`--gpu-strict` 也先判 2；交互图下"任务没走完"仍是 5）。
- **处置（2026-11-09，`REQ-01M6ZDSGQY1RXX3N3WJKA8D26H`）**：后缓冲 surface 的 `LockRect` / `UnlockRect`
  **已实现**（`runtime/d3d9.cpp` 的 `lockBackBuffer` / `unlockBackBuffer`，shim 在 `shims/d3dx9.cpp` 分派）。
  口径（每条都绑可再校验观察）：
  - **锁字段落点 = 后缓冲的 `GuestSurface`**（`runtime/surface.h`，`d3d9_locked/lock_rect/lock_serial/
    lockable/multisample`）。⛔ 不进 `'D9D1'` 设备状态块（64 B 全占满）；⛔ 不进 `'TEX1'`（锁字段在
    +44/+48，会踩别人的）；生命周期随像素块（`SurfaceArena::destroy` 自动收）。
  - `D3DLOCKED_RECT`：`Pitch` = **整幅**行距（锁子矩形不改），`pBits` = 像素首字节 + `top*pitch +
    left*bpp`（**矩形左上**）。依据 = D3D9 文档 + 真机 `0x4A4DC0` 的逐行 memcpy
    （`0x4A5016 call _memcpy`、源指针 `0x4A5021 add edi,[ebp+var_44]` 按 `Pitch` 推进、行宽 = `w*bpp/8`）。
  - 锁前 GPU 屏障：这块像素是当前镜像目标 ⇒ `rasterSyncToGuest`（flush + 读回）；不是目标 ⇒ 只
    `rasterFlush`（⛔ 不把别的 target 读回来）。解锁**不**标脏 / 不发版本号 —— "guest 是权威"由下一次
    绘制的 `!rasterDirty() ⇒ rasterSyncFromGuest` 兑现。
  - 契约级 guest 错误 ⇒ `D3DERR_INVALIDCALL`(0x8876086C) + Warn（**不 trap**，guest 自己对 `0x4A4F8D
    test eax,eax` 分支）：已锁再锁 / 未锁就解锁 / 矩形越界或空 / `DISCARD`（后缓冲不是 dynamic）/
    无 `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` / `MultiSampleType != NONE` / 未定义 flags 位 /
    `pLockedRect`/RECT 不可写。本仓状态崩坏（状态块/arena 对不上、GPU 读回失败、bpp/pitch 为 0）仍 trap。
    ⚠ 与纹理路的**已知分歧**：同一 COM 方法在纹理 surface 上失败仍 trap（含"已锁"），后缓冲上返回错误码。
    `MultiSampleType != NONE` 的锁字段只能白盒构造（`CreateDevice` 对 MSAA 先答 `D3DERR_NOTAVAILABLE`）；
    这条是 `LockRect` 侧的独立兜底，不是可达的生产路径。
  - **守卫**：`tests/d3d9.cpp` 的 `d3d9/backbuffer-lockrect`（CTest `d3d9`，pure）断言 `Pitch`/`pBits`、
    子矩形不改 Pitch、写 `pBits` 穿透、已锁再锁/未锁先解/越界/DISCARD/无 LOCKABLE/MSAA 各
    `INVALIDCALL`、解锁后可再锁（`rect=NULL` ⇒ `pBits=arena`）、`unimplemented_calls == 0`；
    `tests/gpu_batch.cpp` 的 `gpu-batch/backbuffer-lock-barrier`（CTest `gpu-batch-order`，pure）断言
    锁前 `syncToGuest` +1 且 flush 在前、目标不是这块像素时只 flush 不回读。
    ★ `gpu-batch-order` 原先**没有** `add_test`（只剩 `--gpu` 的 `gpu-batch-barrier`）⇒ 已补回顶层注册，
    否则无 GPU 机器上会 SKIP 77，"删屏障"静默逃逸。
  - **变异**：`tools/mutate-check.mjs` 的 `--match backbuffer` 五条（pBits 去偏移 / Pitch 用子矩形宽 /
    删已锁检查 / 删锁前屏障 / 删 LOCKABLE 检查）实测 5/5 变红后还原。
  - **真机读数（2026-11-09，macOS arm64 / RelWithDebInfo，`--present headless --raster cpu`）**：
    `./out/emulator-lift --game-dir /Users/nekosu/Documents/Projects/amayui-cn/raw --skip-logo --audio off --present headless --raster cpu --log-level info --log-cat d3d9 --interaction-graph scripts/interaction/room-exit.graph --time-limit 900`
    ⇒ `LockRect(backbuffer): Pitch=5120 pBits=0x28000008（矩形 0,0..1280,720）· 第 1/2 次锁` 两次 +
    `UnlockRect(backbuffer)` 两次，**没有** `unimplemented D3DX9 call` ⇒ 这一跳已经通了。
    ⚠ 任务仍**没走完**：trace 末尾是 `action key 0x1B applied`（没有 `task done`），进程 **exit 5**，
    `stop=unimplemented detail=未实现的导入 kernel32!RaiseException`（`0x4123C4 __CxxThrowException
    (Command_Exit_Exception)`，从 `sub_412290` / WM_TIMER 2 抛出、返回地址 `0x004123C9`）。
    ⇒ **新 gap = SEH / C++ EH 展开不在 M1**，不是本单没修好；终点 TITLE hash 这一轮**没拿到**。
  - **残余**：`LockRect` 只是这条路上的第一个 gap；后面还有 `operator new(0x540)`、逐行 memcpy、
    `UnlockRect`、`Release`（全 guest 代码），再往后是上面那条 `RaiseException`（新读数）。
- **同族**：`resource-exhausted`（`d3dxResourceStop` 的资源耗尽硬停）同样从 `Stopped` 拆出、非 ok；
  名字与分类的真源是 `apps/emulator-lift/src/runtime/machine.h` 的 `StopReason` + `machine.cpp` 的
  `stopReasonName` / `exitCodeForRun`。

### BUG-16 · `EU-ROOM` 里的「戻る」按钮对**注入点击**没有反应 ⇒ **不是缺陷**（2026-11-09 是量测窗 artifact）
- **归属改判（2026-11-10）**：`room-exit.graph` 的 room 节点换回 `action click center` 后**真机 exit 0 + `task done`**，
  终态 = TITLE 稳定帧 hash `0x94CB492BF7F1AEEB`。三次 run（12:39 / 12:47 / 12:52）**读数逐位相同**：
  `insns=2120728564`、`transitions=11 actions=3 samples=249 changes=44`；三份 trace 里两份 copy 图逐行相同，
  本尊那份只差 `origin=` 一行（`diff` 实测）。
  ⇒ 按钮不是"点不动"；**抬起那一下点击就已生效**，只是"出房间"要跨过一个极长的重载帧才落到 TITLE。
- **2026-11-09 为什么读成"点不动"（已核，不是猜测）**：退出要经过 **抬起帧之后 103 帧退场**
  （每帧 ≈29.5k 条，画面就是 hover 态 `0xF13F…`），再进 **单帧 730,765,167 条**的 TITLE 重载帧
  （`LIFT_FRAME_TRACE=1`，≈73 s guest / 24 s 墙钟）。那三次 run 的 `quit` 分别落在抬起后
  +98M / +188M / +350M 条 —— **全在那一个重载帧里面**，而且三次都恰好"抬起后 103 帧"
  （与今天的退场帧数相同）⇒ 退出当时**已经在进行**，取样窗在重载帧结束前被 `quit` 关掉了。
  旧读数"hash 稳定在 hover 态不变"正是那 103 帧退场期的画面。
- **点击确实是原因（四路对照，2026-11-10，同机）**：空白点 `click 100 100` / 不点击 / 只 hover ⇒ 全部 **exit 5**
  （`expect-change(node room)` 或 `timeout(node back)`）；点 CG 鑑賞 `1100,150` ⇒ 画面变了但**不是** TITLE。
- **★ 判据的坑（留着防再犯）**：room 节点的 `expect screen changed` 会被**抬起那一下的 hover 重绘**满足 ⇒
  它**不能**证明"点击生效"；真正的判据是 `back` 节点等到 TITLE hash。
- **命中区 ≠ 绘制框**：`(1240,687)`（框中心）与 `(1267,706)`（右下内缩 5px）都能触发；同框内的 `(1205,670)` 与
  框外的 `(1190,690)` 在 900M 条窗口里都无效 ⇒ 绘制框 `1200..1272 × 652..711` 的**左边缘不在命中区**。
  命中矩形由脚本参数给定（handler `sub_420640` → `sub_403B30`），不是硬编码 ⇒ `screen rect … avg` 的用途是
  "认出这一屏 + 取中心"，不是"精确命中区"。
- **右键是同一条路**：`WM_RBUTTONUP`→`0x4B9D3F`→`sub_4B8DC0(1)` ⇒ 输入字 **bit5 = 引擎的取消位**
  （`0x411CFE test byte ptr [ebx],20h`）。实测 `room-exit-right.graph`（`click center right`）⇒ exit 0 +
  `task done` + 同一个 TITLE hash（`OBS-20261110-04`）⇒ 任务图已覆盖它
  （⛔ 取消目标由脚本登记的取消标签决定，别把右键图当模板套到别的屏）。
- **代表守卫**：`apps/emulator-lift/tests/interaction_graph.cpp` 的 `graph-scripts-parse`（两份出房间图必须能解析）
  + `parse-button-right` / `emit-button-carried`；**终态级判据仍是 assets 档真机 run**（要安装树）。
- **复现（成功路径）**：`--interaction-graph scripts/interaction/room-exit.graph` ⇒ exit 0 + trace 末 `task done`。
- **重开条件**：若再出现"点了没反应"，先确认观测窗盖住了一次 TITLE 重载帧 —— ⛔ 别再用 `--input-script` 里
  `at insn` 的硬锚点当终点（11-09 就是那么误判的）。

### BUG-17 · 纹理上传的行距按 256 对齐、SDL/Metal 按目标宽度算源行距 ⇒ 整张纹理被削成横条（**已修**）
- **现象**：`--raster gpu --raster-sprites 1`（本次改动前的缺省）进 **OPTION** 或 **EU-ROOM** 后，整块大背景（房间的糕点店照片、OPTION 列表文字）呈**横向条带 / 撕裂**；同一帧里的小 UI 面板（按钮框、图标）正常。同一帧把 sprite 交给 CPU（`--raster-sprites 0`）画面**完全正确**。
- **不是哪次提交引入的（已用重建证伪）**：在 `2bd8eaf`（`5e4fca3` / `8141659` 之前）重建后跑同一路径，得到**逐字节相同**的撕裂帧（sha256 `d0e0af01…`）。⇒ 这是 `--raster gpu` 的**既有**缺陷，与那两次提交无关。
- **已排除（每条都实测）**：
  - **不是 CPU/GPU 光栅不等价**：`LIFT_GPU_VERIFY=1` 的逐批 CPU 参考 vs GPU 读回对照，42 批**全部 `diff_pixels=0`**。
  - **不是这几张纹理的上传错**：把 1280x720 / 1664x1536 / 1280x1664 / 2048x2048 的 GPU 纹理**从 GPU 读回成 BMP**，与 guest 源像素**逐字节相同**（sha256 相同）。⚠ 这四张的 `w*4` **恰好都是 256 的倍数** ⇒ 它们本来就走不出下面那个行距 bug（真正的坏样本是 1120x672，读回时才发现被削成横条）。
  - **不是脏区 / 版本缓存**：强制整幅重传（`content_change_known=false`、`generation=0`）结果不变。
  - **不是合批 / 屏障**：每条 sprite 后立即 flush 结果不变；每帧先 flush + 读回再从 guest 上传结果也不变。
  - **不是混合开关**：把 sprite 的 `blend` 关掉后 sprite 变成不透明黑块（说明 blend 生效、语义正常）。
  - **不是几何 / UV**：日志里 sprite 的 quad、uv、纹理尺寸、serial/version **全部正确**（全屏 quad + uv 0..1）。
- **读数（2026-11-09，macOS arm64 / RelWithDebInfo，真安装，headless 也复现）**：`--raster-sprites 1` ⇒ 房间帧 sha256 `d0e0af01…`（撕裂）；`--raster-sprites 0` ⇒ `708c1ccc…`（与 CPU 光栅帧一致）。撕裂帧在 `--present window` 与 `--present headless` 下相同 ⇒ 与呈现路径无关。
- **处置**：缺省改成 `--raster-sprites 0`（取**正确**的那一侧，`src/main.cpp` 的 `raster_sprites`）；`--raster-sprites 1` 保留给复现 / A-B。**翻回缺省 1 的判据** = 上面"已排除"里剩下的那一处定位到并修好后，`room-enter` 的 `--raster-sprites 1` 帧 sha256 等于 `--raster-sprites 0` 帧。
- **代表守卫**：无（要真安装 + GPU；`gpu-batch-*` 的 CTest 只测后端，不覆盖 D3DXSprite 的合成路径）。
- **复现**：`--raster gpu --raster-sprites 1 --interaction-graph apps/emulator-lift/scripts/interaction/room-enter.graph --dump-frame x.bmp`（headless 即可）。

**根因（2026-11-09 定位并修好）**：`src/runtime/d3d9_gpu.cpp` 的纹理上传把 staging 行距按 256 字节对齐
（`row_pitch = (rect.w*4 + 255) & ~255`），并把 `pixels_per_row = row_pitch/4` 交给 SDL_GPU —— 而 Metal 后端
**按目标区域的宽度**算源行距（实测 `pixels_per_row` 的"有富余行距"不生效）⇒ 每行错开
`(对齐后 − w*4)` 字节，整张纹理被斜切成横条。
**为什么只有 macOS 炸**：SDL 3.4.18 的 **Metal** 后端 `METAL_UploadToTexture` **不读** `source->pixels_per_row`，
它用 `BytesPerRow(destination->w)`（目标区域宽度）当行距（`src/gpu/metal/SDL_gpu_metal.m`；
同一个文件的 `METAL_DownloadFromTexture` **是**读的）；而 **D3D12** 的 `D3D12_UploadToTexture`
**读** `pixels_per_row` 并在不对齐时自己临时拷贝对齐（`src/gpu/d3d12/SDL_gpu_d3d12.c`）
⇒ **Windows 一直是正确的**，这条只在 Metal 上炸。

**这条只在 `w*4` 不是 256 的倍数时可见**：1280 / 1664 / 2048 宽的纹理恰好对齐（所以"背景"纹理是干净的、
我们几次读回都验不出问题）；**1120x672 的角色层**（EU-ROOM 立绘）与 800x600 / 350x35 才必现 ——
它被画在背景之上，看上去就像"背景撕裂"。
**修法**：staging 改成**紧密排列**（`row_pitch = rect.w*4`）且 `pixels_per_row = rows_per_layer = 0`
（让 SDL 自己按 dst 区域推；SDK 文档明确：行距不对齐由 SDL 临时拷贝对齐，见 `SDL_gpu.h` 的 WARNING）。
**守卫**：`apps/emulator-lift/tests/gpu_batch.cpp#gpu-batch/unaligned-row-pitch`（40x20 纹理 + 逐像素图案 +
1:1 采样 ⇒ 目标像素必须逐字节等于纹理）。实测：**修前 800 个像素里 760 个不同**，修后 0 个。
★ 旧的 `subrect-upload` 用例看不到这件事，因为它用的是**纯色**纹理（削了也看不出来）——新用例特意用图案。

### BUG-18 · `EnumFontFamiliesExA` 只上报一条族名 ⇒ 引擎字体名表查不到 `ＭＳ ゴシック` ⇒ `std::out_of_range` ⇒ CRT `int3`（TITLE→OPTION→ADV设定 / TITLE→GameStart 都崩）
- **现象**：`TITLE→OPTION→左侧「ADV设定」` 与 `TITLE→GameStart` 两条路径都停在
  `stop: unimplemented @ eip=004E6E3F` / `detail: 软件中断（M1 不建模中断表）`（exit 2；同一抛点偶尔表现为
  `=== guest memory fault ===` + exit 3）。`0x4E6E3F` 是 `std::_Xout_of_range`（0x4E6E10, 0x30 B, 语料里 COLLAPSED）
  末尾 padding 的 `int3` —— **不是 abort**，也不是"没人接的 `RaiseException`"。
- **根因**：`sub_4581F0`（EA 0x4581F0）拿**当前字体的字面名**去 `sub_428990`（EA 0x428990）的字体名表
  （`this+0x313C0..0x313C4`，元素 32 字节，线性查，**找不到返回 -1**）里查；表里只有我们自己上报的那一条
  `Amayui CN`，而引擎配置里的字面名是 `ＭＳ ゴシック` / `ＭＳ 明朝`（还有引擎自带 `AGE Extend`）⇒ `-1` 被当
  下标用 ⇒ `0x4586A6 cmp edx,eax` / `0x4586A8 ja` 不跳 ⇒ `0x4586AA push "invalid vector<T> subscript"` +
  `0x4586AF call ?_Xout_of_range@std`。该表的**唯一填充点** = boot 时 `EnumFontFamiliesExA` 的回调 `Proc`
  （EA 0x45EAC0）。
- **修法**：`src/shims/gdi32_font.cpp` 的 `shimEnumFontFamiliesExA` 从"一条"改成"按 `kEnumFamilyNames` 逐条回调"
  （16 条：宿主 `Amayui CN` + `ＭＳ ゴシック`(0x52686C) / `ＭＳ 明朝` / `AGE Extend`(0x5208D8) /
  `IPA Pゴシック`(0x5268CC) / `IPA P明朝`(0x5268C0) + `.data:0x52985C` 那 10 条已知族名），
  字形仍一律来自宿主 Amayui CN（名字只是标签）；`@` 开头的竖排族名不报（`Proc` 跳过下标 0 的 `@`、
  `sub_428990` 查表时剥 `@`）。多回调走 `guest_call.h` 的"续延里再 `start()` 返回 false"（同 DDraw `EnumDisplayModes`）。
  ★ **回调极性**是「**非 0 = 继续、0 = 停止**」（MSDN `EnumFontFamExProc`；引擎 `Proc` 所有路径 `mov eax,1`）——
  先前的注释写成"非 0 = 停止"是错的，照它写会让枚举在第 1 条被我们自己的 shim 掐断、崩溃照旧。
- **读数（2026-11-10，macOS arm64 / RelWithDebInfo）**：修前 A `exit=3` + `guest memory fault`（栈
  `0x4E6E3F ← 0x4586B4 ← 0x458A9D ← 0x4716B0 ← 0x4567DA ← 0x42346B ← 0x41346D ← 0x4BB51C`）、B `exit=2` +
  `软件中断`（insns 898,729,338）；修后 A/B 都 `exit 0` + `detail: input script: quit`（A insns 1,176,455,637 /
  B 1,008,053,822）。日志新增 `EnumFontFamiliesExA: 回调 16 条族名 …` + `枚举完成：16 条族名`。
- **代表守卫**：`apps/emulator-lift/tests/gdi.cpp` 的 `enum-font-families`（pure：枚举条数 > 1、5 条查表目标
  逐字节命中、每条 `lfCharSet` = 请求值）与 `enum-font-families-stop`（回调返回 0 ⇒ 只 1 条）；
  端到端 `apps/emulator-lift/tests/title_entry_paths.cpp`（assets，安装根 `links/install` + `LIFT_ASSETS` 才注册）。
  变异实测：名单缩回一条 ⇒ `lift-gdi` 6 failures + 同一二进制 A/B 端到端都 exit 2 且回到 `0x4E6E3F`。
- **复现**（`apps/emulator-lift`）：`--game-dir ../../links/install --skip-logo --present window --present-hidden
  --raster gpu --max-frames 400 --time-limit 150 --input-script <A|B>`；A = `at frame 130 / click 812 622 /
  at frame 200 / click 112 216 / wait 100 frames / quit`，B = `at frame 130 / click 1175 355 / wait 100 frames / quit`。
  ★ 脚本末尾的 `wait` **是判据的一部分**：`quit` 若与最后一次 `click` 同帧生效，目标画面还没建起来就退出 ⇒
  崩溃不发生 ⇒ 守卫恒绿（实测）。
- **相关但独立**：同一次崩溃的异常本该被 WinMain 的 `catch(...)`（`.data:00544784`）接住，但我们的 SEH 在
  `disposition=1` 且 CONTEXT 未改时"继续执行" ⇒ 异常被静默丢弃、落到 `_Xout_of_range` 的 `int3`（这才是
  `detail: 软件中断` 这个误导性文案的来源）。单开 `REQ-01M72GAA7CVKBPP9QRT0PRPBTM`。

### BUG-19 · `dispatch-tax/block-scan` 断言的是一条**均值本身≈0** 的假命题 ⇒ 墙钟抖动时会自己变红（2/6）
- **现象**：`ctest --test-dir out` 偶发红在 `tests/dispatch_tax.cpp` 的 `LIFT_CHECK(fit_off.b >= fit_on.b)`
  （"跳过令牌锁的那一臂每块边际成本不低于正常臂"）。同一条命令重跑就绿 ⇒ 看起来像"环境抖动"。
- **根因（实测，不是猜）**：20 次连跑（`./out/lift-dispatch-tax`，HEAD 版）的 `Δb` =
  `-12.79 1.64 -11.25 3.88 -9.75 -15.52 16.94 -13.01 19.16 9.95 6.74 1.74 -3.33 4.93 16.55 5.89 -9.44 -6.89 -8.33 -0.62`
  ⇒ **mean -0.18 / σ 10.67 / 10 次为负**，而要测的"每块一次 mutex 的成本"只有 ~3 ns。
  三条机制：① 两臂各跑一次长循环 ⇒ 两个读数相隔数百 ms，频率/热/调度不同步；② 三点拟合的三个点也各占一段时间
  ⇒ 慢漂移把拟合线整体**倾斜**，直接变成截距的噪声；③ `b` 本身不是"锁成本"的好载体 —— 配对差 `Δt(k)` 随 `k`
  增长（k=1≈2.5 ns、k=8≈5–20 ns/块），三点拟合把这份成本摊进了**斜率** `c` ⇒ 截距差只剩 ~3 ns。
  同类还有一处：① 的 `token_ns >= lock_ns` 用**中位**（token 中位实测在 8.4–17.3 ns 之间摆），20 次里撞到过一次红。
- **修法**（只改 `apps/emulator-lift/tests/dispatch_tax.cpp`）：片循环把 (档, 臂) 六个格子**轮流**各跑 10 000 块
  （同一台 Machine），每格取**逐片每块时间的中位**（实测出现过 +3457 ns/块 的单片），先测哪一臂也轮流
  （否则"后测的那一臂"普遍更快，实测偏差可达 +20 ns/块）；判据拆成
  (a) `Δb >= -5 ns`（由 20 次实测分布定：满载那批 `Δb ∈ [-1.37, +5.92]`、mean +3.08、σ≈1.8 ⇒ 均值下 ~4.5σ）
  (b) `dt1 >= max(0.5 ns, 10% × 未竞争 lock+unlock)` —— `k=1` 的**配对差**（同片相邻，实测 σ≈0.1 ns），
  这才抓得住"诊断臂其实没跳过锁"；`token_ns >= lock_ns` 改成 **7 样本取最小**。
- **读数**：20 次连跑 0 失败（`Δb ∈ [2.57, 3.16]`、`dt1 ∈ [2.41, 2.61]`）；`ctest -R ^dispatch-tax$` 3/3 Passed。
- **变异（判据真会红）**：把 `src/runtime/guest_token.cpp` 的 skip-lock 改成 no-op（3 行）⇒ 稳定红在
  `tests/dispatch_tax.cpp:555` 的 `dt1 >= dt1_floor`（`dt1 = -0.18` vs 下界 0.50），**而 (a) 仍是绿的**
  （`Δb = -0.35 > -5`）—— 这正是"`b` 的信号与噪声同量级"的直接证据，也是为什么必须另有一条锐判据。
- **教训**：**墙钟微基准不许断言"某个均值关系的符号"**；要么降噪到噪声远小于信号（配对 + 中位 + 轮换），
  要么把判据放到**同一时刻的配对差**上，并给出容差的实测来源。

### BUG-20 · 输入脚本的条件永不满足 ⇒ 动作在 run 结束时**静默丢弃**（唯一线索是"**没有** `[input-script] finished`"）
- **现象**（2026-11-10，macOS arm64 / RelWithDebInfo）：脚本 `at frame 130 / click 1190 630 / at frame 151 /
  profile_begin tail`，本二进制末帧 = **150** ⇒ `at frame 151` 永不触发。run 正常 `ExitProcess`（exit 0），但
  stdout 没有 `=== profile window: tail ===`、**没有** `[profile-window] begin "tail"`，stderr **没有任何告警**；
  唯一的痕迹是**缺少** `[input-script] finished` 那一行。缺这一行同时意味着「脚本跑完了 / 条件写错 / run 提前停了」
  三件事 —— 当前**分不开**（这一单就是拿"窗口没开、表全 0"当成了读数，而不是报错）。
- **根因**：`InputScript::finished()` 是**存在性**观测（`applied_ == actions_.size() && releases_.empty()`），
  `[input-script] finished` 只在它为真（或 `quit`）时打 ⇒ **没被应用的动作在 run 结束时一条读数都不产生**。
  运行摘要的 `inputstat :` 段只报 `ticks` / `action-scans` / 抬起账，不报"还剩几条、第一条是什么"。
- **修法**（`src/runtime/machine.cpp` 的 `dumpRunSummary`，`input_script_.active()` 段**内**、`inputstat :` 的缩进
  续行（`release :` / `marker :`）**之后**——它自己是行首锚定的新字段，夹在续行中间会把一组读数切开）：
  `active() && !finished() && !quitRequested()` 时打一行
  `input-script: unfinished pending=<n> releases=<m> first="<原动作行>"`。`pending` = `actionCount() − appliedCount()` ·
  `releases` = `pendingReleaseCount()` · `first` = **文件序第一条** `applied == false` 的动作的 `Action::text`
  （`pending==0` 但还有没交付的抬起时取第一条 `Release::text`）。★ **⛔ 不能取 `actions_[applied_]`**：动作是
  **按到期 / 边沿**应用的（`tick()` 逐条判到期、`noteMarker()` 按边沿）⇒ 应用顺序**不一定**是文件顺序，
  `applied_` 只是计数。实测反例：`at frame 50 / mouse_move 10 10 / at insn 30 / mouse_move 20 20` ⇒ 后一条先应用，
  `actions_[applied_]` 会报**已应用**的 `20 20`、把真正没跑的 `10 10` 藏起来（复审就是这么抓到的；守卫见
  `unfinished-out-of-order-applied`）。
  含空格 ⇒ 双引号包住、内部 `"` 换成 `'`（行首锚定、可解析）。三个 `const` 访问器在 `src/runtime/input_script.h`
  （`unfinishedActionCount` / `firstUnfinishedActionText` / `firstPendingReleaseText`）：⛔ 不给可写引用、越界返回空串。
  ⛔ 不加 CLI 参数、⛔ 不改退出码（摘要是每次 run 都打的那一份且机器可解析；改退出码会把既有脚本全变红）。
  ★ 它**不替换** stdout 的 `[input-script] finished`：那条在 `finished()` **或 `quit`** 时打，而 `quit` 路径写的
  是**总**动作数（`actions_.size()`；实测 `at insn 10 / quit / at insn 20 / wait 1 insn` 打的是 `2 条动作全部应用`
  而实际只应用了 1 条）⇒ ⛔ 别当"应用数"读。
  ★ `quit` 之后剩下的动作是脚本**有意**跳过的 ⇒ 不报（否则正常脚本会天天告警）；
  0 条未应用（且没有未交付的抬起）⇒ 不报。
- **读数**（2026-11-10，macOS arm64 / RelWithDebInfo，`--present headless --raster gpu`；两轮只有 `--input-script`
  与私有 `--appdata` 不同，都 exit 0、停因 `stopped @ eip=004EE0EA` / `detail: shim 主动结束运行：kernel32!ExitProcess`）：
  · 永不满足那份（`last_frame=150`，与上面的现象同一末帧）⇒ 摘要里
    `input-script: unfinished pending=1 releases=0 first="profile_begin tail"`；stdout **没有** `[input-script] finished`，
    stderr **没有** `[profile-window]` 行。
  · 只留 `at frame 130 / click 1190 630` 那份 ⇒ 该行**没有**，`[input-script] finished：1 条动作全部应用…` **在**。
  · 位置上那行排在 `inputstat :` 的缩进续行 `release :` **之后**（`inputstat :` 那一组先读完，再看这一行）。
- **代表守卫**：`apps/emulator-lift/tests/input_unfinished.cpp`（pure，顶层 `add_test(NAME input-unfinished)`）七例
  （35 checks）：`unfinished-never-due`（`at frame 50` + `mouse_move`，run 一次 Present 都没有 ⇒ 行在、`pending=1`、
  `first` 逐字）· `unfinished-all-applied`（行不在、`[input-script] finished` 在）· `unfinished-quit-negative-control`
  （`quit` 后面还有动作 ⇒ 行不在）· `unfinished-out-of-order-applied`（`at frame 50` + `at insn 30` ⇒ `first` 必须是
  **没跑的** `10 10`）· `unfinished-quote-escaped`（`profile_begin a"b` ⇒ `first="profile_begin a'b"` 且带行尾 `\n`，
  摘要里没有裸 `a"b`）· `unfinished-no-script`（**一份脚本都没装** ⇒ 摘要里连 `inputstat :` 都没有，更没有这一行）·
  `unfinished-pending-release`（`pending=0` / `releases=1` ⇒ `first` 取抬起原文）。
  变异实测**五处**（每处：施加 → 构建 → 只跑这一条 → 红 → 还原 → 全文逐字节一致 → 复绿）：
  ① 去掉 `!quitRequested()` ⇒ `unfinished-quit-negative-control` 红（实参 `pending=1 … first="wait 1 insn"`）；
  ② `unfinishedActionCount()` 少算 1 ⇒ `unfinished-never-due` 红（实参 `pending=0 releases=0 first=""`）；
  ③ `firstUnfinishedActionText()` 改回 `actions_[applied_]` ⇒ `unfinished-out-of-order-applied` 红（实参
  `first="mouse_move 20 20"` = **已应用**那条）；④ 去掉转义循环 ⇒ `unfinished-quote-escaped` 红 2 条（实参
  `first="profile_begin a"b"`）；⑤ 把整段挪出 `active()` 段 ⇒ `unfinished-no-script` 红。
- **复现**（在 `apps/emulator-lift` 下）：
  `./out/emulator-lift --game-dir ../../links/install --appdata <私有 tmp> --skip-logo --audio off --present headless
  --raster gpu --input-script <脚本> --time-limit 300`；脚本 A = `at frame 130 / click 1190 630 / at frame 151 /
  profile_begin tail`（末帧 150 ⇒ 最后一条永不触发），脚本 B = 去掉后两行。
- **未做**：`at frame 151` 本身是**脚本写法**问题（末帧号随档位变），本条只保证"写错了要能看见"；
  ⛔ 不改退出码、⛔ 不加"严格模式"开关。

### BUG-21 · `quit-tail` 夹具用 `at frame` 当锚（boot 期没有 Present）⇒ 为证明"夹具自己会红"养了一条 13 分钟的变异（**夹具已删**）

- **现象**：`quit-tail`（`tests/quit_tail.cpp`，assets 档、两臂各 ~44 s）在 Windows 上先红成
  24 checks / 1 failure（1203 s，两臂都停在 `--time-limit 600`）；为证明"锚退回帧号会红"，
  `tools/mutate-check.mjs` 给它配了一条 `ctestTimeoutSec: 1800` 的测试侧变异 ⇒ 每轮自检 ~13 min。
- **根因（一行）**：`at frame N` 在 boot 的**帧停滞**里被折算成墙钟
  （`[input-script] ⚠ 帧停滞 30001ms（n=0）⇒ 剩余 frame 触发条件按最近帧间隔均值…折算成 wall_ms`，
  `apps/emulator-lift/src/runtime/input_script.cpp`），而 boot 段没有 Present ⇒ 帧号不推进。
  **这条行为本身是文档化的、且有纯档守卫**（`tests/input_tick.cpp` ④、`tests/profile_window.cpp` ④、
  `tests/input_anchor.cpp#anchor/no-stall-fallback-while-pending`）⇒ 夹具拿帧号当锚是**夹具写法错**，
  不是引擎缺陷；端到端重跑它只能量到"夹具自检 + 读数稳定性"。
- **修法（2026-11-11 裁决）**：**删掉整个夹具**（`tests/quit_tail.cpp` + CTest `quit-tail` + 两条测试侧变异），
  两张相关需求单按"夹具已删"drop。理由（`README.md` §0.1 三问）：
  ① `present-tail` 字段接错由纯档 `tests/input_injection.cpp#present-tail-stamp` 守；
  ② 端到端那半（各 1 行 `present-tail` + `stop=stopped` + `detail` 含 `kernel32!ExitProcess`）由
     `tests/native_override_e2e.cpp` 断言；
  ③ 剩下的"锚命中 / 两次逐字相同"是**测试自己**的结论，属观察 → 文档，不配守卫。
- **留下的规则**：e2e 夹具的触发锚用 **`at guest_ms` / `at insn`**（guest 时间跨机器确定），⛔ 不用 `at frame`；
  `--appdata` **每次 `remove_all`**（退出路径会写 `SAVE.DAT`：同一二进制、同一锚，空 appdata 的
  `tail_insns=131104827`，带上一轮存档的 `143751706` ⇒ 两臂会差一帧）。
  Windows 子进程命令行**只用 UTF-16**（`CreateProcessW` + `path::wstring()`），
  `path::string()` 走 ANSI 代码页会把 `天結` 转成 GBK 字节、`utf8ToWide()` 严格解码失败 ⇒ 子进程丢 game-dir；
  仍在用的样例见 `tests/title_entry_paths.cpp`。
- **代表守卫**：无（夹具已删）。"夹具写错要能看见"这一类由 `tests/input_unfinished.cpp` 的未应用动作摘要承担。
- **读数**（2026-11-11，macOS arm64 / RelWithDebInfo / `LIFT_ASSETS=1`，删前最后一跑）：
  `ctest -R '^quit-tail$'` = **Passed 87.91 s**；直接跑夹具 = **43 checks / 0 failures**，
  两臂逐字相同：`last_frame=1493 insns_at_last_present=1191989436 tail_insns=131104827`、
  `click frame=1472 guest_ms=120038 waited_samples=3`、`stop=stopped`。`tail_insns` 与 Windows 实测相同。
- **复现（历史）**：`apps/emulator-lift` 下 `./out/emulator-lift --game-dir ../../links/install --appdata <全新私有>
  --skip-logo --audio off --present headless --raster gpu --input-script <at frame 130 / click 1190 630>
  --time-limit 600` ⇒ boot 里点空、run 停在墙钟兜底。
