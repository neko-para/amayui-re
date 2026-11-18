# 缺陷史（defects）

> 这份文件**替代"每个缺陷配一条用例"**。一条缺陷 = 现象（用户能看到什么）+ 根因（一行）+ 修法（`文件:行`）
> + 何时/谁发现 + **一条**代表守卫（没有就写"无守卫，仅文档"）+ 复现路径。
> ⛔ 不为每个缺陷加回一个用例；⛔ 不写"还剩几条"（会过时）。守卫门槛见 `README.md` §0.1。
> 编号 `BUG-NN` 一经分配不再改；补记在后面追加。这里的**读数**是历史上的观察，不是当前状态。
> ★ **2026-11-16 起 `--interaction-graph` / `--interaction-trace` / `--interaction-fail-dump` 已退役**
> ⇒ 本文里的这些复现命令是**历史命令**；配方今天的入口 = AOT 臂 `--input-plan`
> （`apps/emulator-lift/scripts/recipes/*.plan`，索引见那里的 `README.md`）。

### BUG-01 · 第二次进 OPTION 必然停（纹理 arena 耗尽，一次往返净泄漏 18.08 MB）
- **现象**：进 OPTION → 右键退回 TITLE → 再进 OPTION ⇒ 停在装载中，纹理缺失/黑块，stderr 一句 `surface 分配失败（5898240 字节）：guest 堆耗尽`。
- **根因**：`ComRegistry::release` 到 0 只 `free(vtable)+free(obj)`、**无析构回调**；arena 像素块只在 iface 的 `Release` shim 里回收，而内部有 11 处裸 `cpu.com->release`（`SetTexture`/`SetRenderTarget` 换绑、`destroyForDevice`、Sprite 批次）⇒ 所有权挂在**调用点**上，"最后一次引用由裸 release 降为 0"时像素块永久留在 arena。
- **修法**：`src/runtime/com.cpp:282-286`（到 0 的唯一收敛点先跑 `on_zero` 再 free）+ `src/runtime/com.h:94,125`；纹理在 `src/shims/d3dx9.cpp:1031,1092` 登记 `d3d9::destroyTextureState`；回收见 `src/runtime/d3d9.cpp:936-943`。★ `IDirect3DSurface9` **不登记**（`GetSurfaceLevel` 的表面复用纹理状态块，登记会提前释放像素）。
- **读数（修前，实测非推断）**：三时刻 arena `in_use` = 108,595,152 → 120,693,568 → 126,672,232 B ⇒ 一次往返 **+18,077,080 B**；退回 TITLE 后最大连续空闲块 **5,814,312 B** < 这张纹理要的 5,898,248 B ⇒ 第二次必失败。
- **何时 / 谁发现**：2026-11-08，静态诊断（`.tmp/task2-report.md` §0-§5）→ 端到端实测定量（`.tmp/req-option-leak-body.md` §"真因"）。
- **代表守卫**：原 CTest `option-roundtrip/3cycles`（1/2/3 轮往返的 `in_use`/`peak` 不随轮数增长，容差 1 MiB；**已删**，2026-11-14）；回收面单点由 `tests/d3d9.cpp#d3d9/texture-bound-then-guest-release` 钉。两者都做过"短路 `onZero` ⇒ 当场红"的变异。
- **复现**：`--input-script <TITLE→OPTION→右键→再进 OPTION>`（`.tmp/input/opt-cycle2.txt`；帧 120 点入、172 右键回、234 再进、`--max-frames 400`）。
- `option-roundtrip`：`./out/emulator-lift --appdata <每次全新私有 tmp> --game-dir ../../links/install --present window --present-hidden --native-overrides all --skip-logo --input-script <脚本> --max-frames 700 --assert-guest-token`（脚本 = `at frame 130` + 两轮 `click 812 622` / `wait 50 frames` / `click 640 360 right` / `wait 70 frames`，末尾 `quit`；看 run summary 的 `arena :` / `token :` 与有没有 `guest 堆耗尽` / `unimplemented` 字样。★ 原 CTest 2026-11-14 按测试模型删除 —— 业务正确性不作判据，这不是门禁）。

### BUG-02 · 同一个失败被记成"guest 用了没实现的导出"（误诊链）
- **现象**：摘要与 stderr 都是 `=== unimplemented D3DX9 call === D3DXCreateTexture` 并进 `called-unimplemented` 计数 —— 看起来"缺一个导出"，实际是**我们自己**的容量问题。
- **根因**：`d3dxTrap` 是唯一出口：`createTextureState` 的任何失败（含 arena 耗尽）都送进去，它固定打 `"unimplemented D3DX9 call"` + 写 `cpu.stop_why` + `++cpu.shim.unimplemented_calls` ⇒ 两种完全不同的停因在账上长得一样。
- **修法**：`src/shims/d3dx9.cpp:104-125` 新增 `d3dxResourceStop`（独立标题 `=== D3DX 资源耗尽（不是 unimplemented）===`、独立 `stop_why = "D3DX 资源耗尽"`、**不** `++unimplemented_calls`）；`d3dxTrap`（`:90`）只留真缺口；同处补 `w×h / D3DFORMAT / 像素字节`。
- **何时 / 谁发现**：2026-11-08，`.tmp/task2-report.md` §6（归因链逐行取证）。
- **代表守卫**：原 `option-roundtrip/3cycles` 断言全程**无** `guest 堆耗尽` / `unimplemented` / `D3DX 资源耗尽` 字样（该 CTest **已删**，2026-11-14；复现命令见 BUG-01）。
- **复现**：`.tmp/task2-report.md` §6 的 `d3dx9.cpp:1011-1016` 调用链；stderr 原文见 `.tmp/req-option-leak-body.md` §"症状"。

### BUG-03 · 音频 feeder 与主线程并发跑 guest 代码 ⇒ 宿主堆损坏（0xC0000374）
- **现象**：`option-roundtrip` 跑 2–3 轮时**随机**崩在 `0xC0000374`（宿主 CRT 堆元数据损坏，**无应用帧**），同一脚本有时全绿；实测失败率 1/4 完整测试。
- **根因**：窗口模式 ⇒ 真实时钟 ⇒ 音频喂料起**真线程**（`src/runtime/audio.cpp:151`），feeder 执行 guest 代码（`audio.cpp:1286`）而它的 CPU 是 `audio_cpu_ = *cpu_`（`:1087`）⇒ `GuestHeap::blocks_`（`src/runtime/heap.h:64`）、`HandleTable::entries_`（`handles.h:103`）、`Vfs` 与主线程**同一批对象且无锁** ⇒ 两线程同时 `HeapAlloc` 时 `std::map` 树旋转写坏宿主堆。
- **修法**：`GuestToken`（`src/runtime/guest_token.h`）＝任一时刻最多一个宿主线程持有令牌，互斥点在**每次 shim 调用**（`ShimRegistry::invoke`）：feeder 的整次 fill **不**持，只有它的 shim 调用持（`audio.cpp:1068-1071,1179-1185,1223,1271`）。`--assert-guest-token` ⇒ `token : violations=0 max-concurrent=1`，违规退出码 4。
- **排除项（省得再查）**：`metrics()` 悬垂、`GuestHeap` double free（`heap.cpp:116-121` 是 `abort()` 不是静默）、`SurfaceArena` 指针相撞、`on_zero` 写坏宿主堆 —— 四条都被逐条否证（`.tmp/heapbug3/report.md` §2-§5）。
- **何时 / 谁发现**：2026-11-08；两轮 ASAN 未拿到栈（`.tmp/heapbug2/report-round3.md` §2-§3），第三轮静态审计定到并发（`.tmp/heapbug3/report.md` §1），A/B 见 `.tmp/audiord/*-summary.txt`。
- **代表守卫**：CTest `guest-token-stress`（`tests/guest_token.cpp`，2 线程 × 200 000 次真实 `GuestHeap` alloc/free；把 `acquire/release` 短路成空操作 ⇒ 红）。原端到端复现件 `option-roundtrip` 已删（2026-11-14；复现命令见 BUG-01）。
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
- **代表守卫**：原 CTest `text-baseline-lift`（`assets` 档，需安装树；断言"关掉补偿后墨迹顶正好低 2px"）**已删**（2026-11-14，测试模型：断的是我方视觉策略 `baseline_lift==2`，不作判据）—— 口径留在 `fonts.md`，复现命令见下行；`SCOPE.md:111` 的声明本身仍在。
- `text-baseline-lift`：**原命令已不可按原样复现** —— 它用已退役的 `./out/emulator-lift` 与 `--font`（另单 `REQ-01M7Q9D7NMFVKAN4PY57B5Y5MM`）。换成 AOT 臂后 `--max-frames 1` 只 dump 到近黑首帧（lift2 vs lift0 = 0 px），不能复现"墨迹顶高 2 px"。现行复现改用 `docs/03-emulator/fonts.md` §6.1 的 CONFIG 配方 + 10px 注音区域比对：`ink` 顶在 `--baseline-lift 0 / 2` 下 = 611 / 609（后者丢顶横），见 `REQ-01M7Q9C16JR9ATFJK11A873B26`。
- **复现**：`.tmp/task3-report.md` §4 的 4 条路径对照；`--baseline-lift 0` 与缺省各跑一次逐帧 BMP 比对。
- **后续（2026-11-18）**：新缺陷单 `REQ-01M7Q9C16JR9ATFJK11A873B26` 接在这一条之后 —— 补偿改成**按像素字号查表**（≤16px → 1、其余 `baseline_lift`；同字号统一、⛔ 不做逐字型夹取），并把 `--baseline-lift` / `--baseline-lift-by-size` 装回 AOT 臂（`./out/emulator-lift-aot`，旧 `./out/emulator-lift` 已不存在）。10px 注音行不再丢顶横；`--baseline-lift-by-size 0` 可逐字节复现旧行为。口径见 `apps/emulator-lift/SCOPE.md` §5.2。

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
- **现状（2026-11-13，测试侧第一批删除已做）**：`heap_layout` / `gdi_font_metrics_cache` 两条 CTest 与源文件**已删**（连带 `tools/mutate-check.mjs` 的对应条目）；`heap_layout` 的真不变量（提交后未写页读 0）搬进 `tests/memory_pages.cpp` 的 `commit-unwritten-zero`，度量缓存的真 GDI 合同（换字号 ⇒ 度量必须变）搬进 `tests/gdi_font.cpp` 的 `metrics-size-change`。同批按同类判据删掉 `agerc_ui_state` / `recovery_dialog` / `title_entry_paths` / `mip_e2e` / `overlay-window-smoke` / `emulator-lift-smoke`。`conformance-rejects-hollow-case` 不在删除集内（它是身份判据的负控，0.5 s，现在默认档）。
- **现状补遗（2026-11-13 晚，测试模型重做）**：`quit-graph` / `save-graph` / `close-confirm` / `option-roundtrip` 这类**缺陷复现件**按新模型**不进测试**（业务正确性不作判据）⇒ `repro` 档取消，它们的复现命令留在本文件 / `observations.md`。口径见 `REQ-01M7ATAB5A18W7XBCKFBEQS7HB`。⇒ **待处理**：`input_tick` 的 `actionScans()`、`gpu_batch` 的 `cuts==1`/`submits==2`、`profile_window` 的 `calls=` / 分项和（不在本批）。
- **现状（2026-11-14，本批落地）**：这四条 CTest 与源文件**已删**；复现命令 = BUG-01（`option-roundtrip`）· BUG-22（`quit-graph`）· `observations.md` OBS-20261112-05（`save-graph`）与 OBS-20261114-01（`close-confirm`）。同批删除的其它业务 e2e：`interaction-graph` / `interaction-script-e2e`（BUG-16）· `skip-logo`（OBS-20261114-02）· `native-override-e2e`（OBS-20261114-03）· `base-coverage`（OBS-20261114-04）· `text-baseline-lift`（BUG-08）。

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
- **代表守卫**：原 `apps/emulator-lift/tests/interaction_graph.cpp` 的 `graph-scripts-parse` / `parse-button-right` /
  `emit-button-carried` 与真机交互图 run **都已删除**（2026-11-14，测试模型：业务正确性不作判据）⇒ 只留复现命令
  （见下两行）；终态级判据现在靠人读 trace。
