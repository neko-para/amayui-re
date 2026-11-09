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

### BUG-15 · 出 EU-ROOM 的路径停在后缓冲 surface 的 `LockRect`（未实现），且**退出码 0**
- **现象**：在 `EU-ROOM`（房间稳定帧 hash `0x96E51755F00B2E9E`）里按 **ESC**（VK `0x1B`）⇒ 900 条指令后停：`stop: stopped @ eip=004A4F8D`、`detail: unimplemented D3DX9 call（shim com!IDirect3DSurface9::LockRect）`，而**进程 exit 0**。⇒ 自动化会把它当成"任务成功"。
- **根因**：两次叠加。① `src/shims/d3dx9.cpp` 的 `shimSurfaceLockRect` / `shimSurfaceUnlockRect` 对"后缓冲 surface（`state` == 设备状态块）"**明确 trap**（"⛔ 不把锁字段写进设备状态块"）⇒ 房间退出路径要读后缓冲，这里没有实现。② `src/runtime/machine.cpp:991-1005`：`had_impl` 为真但 shim 自己 `stop_why` 的停机被归成 `StopReason::Stopped`（`ok()` 为真）⇒ **未实现的方法槽被当成正常结束**。
- **读数（2026-11-09，macOS arm64 / RelWithDebInfo，`--skip-logo`）**：ESC 在 insn `1,400,000,000` 落下 ⇒ 停于 `1,400,000,900`；同一次运行 `--max-frames 600` 只呈现到第 318 帧（房间稳定帧从第 141 帧起）。对照：同一房间点 **CG 鑑賞**（`1100,150`）会正常切画面（hash 连变 3 帧）⇒ 不是"房间完全不吃输入"。
- **代表守卫**：无 pure 守卫（要真装才复现）。机械复现 = `--interaction-graph apps/emulator-lift/scripts/interaction/room-exit.graph`（任务没走完 ⇒ **exit 5**，stderr 打出断点与最后画面）。
- **复现**：`--game-dir <安装> --skip-logo --audio off --input-script <at frame 130 / click 944 562 / at insn 1400000000 / key_down 0x1B>`（房间入口 = TITLE 的 `Eushully-chan Room` 按钮，中心 `944 562`）。
- **同族**：退出码口径另见需求单 `REQ-01M6WW9R6SJ7P8XA4522XCR48A`（"资源耗尽硬停仍 exit 0"）—— 本条给那条单补了一个同类读数。

### BUG-16 · `EU-ROOM` 里的「戻る」按钮对**注入点击**没有反应（hover 有高亮）
- **现象**：把光标移到右下角「戻る」（约 `1200..1272 × 652..711`，框内均值 `0xB6957E`）⇒ 画面**会**变（按钮高亮，hash `0x96E5…` → `0xF13F…`）；但在同一位置点击 / 长按 4 s 再抬 / 双击，画面**一次都不再变**，也不产生任何停因。同一次运行里点 CG 鑑賞（`1100,150`）有效、TITLE 上点 OPTION（`812,622`）有效。
- **根因**：待查。已知排除项：① 光标与命中测试在工作（hover 会换高亮）；② 注入的按钮电平在工作（`GetAsyncKeyState` 走同一份影子状态，且 `click-release … waited_samples=3` 说明 guest 采到过）；③ 不是"房间不吃输入"（CG 鑑賞 有效、ESC 有效）。⇒ 怀疑方向：该按钮走的是另一条读点（例如只在某个定时器回调里轮询），或命中区与绘制区不同。
- **读数（2026-11-09，同机）**：三次独立运行（纯 click / move 后 click / `mouse_down` 4 s 后 `mouse_up`），房间帧 hash 稳定在 `0xF13F23F24EEEE636`（hover 态）不变。
- **代表守卫**：暂无（`room-exit.graph` 若把这行换回 `click center`，会以 `expect_change` **响亮失败** —— 那就是它的判据形状）。
- **复现**：`apps/emulator-lift/scripts/interaction/room-exit.graph` 的 `room` 节点把 `action key 0x1B` 换回 `action click center`。