- **复现（成功路径）**：`--interaction-graph scripts/interaction/room-exit.graph` ⇒ exit 0 + trace 末 `task done`。
- `interaction-graph`：`./out/emulator-lift --game-dir ../../links/install --appdata <每次全新私有 tmp> --skip-logo --audio off --present headless --interaction-graph scripts/interaction/room-exit.graph --interaction-trace <room.trace> --time-limit 900` ⇒ 人读 trace：`node … enter` / `when script …` / `task done`，退出码 0/5 只作读数（★ 原 CTest 已删，不进测试）。
- `interaction-script-e2e`：`./out/emulator-lift --game-dir ../../links/install --appdata <每次全新私有 tmp> --skip-logo --audio off --present headless --interaction-graph <await-title.graph> --interaction-trace <e2e.trace> --time-limit 300`（图 = `await script TITLE` / `timeout 180000` / `quit`）；`LIFT_LOG_MARKERS=1` 再跑一次同一形状的 `await screen changed` 图 ⇒ 日志里应有 `[marker] frame=… name=` 行（★ 原 CTest 已删，2026-11-14）。
- **重开条件**：若再出现"点了没反应"，先确认观测窗盖住了一次 TITLE 重载帧 —— ⛔ 别再用 `--input-script` 里
  `at insn` 的硬锚点当终点（11-09 就是那么误判的）。

### BUG-17 · 纹理上传的行距按 256 对齐、SDL/Metal 按目标宽度算源行距 ⇒ 整张纹理被削成横条（**已修**）
- **现象**：`--raster gpu --raster-sprites 1`（本次改动前的缺省）进 **OPTION** 或 **EU-ROOM** 后，整块大背景（房间的糕点店照片、OPTION 列表文字）呈**横向条带 / 撕裂**；同一帧里的小 UI 面板（按钮框、图标）正常。同一帧把 sprite 交给 CPU（`--raster-sprites 0`）画面**完全正确**。
- **不是哪次提交引入的（已用重建证伪）**：在 `2bd8eaf`（`5e4fca3` / `8141659` 之前）重建后跑同一路径，得到**逐字节相同**的撕裂帧（sha256 `d0e0af01…`）。⇒ 这是 `--raster gpu` 的**既有**缺陷，与那两次提交无关。
- **已排除（每条都实测）**：
  - **不是 CPU/GPU 光栅不等价**：`LIFT_GPU_VERIFY=1` 的逐批 CPU 参考 vs GPU 读回对照，42 批**全部 `diff_pixels=0`**。
    ★ **该开关已删**（`REQ-01M7A74YG08R65NJBFXF7DEJDK`：`gpuEligible` 拒批翻"跳过"后 CPU 参考光栅不再是可走的一条路）⇒ 这条读数是**历史**，不可按原样复现；现在的像素对照用 `--dump-frame` / `--present-screenshot` 的逐像素 max-diff。
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
  逐字节命中、每条 `lfCharSet` = 请求值）与 `enum-font-families-stop`（回调返回 0 ⇒ 只 1 条）。
  ★ 原来的端到端复现件 `apps/emulator-lift/tests/title_entry_paths.cpp`（assets + `LIFT_ASSETS`）已于
  2026-11-13 删除 —— 它的根因就是上面这两条，而它自己是"固定帧号 + 点像素"的**缺陷复现件**、不是设计
  的合同（同批裁决把这类整体移出测试；`repro` 档已于 2026-11-14 取消，复现命令只留在本文件 / `observations.md`）。
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
- **根因（实测，不是猜）**：20 次连跑（`./out/tests/lift-dispatch-tax`，HEAD 版）的 `Δb` =
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
  ★ 上面是本段当时的 v1 记录；**现行口径**见 `REQ-01M72WHFMCWQ0KEJDQG0XH21AH`（`quit` 提前结束也照实报「已应用 / 共」）。
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
  ② 端到端那半（各 1 行 `present-tail` + `stop=stopped` + `detail` 含 `kernel32!ExitProcess`）原由
     `tests/native_override_e2e.cpp` 断言 —— 该 CTest **已于 2026-11-14 按测试模型删除**（复现命令见 BUG-22 与本条末尾）；
  ③ 剩下的"锚命中 / 两次逐字相同"是**测试自己**的结论，属观察 → 文档，不配守卫。
- **留下的规则**：e2e 夹具的触发锚用 **`at guest_ms` / `at insn`**（guest 时间跨机器确定），⛔ 不用 `at frame`；
  `--appdata` **每次 `remove_all`**（退出路径会写 `SAVE.DAT`：同一二进制、同一锚，空 appdata 的
  `tail_insns=131104827`，带上一轮存档的 `143751706` ⇒ 两臂会差一帧）。
  Windows 子进程命令行**只用 UTF-16**（`CreateProcessW` + `path::wstring()`），
  `path::string()` 走 ANSI 代码页会把 `天結` 转成 GBK 字节、`utf8ToWide()` 严格解码失败 ⇒ 子进程丢 game-dir；
  仍在用的样例见 `tests/host_encoding.cpp` 的 `runChild`（Windows 分支拼 `std::wstring` 命令行）。
- **代表守卫**：无（夹具已删）。"夹具写错要能看见"这一类由 `tests/input_unfinished.cpp` 的未应用动作摘要承担。
- **读数**（2026-11-11，macOS arm64 / RelWithDebInfo / `LIFT_ASSETS=1`〔历史档名；现为 `LIFT_STRESS`〕，删前最后一跑）：
  `ctest -R '^quit-tail$'` = **Passed 87.91 s**；直接跑夹具 = **43 checks / 0 failures**，
  两臂逐字相同：`last_frame=1493 insns_at_last_present=1191989436 tail_insns=131104827`、
  `click frame=1472 guest_ms=120038 waited_samples=3`、`stop=stopped`。`tail_insns` 与 Windows 实测相同。
- **复现（历史）**：`apps/emulator-lift` 下 `./out/emulator-lift --game-dir ../../links/install --appdata <全新私有>
  --skip-logo --audio off --present headless --raster gpu --input-script <at frame 130 / click 1190 630>
  --time-limit 600` ⇒ boot 里点空、run 停在墙钟兜底。

### BUG-22 · 音频 feeder 与主线程**共用 TEB** ⇒ `fs:[0]`（SEH 链头）被另一条线程改写 ⇒ Quit 的 C++ throw 派发到已复用的 record ⇒ `eip` 落到 guest 堆
- **现象**：`graphs/sn0000-quit.graph`（TITLE→ADV→FELLOW→TITLE→Quit）在 Quit 处**不是正常退出**，而是
  `stop : unimplemented @ eip=0x109A5A71`（用户实测）；同一命令重跑 EIP 每次都不同
  （`0x109C0341` / `0x103676C1` / `0x10357221`），偶尔是 `decode-error @ 0x00544774`。玩家侧 = "点了退出，游戏没退"。
- **根因**：音频 feeder 是**真宿主线程**、跑 guest 代码（`sub_4B6C30` 的 Ogg 解码链上有 SEH prologue），
  而它的 `fs` 与主线程**同一份 TEB**（`audio_cpu_ = *cpu_` 只拷了寄存器，`fs_base` 仍指 `0x7FFD0000`）。
  于是 feeder 的 `mov large fs:0, eax` / epilogue 会改写**主线程的 SEH 链头**：主线程在 Quit 的
  `_CxxThrowException`（`0x418E74` → `call RaiseException` `0x4E8AB0`）读到的链头要么是 feeder 栈上的 record、
  要么是一处**已过期**的主线程 record。两条路的 record 内存都已被复用 ⇒ `Handler` 是数据（堆 EA / ThrowInfo）
  ⇒ 按数据当代码执行。读数：一次 run 里 feeder 改写共享 `fs:[0]` **173,755 次**；
  `chain[0] record=0x6FDFFBA8 region=stack handler=0x109C0340 region=heap`。
  真 Windows 的 `fs` 指向**每线程**的 TEB，这条通道不存在。
- **修法**：`src/runtime/audio.cpp` 的 `setupFeederContext()` 给 feeder 建**自己的 TEB**（`GuestLayout::feeder_teb` = `0x7FFD6000`；提交前显式查占用 —— `commitZeroed` 对已占页是**静默清零**；
  `0x7FFD3000` = guest-text、`0x7FFD5000` = 纯档 argv 已占），抄主 TEB 的通用字段后覆盖每线程字段
  （`ExceptionList` / `StackBase` / `StackLimit` / `Self` / `UniqueThread`），再把 `audio_cpu_.fs_base` 指过去。
  诊断落点：`src/runtime/exec.cpp` 的 `unimplemented()` 打印 `regionOf(eip)` + 最近 24 条 ring +
  "数据地址当 eip"警告（⛔ 不改 dispatch 语义）。
- **何时 / 谁发现**：2026-11-12，用户实测 → `.tmp/audio-token/quit-unimplemented-analysis.md` 静态取证 →
  本轮流式取证（`LIFT_SEH_TRACE` 的 feeder `fs:[0]` 观察点 + 链头逐环区域归属）定因。
- **代表守卫**：原 CTest `quit-graph`（`tests/quit_graph.cpp`，**repro 档**：`LIFT_REPRO=1` 才注册；
  真 feeder 臂、单臂）：修后 `stop : stopped @ 004EE0EA` + `kernel32!ExitProcess`，链头 `0x6FFFFB74`（`region=stack`）、
  handler `0x00513B90`（`region=image`）。变异 `tools/mutate-check.mjs` 的 `ctest:quit-graph`
  （feeder `fs` 指回主 TEB）⇒ 当场红。★ 2026-11-13：这条是**缺陷复现件**，与 `save-graph` /
  `close-confirm` / `option-roundtrip` 一起移进 `repro` 档（默认门禁与 `LIFT_ASSETS=1` 都不注册）；
  原来的"同一 graph 两遍"同时删成单臂。★ **2026-11-14：整条 CTest + 源文件 + 变异条目已删**（测试模型：
  业务正确性不作判据）；只剩下面的复现命令。
- **复现**：`--appdata <私有 tmp> --game-dir ../../links/install --present window --present-hidden
  --raster gpu --native-overrides all --skip-logo --interaction-graph ../../graphs/sn0000-quit.graph
  --max-frames 4000`（⛔ headless 不是有效臂：本图会停在 step3 等 GAMESTART 超时；`--audio off` 没有 feeder）。
- `quit-graph`：`./out/emulator-lift --appdata <每次全新私有 tmp> --game-dir ../../links/install --present window --present-hidden --native-overrides all --skip-logo --interaction-graph ../../graphs/sn0000-quit.graph --interaction-fail-dump <quit.fail.bmp> --max-frames 4000 --time-limit 300 --verbose --log-cat dsound,general` ⇒ 人读判据：`stop : stopped` + `kernel32!ExitProcess`，且**没有** `unimplemented` / `decode-error` / "数据地址当 eip"（★ 原 CTest 已删，不进测试）。

### BUG-23 · alpha test 的批**整块不画** ⇒ 该图层整块消失（不是变半透明）；而 `LineCaps.ALPHACMP` 还在声明（假话）
- **现象**：带 `ALPHATESTENABLE=1` 的 Draw 被 GPU 准入**整批拒绝** ⇒ **这一批几何一个像素都不出现**。
  玩家侧 = **该图层整块消失**：colorkey 贴图**整张**不见、镂空树叶 / 发丝**整个四边形**不见、
  UI 边框与文字块成片不见。⚠ 与"混合参数配错"完全不同（不是变半透明、也不是边缘不对）——
  形状 / 位置 / 深度都对，只是**没有像素**。
- **根因（一行）**：`src/runtime/d3d9_pipeline.cpp` 的 `gpuEligible` 里
  `if (ctx.alpha_test) { *why = "GPU 不支持 alpha test（ALPHATESTENABLE=1）"; return deny(kUnGpuAlphaTest); }`
  （具名位 bit30）。CPU 光栅删掉之前这条状态是**逐片元**正确执行的（8 个 D3DCMP + `ALPHAREF`），
  删掉之后变成"整批消失" ⇒ **能力回退**，不是范围裁剪。同时 `src/runtime/d3d9.h` 的
  `kCapsLineClaim` 当时仍是 `0x0E`（声明了 `ALPHACMP(0x08)`），而 caps 的已裁决口径是
  "caps 是**声明**、不是实现集；本线只能响亮拒绝的能力**不能表达就清 0**" ⇒ 那一行是**已经裁决过的假话**。
- **修法（2026-11-14，备选路：清位 + 说清后果）**：`src/runtime/d3d9.h` 的 `kCapsLineClaim`
  `0x0E` → `0x06`（去掉 `ALPHACMP`）；`tests/d3d9.cpp#d3d9/caps-claims` ④ 从
  `caps_bit ⇔ suppressed` 的双向蕴含改成**两件独立事实**（caps 里没有该位；带 alpha test 的批
  `D3DERR_INVALIDCALL` + `skipped_draws +1` + bit30）；`src/runtime/d3d9_pipeline.cpp` 的
  `noteLinePointCapsOnce()` 文案与打印同步成 `0x06` / "ALPHACMP 未声明（alpha test 整批拒绝）"。
  ⛔ **没修"整批不画"本身**：实现 alpha test 要给片元着色器加变体 + 扩缓存键（pipeline 变体），
  以"真场景有命中证据"为条件 —— 本条的读数（下面）不满足该条件。
- **读数（2026-11-14，Windows x64 / RelWithDebInfo / 当前工作树）**：boot→TITLE `--max-frames 600`，
  `GPU 不支持 alpha test` **0 次命中**、`unsupported :` 里**没有 bit30**（只有 `…0800` 的 bit11）。
  ⇒ 这 600 帧里没有可观测后果；⚠ **ADV / 游戏内没量到**（`sn0000.graph` 停在 GAMESTART 之前）
  ⇒ 这是**潜在雷**，不是"已排掉"。原始读数见 `docs/03-emulator/observations.md` 的 `OBS-20261114-09`。
- **何时 / 谁发现**：2026-11-14，需求单 `REQ-01M7AZ65EAE7Q6STW2S0ZNXGM4` 复核
  （`git show HEAD:apps/emulator-lift/src/runtime/d3d9_pipeline.cpp` 里删掉的 CPU alpha test 19 行
  vs 今天的 `deny(kUnGpuAlphaTest)`）；父级裁决走备选路（清位 + 说清后果），
  实现 alpha test 以"有命中证据"为条件。
- **代表守卫**：`apps/emulator-lift/tests/d3d9.cpp` 的 `d3d9/caps-claims` ④（CTest `d3d9-gpu`，顶层 pure 档，
  没有 GPU 设备时 SKIP 77）—— 断言 caps 无 `ALPHACMP` **且** 该批 `INVALIDCALL` + `skipped_draws +1` + bit30；
  无 GPU 环境用 `apps/emulator-lift/tests/gpu_batch.cpp` 的 `gpu-batch/skip-ledger` ②（CTest `gpu-batch-order`，
  pure：原因文本仍进 `skipped top:`、具名位仍进 `unsupported :` 的 `bit30=…`）。
  ★ 重开实现的条件：任何真场景日志出现 `GPU 不支持 alpha test` 或 `unsupported :` 的 bit30
  ⇒ 按需求单"首选"那节的两臂判据（`ALPHAFUNC=NEVER` ⇒ 0 像素、`ALWAYS` ⇒ 像素出现）实现，并把本单改回 bug。
- **复现 / 排障（`apps/emulator-lift` 下）**：
  ```powershell
  # ① 命中面：这条命令在 boot→TITLE 600 帧里应输出 0 次命中（= 没有可观测后果）
  ./out/emulator-lift.exe --appdata ../../.tmp/alphatest/appdata --game-dir ../../links/install `
    --present headless --native-overrides all --skip-logo --max-frames 600 `
    --log-level info --log-cat d3d9 2>&1 | Tee-Object ../../.tmp/alphatest/replay.log
  Select-String -Path ../../.tmp/alphatest/replay.log -Pattern 'GPU 不支持 alpha test'   # 命中批数
  Select-String -Path ../../.tmp/alphatest/replay.log -Pattern 'unsupported :'           # 看有没有 bit30
  # ② 合同（清位后的 caps + 拒批的账）
  ctest --test-dir out -R 'd3d9-gpu|gpu-batch-order' -V
  ```
  看到"某个图层整块不见"时：先查 `unsupported :` 有没有 `bit30=GPU 不支持 alpha test`、
  `skipped top:` 有没有 `GPU 不支持 alpha test（ALPHATESTENABLE=1）`（**默认级别各只出一次**）——
  有就是本条，不是混合参数配错。

### BUG-24 · AOT 点 **OPTION** 响亮停：菜单 switch 的 `case 14`（`0x41B92D`，跳表 `jpt_41B672` 就在 `.text` 里）从未被抬升
- **现象**（2026-11-15 用户实测）：`emulator-lift-aot` 跑到 TITLE 后点 **OPTION** ⇒
  `stop : decode-error @ eip=0041B92D（非正常结局） — EA 0041B92D 既不在派发表里（closure 缺这一行？）也不是 shim 桩 EA ⇒ 响亮停（查找 1 次比较；表 33496 项）`。
  走同样路径到 TITLE 是正常的，**只有点到这一支才撞上**。
- **已证事实**（可复算）：
  1. `0x41B92D` 是菜单命令 switch 的 **case 14**：
     ```
     .text:0041B672   jmp  ds:jpt_41B672[edi*4]        ; switch jump
     .text:0041B974   jpt_41B672 dd offset loc_41B679  ; 跳表本体在 .text 内部（共 15 项，case 0–14）
     .text:0041B92D   loc_41B92D:  ; jumptable 0041B672 case 14
     ```
  2. **生成物里没有这个块**：`out/aot/lift_aot.ll` 既没有 `bb_41B92D`，也没有 `switch` 的 `i32 4307245` case
     ⇒ 该 EA **从未被解码/发射**。
  3. 同表另外 12 个 case 目标**都有 `bb_*` 块**（所以那些分支能跑）⇒ 跳表**大体被识别了**，**只丢了最后一项**。
  4. **不是回归**：`apps/emulator-lift/aot/closure.txt` 未被修改（入库件）；派发表 `entries=33496` 与
     `OBS-20261115-03`（今天之前）完全一致 ⇒ 可派发集合没变。
  5. **范围更广**：语料里 `jmp ds:jpt_*[reg*4]` 型 switch **56 处**；抽 6 张表，"case 目标不在闭包根"比例 =
     `5/6`、`6/6`、`11/13`、`15/15`、`9/13`、`11/13` ⇒ 菜单/命令分发型代码里点到没走过的分支都可能撞上。
- **机制（2026-11-15 定死；纯只读取证，每步有读数）**：**表被当成指令吃掉了 ⇒ 真正的 case 目标落在一条"垃圾块"内部
  ⇒ 被 overlap 判据永久拒收**。因果链：
  1. `.lst:40920` 的 `0x41B96F call __CxxThrowException@8`（5 字节、**不返回**）⇒ 而"每条 call 的下一条 EA"
     被**无条件**塞进 entries/leaders（`lift_gen.cpp:437-441`）⇒ **`0x41B974` = 跳表首**成了 leader/派发入口
     （`lift_aot.ll:231122` 有 `i32 4307316, label %bb_41B974`）；
  2. 从表首解码：第一个 dword `0x41B679` 的字节是 `79 B6` ⇒ 解成 `jns 0x41B92C`（`.ll:231271-231278` 的 IR 形状完全对上）；
     jcc 目标 `0x41B92C` 成 leader；
  3. **阶段①里 case 体一个都没解码**（间接 jmp 没有静态后继）⇒ 没有任何块在 `0x41B923` 处截断它
     ⇒ 这条垃圾块从 `0x41B92C` **线性爬过 `0x41B92D`**；
  4. D26 判 `0x41B92D`：不在入口集、不是块首、**`BlockSpans::inside` 为真** ⇒ 记 overlap 并塞进
     `rejected_targets_`（**单调、永久**）；
  5. 同轮另外 10 个 case 目标被收下 ⇒ `0x41B923` 的块出现 ⇒ 那条垃圾块**后来**才被丢掉
     （所以 `.ll` 里只剩 `bb_exit_41B92C`），**但判过的不会再判** ⇒ `bb_41B92D` 永远不出现。
  ⇒ **排除的两个候选**：**① case 数读成 14 不成立**（`lift_aot.ll:231223` 是 `icmp ugt i32 %t41, 14`；
  表 15 项、索引 0–14、PE 值与 `.lst` 逐项一致）；**② 对齐过滤不成立**（D26 的 4 字节对齐判的是**表项地址**，
  对目标 EA 无要求；同表 `0x41B679`/`0x41B6AB`/`0x41B885`/`0x41B8C5` 都非 4 对齐且都被收下）。
  ★ 同一条垃圾块还吞了**真实的 `sub_41B9B0`**（生成日志 `[soft-root/重叠（call）] 0041B9B0`）。
  ★ 上图"13 个不同目标"是按 `loc_*` 数的；算上 `def_41B672`（= `0x41B963`，cases 7,13 共用）是 **14**。
- **全语料量化（62 张 `jpt_*` 表 / 663 项 / 605 去重）**：可派发 **573**；
  **缺块 25**（要新块，D26 型 —— 24 个在 `.lst` 的 COLLAPSED 区，正常区只有 `0x41B92D` 这一例）；
  **缺 entry 5**（只需 entries，D21 型，**0 新块**）；不在 `closure.txt` 的 **557/605**；有缺口的共 **7 张**。
  另有 **10/62 张表的表首本身就是块首 + 派发入口**（= 表字节被解码这个面的规模）。
- **修法方向（按形态，不按 EA；`closure.txt` 不动）**：
  - **S1 前置识别**：形态 = `Jmp` + `Mem` + `index≠None` + `scale==4` + `base==None`；表长**首选由前置 bounds check 反推**
    （`cmp idx,N` + 无符号 `ja` ⇒ 项数 N+1，并用"`ja` 目标 ∈ 表项"交叉验证），兜底才是"向后读到不像指针"；
    ⛔ 产品路径不读 `.lst`（测试可以 —— `LIFT_AGE_LST` 已是多条 CTest 的 env）；
  - **S2 表区间从解码里排除 + 表首不当 leader**（call 的返回 EA 落在表区间 ⇒ 不入 entries）⇒ 从根上断掉 `jns` 那条链；
  - **S3 表项在建块之前就成为 leader** ← **根治点**：今天 D26 是"块定型后才收"，所以只能丢；
    先立 leader，`buildBlocks` 会在 leader 处断开 ⇒ 那 25 个缺块自动修好，且"表项落进别人块"这一类消失；
  - **S4 间接 jmp → `switch`**（仍读 guest 内存、语义逐位不变）、0 派发，与 S7 的目标形态一致；越界语义需单独裁决。
  - 阶段：`P0 只读审计 → P1 排除 → P2 立 leader → P3 switch → P4 D21/D26 退位`，每阶段可独立回退；
    守卫 = 新 CTest `--jmptab-audit`（断言"每张真实跳表的每个 case 目标都可派发 + 表区间无块首"，
    **今天红、P2 后绿**）+ 变异 M1（bounds 谓词反转）/ M2（去掉 exec-range 过滤）。
  - ⚠ 不确定项（未写成结论）：盖住 `0x41B92D` 的那条块是 `[0x41B92C, X)` 属**推断**（`blocks` 无 dump 口、
    垃圾块后续被丢所以 `.ll` 里看不到）；10/62 里只有 2 张能点名触发者；24 个 collapsed 区缺块目标的机制未定。
- **⛔ 不许只把这几个 EA 手工塞进 `closure.txt`**（那修好一个菜单、留 61 张表）。
  修完要给"62 张表里还有多少 case 目标未进闭包 / 未可派发"的**前后读数**。
- **复现 / 排障**（`apps/emulator-lift` 下）：
  ```powershell
  .\out\emulator-lift-aot.exe --skip-logo --present window --max-frames 3000 `
    --game-dir ..\..\dist\install --appdata <每次全新的空目录>
  # 在 TITLE 上点 OPTION ⇒ 应不再出现 decode-error @ 0041B92D
  ```
  离线核对（不跑游戏）：`Select-String corpus\disasm\files\AGE.EXE__dumped.sectfix.EXE.lst -Pattern "jumptable 41B672"`
  应列出 case 0–14（含 `loc_41B92D`），再逐个查 `out\aot\lift_aot.ll` 里有没有对应 `bb_*`。
  全量取证脚本 / 结果 / 设计单曾是本地临时稿（`.tmp/aot/jpt/`，已删除 ⇒ ⛔ 不可再引用）；
  可再校验的判据只有下面这条 CTest。
- **代表守卫**：CTest **`lift-jmptab-dispatch`**（`add_test` 在 `CMakeLists.txt` **最外层无条件** ⇒ `pure` 档；
  跑 `emulator-lift-gen --jmptab-audit --exe <sectfix> --roots aot/closure.txt`，退出码 = 判据）。
  它断言四条**性质**（⛔ 不断言当前数字）：P-1 表区间落在同一个可执行节内；P-2 表区间内**不存在任何已发射块首**；
  P-3 每个"在可执行节内 ∧ 能解码"的表项都**可派发**；P-4 表项无自指。
- **落地结果（2026-11-16，`9998381`）**：
  - 守卫**先红过**：只上 P0（审计模式、不动闭包）时 `exit 1` —— P-2 红 15 张表 / 34 个块首
    （来源分类 `call-ret` 8 · `jcc` 16 · `none` 10）、P-3 红 5 张表 / 30 项；P1+P2 落地后全过。
  - `bb_41B92D` 出现（块 + 派发 case + `lift_table.inc` 行）；覆盖它的那条垃圾块消失；
    `.ll` 里表首 `bb_41B974` 从 **2 处 → 0 处**（表字节不再被当指令解码）。
  - **62 张 `.lst` 表的可派发目标 577 → 605（缺 28 → 0）**；缺块 25→0、缺 entry 3→0。
  - **关档零回归的硬证明**：`--no-jmptab-exclude --no-close-jmptab-items` 下 `.ll` 的 sha256 与改前
    **逐字节相同**（`C9FF6054…E14FB49`），`roots/entries/functions/blocks/insns` 逐项相等 ⇒ 新代码在关档下完全惰性。
  - 默认档读数：`blocks` 71,929→71,995 · `insns` 349,663→349,396 · `entries` 33,496→33,509 ·
    `functions` 10,211→10,254 · D21 `added` 23→5 · D26 软根 603→75。
- **★ 变异实测的教训（比这条 bug 本身值钱）**：设计单给的两条变异（M1 把 bounds 谓词 `ja`→`jbe`、
  M2 去掉"值必须在可执行节内"）**在默认档都不红** —— M1 反过来后策略 A 全部失效退回策略 B，
  而 B 逐项读**精确**，表长照样对；M2 被"离表 >64 KiB 就停"兜住。**能真红的两条**是：
  M1′ 把项数取 `imm` 而不是 `imm+1`（配 `--no-close-jmptab`）⇒ 收敛硬红并**点名**
  `0044A3A0 0049F730 004C80B0`；M3 让 P2 不注入 ⇒ P-3 红 4 张表 / 22 项（正是那 4 张**地址非 4 字节对齐**的表）。
  ⇒ 教训：**变异必须能点名机制**，否则"它会红"是空话。
- **量化口径的两处纠正（本轮实测）**：① 设计单写"缺 entry 5 个"**实际是 3 个**
  （`0x4F5F81` / `0x4F88BB` 是把 `def_*` 符号名硬解成 EA 的幽灵，PE 真值 `0x4F619D` / `0x4F8C5F` 早就在派发表里）；
  ② 那 24 个 collapsed 区缺块的**真因不是 overlap**，而是那 4 张表的**地址非 4 字节对齐**
  （`0x4F1A01` mod1 / `0x4F542A` mod2 / `0x503AAE` mod2 / `0x509903` mod3）⇒ D21/D26 的 `i += 4` 扫描**一个都看不到**它们。
  ★ 另外识别出 **8 张 `.lst` 里没有名字的真表**（PE 字节级证据：真 `FF 24 8D/95` 站点 + CRT `memcpy`/`memset`
  的二级分派 + 表后 40–67 B 的 case 体簇 + 末项后一个 dword 是代码字节而非指针 ⇒ 终止干净），
  它们贡献 `functions` +18。**拒绝它们等于在 `memcpy` 里复制一遍本条缺陷。**
- **⛔ 不要再用"62 张表 / 663 项 / 605 去重"当验收值**：识别面是**解码流**而不是 `.lst` 标签，
  真实候选是 **76 个 disp32**（62 张与 `.lst` 同址 + 14 个 `.lst` 无名的），终态是 `tables=70`（6 个假站点被判据 ② 打掉）。

### BUG-25 · AOT 臂：`EnumSystemLocalesA` 的 guest 回调续延桩写的是**字面量**，只有解释器认得 ⇒ 「加载存档」响亮停
- **现象**（2026-11-15 用户实测）：AOT 臂走「**加载存档**」路径 ⇒
  `[kernel32] EnumSystemLocalesA：续延桩 0x7FFEF000 不可调用 —— 这台 cpu 没有块缓存（X86::cache == nullptr，AOT 臂 / 手搓夹具），
  而宿主的桩表里也没有这个 EA（ShimRegistry::add 不发 stub_ea）⇒ 无法执行 guest 回调的续延。`
  同一条路径在**解释器臂**（`emulator-lift`）下正常。
- **机制（已定死，每条有行号）**：
  1. 「续延桩」= 一个**内部 shim** `lift!__guest_callback_return`（`kernel32_string.cpp` 的 `registerStringShims`，
     用 `s.add(...)` 登记，**不是导入**：不进 IAT、不进 overrides）。
  2. `ShimRegistry::bind` 的 extras 循环**早就给它发了真桩 EA**（桩页 `[0x7FFE0000, 0x7FFE8000)`、8192 槽）。
  3. 但 `startGuestCallback` 往 guest 栈上铺的返回地址是**字面量** `0x7FFEF000` —— 它落在桩页**之外**
     ⇒ `byStubEa` / `idForStub` 查不到。
  4. 解释器能跑，只因为 `cpu.cache->installShim(0x7FFEF000, id)` 把那个字面量塞进了 `BlockCache`
     （块缓存能装**任意** EA）。AOT 没有块缓存，识别集合 = 派发表 ∪ 桩页内 ⇒ **接不住**。
  5. ⇒ 这不是"缺续延执行能力"，是**铺错地址**：能用的 EA 一直在注册表里，只是没人用它。
- **裁决（两条，都是本轮定的）**：
  1. **修法**：铺栈那个地址与 `installShim` 都改用 `cont->stub_ea`（为 0 才惰性发号，仍拿不到 ⇒ Trap 响亮停，
     ⛔ 不退回字面量）；守卫换成**与臂无关**的不变量 `cpu.cache != nullptr ∨ byStubEa(cont_ea) 命中`。
     零新槽（`registered=385 / bound-stub-ea=385 / next-slot=385` 修前修后一个没动）。
  2. **⛔ 那条注释里"将来改成 `addRuntime` 会自动放宽"是假话，已按事实重写**：守卫读的是**常量字面量**，
     而桩 EA 是 `页基 + 槽号*4`（槽号由登记序定）⇒ 没有任何常量能等于它 ⇒ 守卫**必须改**。
     同理"复审建议 (a)"的**字面实现会炸**（注册期 `shim_slots_ == 0` ⇒ `addRuntime` 直接失败）。
  3. **★ 由此得到一条规则（已进 `AGENTS.md` §12.8）**：**shim 回调的续延 = 一次普通 shim 调用**；
     它的 EA **必须来自桩页分配**（`addRuntime` / `bind` 发号），⛔ **不许写字面量**。
     判据不是"这条臂能跑"，而是"**每一条臂的识别集合里都有它**"。
     ★ 佐证：合成 EA `0x7FFEF004/008/010/014` 也是字面量，但**两臂各有一段 pump** 才没出事——
     这正是"手写数字然后只教一条臂 ⇒ 另一条臂必漏且静默"的正面/反面两个例子。
- **族的大小（本轮枚举）**：按"EA 必须落进该臂的识别集合"分类 —— 精确同族（字面量 + 只有 `installShim`）= **1 处**（就是本条）；
  合成 EA 型 4 个 EA + 2 处多回调续延、`addRuntime` 真桩 EA 型 8 个调用点，**两臂都通**（对照组）；
  另一族是真的要"嵌套 guest 执行器"（`native_image_upload`，**但它只是解释器侧的 native override**，AOT 从不装
  ⇒ 见 `REQ-01M7CZH25AVZWVZH4JH408CJPB` 的 2026-11-16 更正，⛔ 不是 AOT 的缺口）。
- **代表守卫**：`apps/emulator-lift/tests/locale_ext.cpp` 的用例 **`enum-locales-no-cache`**
  （并进既有 CTest `locale-ext`，`pure` 档，0.1 s）。形状：把 `EnumSystemLocalesA` 整个流程放到
  **`cpu.cache = nullptr`** 的 `X86` 上、**按 AOT 的方式派发**（桩 EA ⇒ `lift_shim`；guest 代码由解释器代跑到栈顶返回地址），
  断言 2 个 locale 的回调都真的发生、停因不是 `Trap`、且栈上那个 EA 能被 `byStubEa` 查到。
  ★ **修前它红**（`568 checks, 12 failures`，退出 1，打的就是用户看到的那句话）；**修后绿**。
  变异：把 `cont_ea` 改回字面量 ⇒ 该用例当场红。
- **可证伪读数（本轮实测）**：解释器 `--trace-shims` 193,182 行**修前修后 SHA256 相同、逐行 0 差异**；
  AOT 300 帧语义指纹逐位相同（`exit 0` / `frames 300` / `stop @ 00498AE4` / arena 三元组 / `table misses=0`）。
- **未做（如实记）**：AOT 侧那条 `LIFT_AOT_SHIM_TRACE=1` 的「1 行 `kernel32!EnumSystemLocalesA` + 2 行续延」
  读数**没拿到** —— boot 的 300 帧不经过「加载存档」那一屏，而 AOT 臂只有 `--input-plan`
  （三条指令 `await script` / `wait` / `click`），**没有**解释器那套 `--input-script` / `--interaction-graph`
  控制面，也还没有一份经验证到那一屏的配方
  ⇒ 需要一次人工点窗口的 AOT 跑。**用户 2026-11-16 手工确认：该提交点上「加载存档」已正常。**
### BUG-26 · 同一输入两臂结果不同：TITLE 上 `click 1070 480` 在 AOT 生效、在解释器无反应

- **现象**（2026-11-16 探测时撞到；13 次 run，每次全新空 `--appdata`）：

  | 臂 | `region`（in_use / surfaces / heap_blocks） | 判定 |
  |---|---|---|
  | **AOT**（`--input-plan`，`click 1070 480`） | `117490928 / 25 / 6821`（两次 run 逐位相同） | 相对基准 **+8.9 MB、+4 surfaces** |
  | **解释器**（同一动作） | 与**无动作逐位相同**；`engine_marker` 停在 `TITLE.BIN`、无新 identity（`frame=` 全是 0） | **无反应** |
  | 基准（不给 plan） | `108595152 / 21 / 6674`（三次相同） | — |
  | 特异性对照（中性 `click 640 360`） | `108595152 / 21 / 6676` | 只多 2 个 block ⇒ 不算反应 |

  ⇒ 同一份**语义**输入（⚠ 不是同一个 `--input-plan` 文件：解释器臂今天**不接** `--input-plan`），
  一条臂换了画面、另一条臂什么都没发生。
- **为什么这条比"配方对不对"重要**：如果两臂对同一输入给出不同结果，那么**所有"解释器能跑通 / AOT 跑不通"
  的结论都可能是输入路径差异而不是引擎差异**。这条不查清，后面每个"某处 AOT 表现不符"都要重打折扣。
- **两个候选原因（都未定）**：
  1. **时间步长**：两臂 guest 时间步长差 ~**15 倍** ⇒ 同一条帧号门（旧配方的 `await presents 150`；
     今天在 `--input-plan` 里已是**解析期硬错**）对应的 guest 时刻差很远；
  2. **解释器侧 `bounded-no-sample=1`**：可能让 `region` / marker 在实际渲染前就收口。
  ★ 第三种必须一起排除：**两臂走的是两条不同的施加路径** —— AOT 是本轮新接的 `deliver*`（`runtime/input.h`），
  解释器是老的 `InputScript::tick`。若两条路径对"同一动作"的语义不同（按下/抬起时机、坐标缩放、客户区 vs 屏幕坐标），
  那是**实现分歧**，比时间步长严重。
- **判据（可判，两臂各跑一次）**：同一次点击后，两臂的 `region` 三数**要么都变、要么都不变**；只有一侧变 ⇒ 分歧成立。
  ⚠ 两臂今天**没有同一份配置面**：AOT 用 `--input-plan`（三条指令），解释器要用 `--input-script` /
  `--interaction-graph` 复现同一个动作 ⇒ 判据只能是"同一份**语义**动作"（点击的客户区坐标 + 生效后的
  guest 时刻粗略对齐），⛔ 不是"同一个文件"。
  命令（两臂都要 `--skip-logo`、都要**全新空** `--appdata`、都要 `--region-log`）：

  ```powershell
  .\out\emulator-lift.exe     --skip-logo --present window --max-frames 400 --appdata <新> --region-log <同一份语义动作>
  .\out\emulator-lift-aot.exe --skip-logo --present window --max-frames 400 --appdata <新> --region-log --input-plan <同一份语义动作>
  ```
- **⛔ 两个读数的教训**（本轮实测，别再踩）：
  - `stop @` 在 `--max-frames` 下**两侧恒同**（`00498AE4`，停在 Present 桩）⇒ **无证明力**；
  - `lastHash` 每次 run 都不同（`clock=real-time` + TITLE 是动画屏）⇒ **不是稳定指纹**；
  - ⇒ 可用的是**收尾 `region` 三数**：同一命令多次 run **逐位相同**，且随屏而变。
- **另记**：`ESC`（VK `0x1B`）在这条路径上**无区分力**（两侧落同一 `EA 00513B90` / 同一条 `agerc!_ShowDialog`，返回 IDOK = 退出程序）。
  ★ 旧口径"`--input-plan` 表达不了『同一份计划去掉动作』（只有门没有动作 ⇒ `缺动作行`、exit 2）"
  **已不成立**：现在是线性指令流 ⇒ 只写门 / 只写 `wait` 都合法，干净的"去掉 click"对照**可以**写。
  （本轮读数仍拿"不给 plan"当基准；那批 13 次 run 是旧语法时代取的。）
- **复现**：本机 13 次 run 的原始记录是本地临时稿（`.tmp/aot/`，已删除 ⇒ ⛔ 不可再引用）。
  今天等价的配方（三条指令，`--input-plan`）：

  ```text
  await script TITLE
  click 1070 480 right     # x/y = 1280×720 参考系；实测窗口档客户区 = 1280×720 ⇒ 与旧读数同义
  ```

  ⚠ 旧探测用的是 `await presents 150`（帧号门，今天**解析期硬错**）；`await script TITLE` 是**就近替换、
  不是等价**（两者的 guest 时刻不同，见候选①）。
  入库图 `graphs/title-loaddata.graph`（坐标 `1070,480` 是**用户给的**，非本仓推导）。

### BUG-27 · `bt/bts/btr/btc` 的**内存形**把位偏移截成低 5 位（寄存器形语义）⇒ 客侧 `_strcspn` 跳错槽 ⇒ `_setlocale` 失败 ⇒ 日文文本按**字节**量

- **现象**（2026-11-17 用户口径 + 读数）：SC0000 的加载确认框「ロードしますか？」两颗按钮偏宽
  （引擎客户区口径 `0xA0+0x1D*9c` = **334 px**，字符口径应 **247**）；`i2c6` 的 `local 9c = 6`（应 3）。
  ⇒ 中文/日文文本在日文执行环境里按字节算宽度，UI 排版整体偏宽。
- **根因**：Intel SDM（BT/BTS/BTR/BTC 的 Operation 段）——**寄存器形**才把位偏移截到操作数宽度的低位；
  **内存形**的位偏移是**地址**偏移：访问 `EA + (BitOffset DIV 位宽) * 字节数`，只有余数选位。
  我们两臂都无条件截成低 5 位 ⇒ 客侧 CRT 的 `_strcspn`（32 字节位图、`bts/bt [esp],eax`）跳错槽：
  `"japanese"` 的第 4 字节 `'n'=0x6E` 与分隔符 `'.'=0x2E` 低 5 位同为 14 ⇒ 被误判成"命中分隔符"
  ⇒ 语言解析成 `"japa"`、国家 `"ese"` ⇒ LCID 查不到 ⇒ `setlocale` 返回 NULL ⇒ `mb_cur_max=1`
  ⇒ `__mbstrlen` 走 `strlen`。
- **修法**：`src/runtime/exec_integer.cpp`（`Op::Bt/Bts/Btr/Btc`，语义真源）内存形
  `dst.mem.disp += (off / 位宽) * 字节数`、`bit = off & (位宽-1)`；`src/aot/lift_gen_emit.cpp`
  `emitBitTest` 同一判据（`sdiv` + `loadMemAt/storeMemAt`，读写语义只有一份）。
  连带：`aot/closure.txt` 补 `0x004F80E0`（`EnumSystemLocalesA` 的回调 EA；它只以**未对齐立即数**
  `push $0x4F80E0` 出现在 `0x4F822B`，D26 的 4 字节对齐扫描抓不到 ⇒ 修好位偏移后第一次走到它就
  `decode-error @ 004F80E0`）。
- **读数**（mac arm64 / RelWithDebInfo / `split8-O2-lto-abi`）：`local 9c` **6 → 3**；按钮宽三种口径
  都 **−87**（青色填充 322→235 · 外沿 342→255 · 客户区 334→247）；60 帧语义指纹五项逐位相同
  （`entries` 33509→33517 = 闭包 +1 根的预期）。A/B 与打印：`.tmp/sc0000/locale-diag.md`。
- **何时 / 谁发现**：2026-11-17，用户指出"raw 上的排版也歪了就是我们的问题"→ 诊断 subagent 用
  `setlocale` 入参打印定位（`lang=[japa] country=[ese]`）。
- **代表守卫**：`tests/conformance_cases.cpp` 的 6 条内存形用例（`bt-mem-hi-slot` / `bt-mem-low-bit-only`
  负控 / `bts-mem-slot` / `btr-mem-slot` / `btc-mem-slot` / `bt-mem-word` 16 位）+ AOT 侧
  `tests/ops_diff.cpp` 用真实站点 `0x4F7C8D`/`0x4F7CA8` 两臂比较。变异：去掉 `disp` 修正 ⇒ 红；
  AOT 把 delta 折 0 ⇒ `lift-ops-diff` 红。⚠ **`conformance_cases.cpp` 只驱动解释器**：AOT 语义是
  构建期从 `AGE.EXE` 生成的，造"合成字节的 AOT 臂"= §12.8 禁止的第三份实现 ⇒ 两臂同判拆成两层。
- **复现**：`.tmp/sc0000/lc-dialog.plan`（`load24` 截到确认框）+ 全新 appdata；`emu-ctl screenshot`
  量按钮宽，或临时打印 `local 9c`。

### BUG-28 · `GetCPInfo` 的 `CPINFO::LeadByte` 写到 +4（文档是 +6）⇒ 0x81–0x9F 前导字节整段丢

- **现象**：与 BUG-27 同一用户可见结果（文本按字节量、按钮偏宽）。**只修 BUG-27 时 `local 9c` 仍 6**；
  两处都修才 3。
- **根因**：`CPINFO = UINT MaxCharSize(4) + BYTE DefaultChar[MAX_DEFAULTCHAR=2] + BYTE LeadByte[MAX_LEADBYTES=12]`
  ⇒ `LeadByte` 从 **+6** 起。旧实现写 +4：`0x81/0x9F` 落进 `DefaultChar`，`LeadByte` 只剩 `{0xE0,0xFC}`
  ⇒ 客侧 CRT 的 `__mbctype` 不把 `0x81–0x9F` 当前导字节 ⇒ 平假名首字节（`0x82`）按单字节数。
- **修法**：`src/shims/kernel32_bootstrap.cpp` `shimGetCPInfo`：`buf[6..9] = 81,9F,E0,FC`，
  `buf[4]/[5]`（`DefaultChar`）保持 0。
- **读数**：`GetCPInfo(932)` 的 18 字节块 `02 00 00 00 00 00 81 9F E0 FC 00 …`（修前 `… 81 9F E0 FC` 在 +4）。
- **何时 / 谁发现**：2026-11-17，同一轮诊断（A/B 的第二因）。
- **代表守卫**：`tests/locale_ext.cpp#get-cp-info-layout`（`MaxCharSize==2`、`+4/+5==0`、`+6..+9=={81,9F,E0,FC}`、
  其余字节不被写）。变异：`buf[6]→buf[4]` ⇒ 红。

### BUG-29 · AOT 臂把「要不要真 feeder 线程」判成 `has_machine` ⇒ 恒主线程 D 泵、预读 600 ms ⇒ 所有音效延迟 ~0.6 s

- **现象**（用户 2026-11-17 实测）：TITLE 上把鼠标**移进/移出按钮**触发的 SE，从"触发"到真正听见有明显可见间隔。
- **根因**（父级静态核实）：`runtime/audio_policy.h` 的 `audioThreadMode(mode, real_time_clock, has_machine)` 要求三者全真才走
  "host-thread（方案 C）"；调用点 `runtime/audio.cpp:203-208` 传 `has_machine = cpu_ && cpu_->machine`，而 **AOT 臂有意
  `cpu_.machine == nullptr`** ⇒ 谓词恒 false ⇒ AOT 从第一刻起就是"主线程泵（方案 D）"，并不是"起线程失败再回退"。
  D 泵的预读 lead 是 `audio.cpp:47` 的 `kLeadBytes = 600 ms`（同一处注释就写着"代价是 SE 也有 ~600ms 延迟"）；
  真线程档的 lead 是 `audio.cpp:50` 的 `kThreadLeadBytes = 100 ms`。
- **★ 用户裁决**：**D 泵从来不是被决策过的方案**，AOT 侧按 `has_machine` 判成恒 D 是**绝对的错误**。
  ⇒ 修法不是"把 lead 调小"，而是给 AOT 实现**真 feeder 线程**（feeder 自己的 TEB + 音频栈，⛔ 不共主线程 TEB ——
  共用就是 BUG-22 那条 `fs:[0]` 被另一条线程改写、Quit 的 C++ throw 派发到已复用 record）。
- **连带**：`audio.cpp:1325-1352` 的 `pump()` 反向锁序论证建立在"绝不存在 feeder 在跑而 pump 走 D"之上 ⇒ 谓词一改，那段注释与前提必须一起改。
- **何时 / 谁发现**：2026-11-17，用户报"hover 音效触发与播放有明显间隔"；父级在同一轮把它归到 `has_machine` 门与 600 ms lead。
- **需求单**：`REQ-01M7MHEZRQJSMAAWPESVW44DSK`（判据：AOT + `--audio sdl` ⇒ `feeder=host-thread`、lead=19200 B、TEB 不同、`--audio off` 输出逐字节不变）。

- **修法 / 读数（2026-11-17 已修）**：`audioThreadMode()` 降到两个事实（`mode == Sdl && real_time`），
  "能起 feeder"由 `attach(..., const GuestLayout&)` 的引用入参钉死；AOT 新增 `CriticalSectionTable`（feeder 的硬前提）、
  feeder 的 guest 执行器走 **AOT IR 派发**（新 `src/aot/aot_feeder.{h,cpp}`），TEB 用 `GuestLayout::feeder_teb`（自有，不共主线程）。
  **读数**：`pushed - played` 115200 B（600.0 ms）→ **19200 B（100.0 ms）**（4/4 次精确同值）；
  `calls=` 12 万级 → **0**；underruns / `fill_slow` 全 0；`dev_frames=1024`（21.3 ms，另一笔账）。
  **代表守卫**：`aot_audio_flags.cpp#s0-thread-mode-truth-table`（用户裁决的机械表达）+ `audio.cpp#audio-device/feeder-teb-is-own`（BUG-22 回归口）。

### BUG-30 · 存档列表显示 `1970/1/6 21:23`：`GetLocalTime`/`GetSystemTime` 把宿主 uptime 当 1970 epoch（且不做时区）（**已修**，2026-11-18）

- **现象**（用户 2026-11-18 实测）：存档列表的时间显示 `1970/1/6 21:23`；同一份存档的时间随宿主开机时长漂移。
- **根因**：`runtime/clock.h` 的 `rawNow100ns()` 取 `steady_clock`（零点 = 宿主开机），`now100ns()` 没有 epoch 项
  （`epoch_100ns_` 只进 `machine.cpp` 的 trace、从未被 set）；`shims/kernel32_time.cpp` 的 `GetLocalTime` / `GetSystemTime`
  直接把它按"1970-01-01 起"展开 ⇒ 引擎拿到的本地时间 = `1970-01-01` + 宿主 uptime。
  AOT 臂 `aot_run.cpp` / `aot_host.cpp` **强制** real-time ⇒ 产品臂没有虚拟档可躲。
- **写侧 / 读侧均无错**：写侧 `0x4377B6 GetLocalTime` → 0x124 B header `+0x108`（16 B `SYSTEMTIME`），`0x437869 WriteFile`；
  读侧 `0x438120` 原样搬 header、`0x42D700` 原样交给脚本，`SAVE.BIN` 用字面量 `/` `:` 画数字（所以不补零）。
- **读数（2026-11-18 / win32）**：宿主 boot `2026-11-12 04:35:16`（WMI，1 s 粒度），SAVE03.DAT `+0x108` =
  `b2 07 01 00 02 00 06 00 15 00 17 00 36 00 f9 02` ⇒ `1970-01-06(dow=2) 21:23:54.761`（自 1970 起 509,034.761 s）；
  `boot + 509,034.761 s` = `2026-11-18 01:59:10.8`，与该文件 mtime `01:59:11.204` 差 0.4 s（六份 header 全部 <1 s）。
  同一串按 `FILETIME` / `time_t` / `i64 100ns` 解释都对不上 21:23 ⇒ 不是未初始化内存。
- **次因（独立、被主因掩盖）**：`GetLocalTime == GetSystemTime`、不读宿主时区（`kernel32_time.cpp` 已登记偏差 1）
  ⇒ epoch 修好后画面仍是 UTC，与 +08 差 8 小时。`time` / `_time64` / `SystemTimeToFileTime` / `FileTimeToLocalFileTime`
  在 AGE.EXE 里**没有导入**，也无对应 shim。
- **修法方向（原登记；2026-11-18 已按此实施，见下）**：把"墙钟"与"uptime"拆开 —— `GetTickCount` / `timeGetTime` / QPC 仍用单调 uptime；
  `GetLocalTime` / `GetSystemTime` / `GetSystemTimeAsFileTime` / `GetThreadTimes` 用 `system_clock` 起步的墙钟；
  `GetLocalTime` 再做宿主时区换算。测试用注入墙钟 base + 时区，⛔ 不断言"当前年份"。
- **代表守卫**：无（`apps/emulator-lift/tests` 里 grep 不到 `GetLocalTime` / `GetSystemTime`）。建议新增 `kernel32_time/*`。
- **复现**：直接读 header：
  `[IO.File]::ReadAllBytes('<SAVE03.DAT>')[0x108..0x117]` 后按 `SYSTEMTIME`（16 B LE）展开。

- **修法（已实施，2026-11-18）**：
  · `runtime/clock.h` 新增 `wall100ns()`（**1970-01-01 00:00:00Z** 起算的 100 ns）：
    `wall_base_100ns_ + now100ns()`。real 档在 `setRealTime(true)` 捕获 `system_clock::now() - rawNow100ns()`
    （单调零点那一刻的墙钟）⇒ 之后只随单调增量走（宿主 NTP 回拨 / 手改钟不会让墙钟倒退或跳）；
    虚拟档 base 缺省 **0** ⇒ 与修前的展开**逐位相同**。`now100ns()` / `nowMs()` / `qpc()` 一行未改。
    测试缝 `setWallBaseForTest(base)`；冻结语义与 `now100ns()` 同源（暂停对墙钟同样不可见）。
  · `shims/kernel32_time.cpp`：`GetSystemTime` = UTC 墙钟；`GetLocalTime` = UTC → **宿主时区**；
    `GetSystemTimeAsFileTime` = 墙钟 + 1601 delta；`GetThreadTimes` 的创建时间用墙钟。
    `GetTickCount` / `GetTickCount64` / `timeGetTime` / QPC 不动。
  · 时区落在新头 `src/shims/time_zone.h`：Windows `SystemTimeToTzSpecificLocalTime(nullptr, …)`、
    POSIX `localtime_r()` 的 `tm_gmtoff` —— **DST 按被换算的那个 UTC 时刻判定**（⛔ 不看"现在是不是夏令时"）。
    测试缝 `setOffsetForTest(fn)` 注入 `(UTC 秒) → 偏移秒`，用例不依赖本机 TZ。
- **读数（2026-11-18 / win32 / RelWithDebInfo，修后）**：注入墙钟 `2024-03-05T12:34:56.789Z` + uptime `1 h` ⇒
  `GetSystemTime` = `2024/3/5(dow=2) 12:34:56.789`；同一时刻 `GetTickCount` = **3,600,000**（证明两条读数已拆开）；
  `GetSystemTimeAsFileTime - 0x019DB1DED53E8000` = 注入墙钟（往返一致）；`GetThreadTimes` 创建时间 = 同一 FILETIME。
  假 DST 规则（冬 +1 h / 夏 +2 h）下，`2024-01-15T12:00Z` ⇒ `GetLocalTime` 13:00、`2024-07-15T12:00Z` ⇒ 14:00，
  `GetLocalTime - GetSystemTime` 恰为注入偏移；冻结 5 s 期间墙钟与 uptime 都不前进，解冻后只随单调增量走。
  **CTest**：`kernel32-time` 59 checks / 0 failures（0.03 s）；全量 `ctest` = **109/109**（155.71 s，0 skip）。
  **变异**：`node tools/mutate-check.mjs --match kernel32-time` ⇒ `GetSystemTime` 退回 `now100ns()` 后该守卫**红**
  （CTest 退出码 8），1/1 按预期变红，之后全量重建回基线。
- **未做 / 待验**：**真存档画面未复验**（需要游戏安装 + 一次存档写读；本轮的判据是注入时钟的 CTest）。
  预期：`SAVE.BIN +0x108` 从 `1970/1/6` 变成宿主本地日期（+08 机器上比 UTC 多 8 h）。
- **代表守卫**：`kernel32_time.cpp / wall-vs-uptime-split`（uptime 与墙钟必须同时成立）、
  `filetime-and-thread-creation-use-wall`、`local-time-host-zone-dst`（假 DST 规则）、
  `virtual-base-plus-virtual-now`、`freeze-freezes-wall-too`；CTest 名 `kernel32-time`。

### BUG-31 · `--until` 只认派发主循环：`lift_dyn` 尾调用目标与 `call` 返回点停不住（假阴性）

- **现象**：`--until <EA>` 对两类 EA 永远不命中、run 只会 time-limit：
  ① 跳转表 case 体（`jmp ds:jpt_4B1832` → `lift_dyn` 尾调用）；
  ② `call` 的返回地址（如 `0x4A9F50` / `0x4AF05A`，`out/aot/lift_table.inc` 里没有这些块入口）。
  实测（2026-11-18）：blind wipe 的 4 条 mode 探针（`0x4B1F99 / 0x4B21DB / 0x4B286F / 0x4B2AB5`）全部 time-limit；
  阳性对照 `--until 0x4B06D0`（call 目标 = 块入口）当场命中。`D3DXMatrixDecompose` 的 E2E 用
  `--until 0x4A9F50 / 0x4AF05A` 4 次尝试全部 time-limit。
- **根因**：`--until` 的比较只在 AOT 派发主循环（`aot/aot_run.cpp`）；`runtime/lift_abi.cpp` 的 `lift_dyn`
  直接尾调用 `fn`，不检查 until；生成代码块内也不检查。
- **正确用法（临时绕开）**：用**派发块入口**（`out/aot/lift_table.inc` 里有的 EA），或 `call` 之后紧邻的块入口
  （`0x4A9F55` / `0x4AF05F`；后者尚未取得命中读数，属待核）。
- **修法方向（未实施）**：给 `lift_dyn` 加**仅诊断档**的 until 检查（全局 atomic 开关 + 比较 EA，默认档一次 relaxed load），
  或在派发表生成时把"call 返回点"列为可停块入口；⛔ 不要为诊断把检查塞进每个生成块。
- **代表守卫**：无（`--until` 目前只有 CLI 行为，无 CTest 钉"跳转表 case 能停"）。
- **复现**：`--until 0x4B21DB` 的 blind wipe mode 探针（命令见 `docs/03-emulator/wipe-profile.md`）⇒ time-limit；
  `--until 0x4B06D0` 同配方 ⇒ `until-ea` 命中。

### BUG-32 · 加载 SAVE04 → 教程询问选「否」→ 槽 0x24 纹理 vtable=0（fault EA 0x20；已修）

- **现象**（用户口径 2026-11-18，win 上；mac 1/1 复现）：加载 `saves/SAVE04.DAT`（第 5 槽）→ 进入地图 →
  「要接受迷宫教程吗？」选**不接受** → 进地图的 blind wipe / blur 转场里 `exit 3` +
  `=== guest memory fault ===`。★ **真实 fault 是 READ `0x00000020`**：
  `fault ea : 0x00000020 (host 0x300000020 - base 0x300000000)`；旧记录的
  `access: WRITE 0x1005BA8C` 是 AOT lifted 访存不更新 `g_access` 留下的**最近一次 shim 侧写**，不是本次 fault。
  原生 PC `lift_cc_4A50C0+3124 ldr w2,[x22,w9]`（x22=`mem`，w9=0x20）= `0x4A5125 mov eax,[edx+20h]`，`edx=0`。
- **根因（2026-11-18 父级 + `LIFT_WATCH` 实测）**：
  - 槽 0x24（`this+0xA5D8+0x24*4 = 0x1005AB58`）= P（1280×720 `$1$EVT90$` NormalTexture；`P+0x450` 的下一块头
    `'LIFT'+0x450` 完好，槽 0x25 的 vtable `0x528BE8` 正常）⇒ 只有槽 36 被就地写坏，不是悬空/复用。
  - `ID3DXSprite::Draw` 批次：`CreateSprite` 批次 base `0x10200D58`、容量 16、`kBatchEntryBytes=72`。
    `reserveBatch` 扩容到 32 时 `GuestHeap::realloc` **移动**块，但 `reserveBatch(X86&, const State&)` 只把新 base
    写回 **guest 状态块**，`draw()` 仍用旧 `s.batch` 写第 17 条（`count=16`）：
    `旧 base + 16*72 = 0x10200D58+0x480 = 0x102011D8 = P-8`。
  - 四条 4 字节写 `{pTexture, flags=5, 0, 0}` 覆盖 P 的块头与 `[P]/[P+4]`（vtable/base=0）；新批次第 17 条
    未初始化 ⇒ `End` 读到垃圾纹理 `0x0BED9FE3`（日志 `第 16 条 Draw 的纹理 … 已不是本模块的纹理对象`）。
  - watch 读数（`LIFT_WATCH='0x1005AB58:4>follow'`，`.tmp/bug32/logs/watch2.log`）：
    `WRITE 0x1005AB58 block=lift_cc_004A2CCF` → `follow → ptr=P` →
    `WRITE P-8 n=4 shim=ID3DXSprite::Draw`（连续四条）。
- **修复**：`reserveBatch(State&)` 成功后 `s.batch=np; s.capacity=cap;`（`src/runtime/d3dx9_sprite.cpp`）；
  `draw()` 用更新后的 base；另加"批次 base 不是活块/越界"只 warn 不 trap 的防御判据。
- **守卫**：`apps/emulator-lift/tests/d3dx9.cpp#d3dx9/sprite-batch-realloc-move`（Draw 17 次；断言新 base 的第 17 条、
  旧 base 之后未被写；修前 835 checks / 4 failures，修后 **836 / 0**）。
- **复现 / 验收**（macOS arm64 / `--present headless` / RelWithDebInfo）：
  ```bash
  cd apps/emulator-lift
  ./out/emulator-lift-aot --present headless --skip-logo --audio off \
    --game-dir ../../links/install --appdata <私有副本> \
    --input-plan scripts/recipes/save04-fault.plan --max-frames 12000 --time-limit 900
  ```
  - 修前：**exit 3** + fault（`eip=004A50C0` / `lift_cc_004A50C0`；`input-plan: fired=10 pending=7` 是 `_Exit(3)` 的形状）。
  - 修后：**不再 fault**；plan 走过 wipe 到 **AIM**（地图）第 2982 帧；配方尾部 `SC4000 0x10D00` 门是
    fault 路径专用的 ⇒ exit 5（**不是** fault，也没有 `End: 第 16 条 Draw` 告警）。
    `LIFT_WATCH` 修后没有 `P-8` 写（`.tmp/bug32/logs/watch3-fixed.log` / `watch4-fixed.log`）。
  - 门禁：`ctest -R '^(fault-report|d3dx9)$'` 2/2。
- **历史读数（修前，保留）**：交互式 `emu-ctl` 逐步 1/1；存档 sha256 `e8a6dbf3…`；
  崩溃前最后 sample `SC4000.BIN:0x00010D88` frame 1232；崩溃帧先建 effect 201/203。
  `.tmp/save04-tutorial/report.md` §3。★ 旧 `.tmp` 的 `await position SC4000 0x5900 0x5960` 是错的：
  正确门是 `[0x5800,0x5860)`（SC4000 停在 `0x5828` 等输入）。
- **`--input-plan` 点击前提（已推翻）**：trace 证明配方 click 被消费（无 click 对照 60 s 不成立），
  emu-ctl 同屏 click 施加形状相同（`at=PeekMessage` + `reason=forced`）⇒ `releaseDue`/`sampled` 不是根因，
  **未改 release 语义**。配方对教程“否”的 click 仍有时序敏感性（`wait 2000` 档成功率高，但不保证）。
- **未做**：win32 未跑（分支未编译）；watch 的 follow pending 是 `thread_local`（跨线程会漏）；
  `writeBytes/fillBytes/wr8/16/64` 的 watch 接线无正例（低风险跟进）；dump 在信号上下文逐字节 `fprintf`。
