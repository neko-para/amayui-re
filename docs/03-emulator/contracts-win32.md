# L1 · 边界合同：我们对外承诺的 Win32 / D3D9 / DDraw / DirectShow 行为

> 这份是"**换一份实现也不许变**"的那部分，也是将来重写平台层时的验收面。
> 每条都落到一个 **API** 上，并且**只写调用方看得见的结果**（返回值 / LastError / 内存内容 / 边界行为）。
> 实现内部怎么选（堆策略、缓存键、批合并数、计时分项）**不在这里**，那是 `design-notes.md`（L3）；
> "我们测到过什么"是 `observations.md`；"曾经错在哪"是 `defects.md`。
>
> 依据栏三种写法：
> **`文档 · <API>（✅ URL）`** = 本仓已存档页面（`external-docs.md` 有可点链接）·
> **`文档 · <API>（URL 待补）`** = 代码注释点名过该文档、我们照它实现，但**本仓还没存下页面地址**（不许凭记忆补）·
> **`我们自定`** = 文档没定义，或我们**故意偏离**（偏在哪 / 为什么见 `external-docs.md` 末节）。
> `实现` 栏是**读过的**代码落点；`守卫` 栏是用例名（`用例文件 / 子用例` = 该文件里的 `g_current_case`；
> CTest 注册的是**整个用例文件**（一个文件可能注册成多个套件），子用例名本身不是 CTest 名），没有就写 `无`。

## MEM —— 堆 / 虚拟内存

| id | 合同 | 依据 | 实现 | 守卫 |
|---|---|---|---|---|
| L1-MEM-01 | 分配失败（堆耗尽 / 请求过大）时 `HeapAlloc` / `LocalAlloc` / `GlobalAlloc` / `CoTaskMemAlloc` 及三个 `*ReAlloc` 返回 0，且 `LastError=8`（`ERROR_NOT_ENOUGH_MEMORY`）—— ⛔ 不崩、不返回垃圾指针 | 文档 · `HeapAlloc`/`LocalAlloc` 的失败返回（URL 待补） | `shims/kernel32_mem.cpp:89-95`（统一出口 `outOfMemory`） | `shims_core.cpp / heap-realloc-keeps-content` · `heap_layout.cpp / heap-request-overflow` |
| L1-MEM-02 | `HEAP_ZERO_MEMORY` / `LMEM_ZEROINIT` / `GMEM_ZEROINIT` ⇒ 返回块载荷全 0；`VirtualAlloc(MEM_COMMIT)` 新提交页读 0，但对**已提交**页再 COMMIT **内容不变**（不许清零） | 文档 · `VirtualAlloc` Remarks：已提交页的内容不被重置（代码注释 `kernel32_mem.cpp:384` 引） | `runtime/heap.cpp:120` · `shims/kernel32_mem.cpp:386-396` | `shims_core.cpp / heap-alloc-zero-and-size` · `heap_layout.cpp / batch-commit-unwritten-zero` |
| L1-MEM-03 | `*ReAlloc` 扩大时旧载荷的前 `req` 字节被搬到新块（内容保留）；失败返回 0 且**原块的地址/大小/内容都不动** | 文档 · `HeapReAlloc`（URL 待补） | `runtime/heap.cpp:182-193` · `shims/kernel32_mem.cpp:171-179` | `shims_core.cpp / heap-realloc-keeps-content` |
| L1-MEM-04 | `VirtualAlloc(hint,…,MEM_COMMIT)` 对**未先 RESERVE** 的地址 ⇒ 0 + `LastError=487`；`dwSize==0` 或 type 既非 COMMIT 也非 RESERVE ⇒ 0 + 87 | 文档 · `VirtualAlloc`（代码注释 `kernel32_mem.cpp:450-451` 明写 487） | `shims/kernel32_mem.cpp:405-417`、`449-459` | `shims_core.cpp / virtual-commit-hint-needs-reserve` |
| L1-MEM-05 | `VirtualFree(MEM_RELEASE)` 要求 `dwSize==0` 且 `lpAddress` 是某个活预留区的**基址**，可任意顺序释放；重复释放/中间地址 ⇒ 0 + 487，**别的区不受影响**；`MEM_DECOMMIT` 的 `dwSize==0` ⇒ 退到该区域末尾 | 文档 · `VirtualFree`（代码注释 `kernel32_mem.cpp:567` 引） | `shims/kernel32_mem.cpp:515-547`、`550-578` | `shims_core.cpp / virtual-release-any-order`、`virtual-decommit-zero-size` |
| L1-MEM-06 | 尺寸查询的失败码分家：`HeapSize` 对 NULL/已释放/非本堆 ⇒ `0xFFFFFFFF` + 87，`LocalSize`/`GlobalSize` ⇒ 0 + 87；而 `*Free` 拿到野指针/double free 是**打印 + abort**（不返回错误码） | 文档 · `HeapSize`/`LocalSize`（URL 待补）＋ `我们自定`（free 的响亮失败） | `shims/kernel32_mem.cpp:181-191`、`243-253`、`306-316`、`98-104` | `shims_core.cpp / heap-free-invalidates-size`、`local-and-global` |

## FILE —— 文件 / 路径 / 句柄

| id | 合同 | 依据 | 实现 | 守卫 |
|---|---|---|---|---|
| L1-FILE-01 | guest 路径先归一到**唯一根**：`..` 归一后仍须在根下（否则失败，⛔ 不夹回）；所有盘符/绝对路径落到同一根；逐段 ASCII 大小写不敏感且要求**唯一**匹配，多候选即失败 | `我们自定`（Windows 上多候选不会发生；枚举按 UTF-8 字节序也是我们定的） | `runtime/vfs.cpp:236-243`、`733-736`、`320`、`345-355` | `file_io.cpp / escape-root-rejected`、`case-insensitive-read` · `file_path_ext.cpp / guest-root-absolute` |
| L1-FILE-02 | `CreateFileA` 的 `dwDesiredAccess=0` **什么都不授予**：`ReadFile`/`WriteFile`/`SetFilePointer(Ex)` 全失败 + 5；截断已有文件需写位，否则 `INVALID_HANDLE_VALUE` + 5 且宿主内容不变 | 文档 · `CreateFileA`：`0` = neither read nor write（代码注释 `kernel32_file.cpp:559-560`） | `shims/kernel32_file.cpp:766-771`、`821-828`、`671-677` | `file_io.cpp / access-zero-open`、`truncate-needs-write` |
| L1-FILE-03 | `CREATE_ALWAYS` / `OPEN_ALWAYS` 命中已存在文件时仍**成功**，但置 `LastError=183`；其余 disposition 不置，成功路径**不碰** LastError | 文档 · `CreateFileA` disposition 表（代码注释 `kernel32_file.cpp:726`） | `shims/kernel32_file.cpp:660-664`、`726-728` | `file_io.cpp / create-always-last-error` |
| L1-FILE-04 | 共享冲突按**双向**判（新 access × 旧 share、旧 access × 新 share）⇒ `INVALID_HANDLE_VALUE` + 32（`ERROR_SHARING_VIOLATION`） | 文档 · `CreateFileA` 的 `dwShareMode`（URL 待补） | `shims/kernel32_file.cpp:499-518` | `file_io.cpp / share-mode-conflict` |
| L1-HANDLE-01 | 句柄值 = `kFirst + kStep × 槽号`（单调、槽位只增不减）；`CloseHandle`/`FindClose` 后同一值**立刻取不到**；文件/查找族拿到已失效值 ⇒ 打印诊断 + abort，kind 不符 ⇒ 0 + `LastError=6` | 文档 · 失效句柄 ⇒ `ERROR_INVALID_HANDLE`（文档未定义"我们硬停"）＋ `我们自定` | `runtime/handles.cpp:40`、`110-118`、`138` · `shims/kernel32_file.cpp:424-432` | `无` |

## CP —— 码页 / 字符串（CP932 是这台机器的默认码页）

| id | 合同 | 依据 | 实现 | 守卫 |
|---|---|---|---|---|
| L1-CP-01 | `MultiByteToWideChar`(CP932) 遇**非法/截断序列**：默认替换成 `U+FFFD` 并**只前进 1 字节**（后续字节照常解）；带 `MB_ERR_INVALID_CHARS` ⇒ 0 + `LastError=1113`。`cbMultiByte==0` 或源指针空 ⇒ 0 + 87（不是"转换出 0 个字符"） | 文档 · `MultiByteToWideChar`（URL 待补；`cp932.cpp:93` 注释明写"Windows 遇非法序列也会推进"） | `runtime/cp932.cpp:47-116,93` · `shims/kernel32_bootstrap.cpp:250-278` | `codepage.cpp / mb-to-wide-invalid` · `strfmt.cpp / cp932-table` |
| L1-CP-02 | `MultiByteToWideChar` 的**返回值与缓冲大小都是字符数**（不是字节数）：`dst==NULL`/`dstLen==0` ⇒ 返回所需字符数；缓冲不够 ⇒ 0 + `LastError=122`（`ERROR_INSUFFICIENT_BUFFER`） | 文档 · `MultiByteToWideChar`（同上） | `shims/kernel32_bootstrap.cpp:291-304` | `codepage.cpp / mb-to-wide-932` |
| L1-CP-03 | `wsprintfA` 族按**字节**计数：返回值 = 写入的字节数（不含结尾 NUL），`%s` 的 CP932 字节原样透传 | 文档 · `wsprintf`（URL 待补） | `shims/kernel32_string.cpp:300-316` | `strfmt.cpp / wsprintf-basic`、`wsprintf-cp932-bytes` |
| L1-CP-04 | `GetStringTypeW(CT_CTYPE1)` 按**文档化区间规则**给位（ASCII 仍走 `ctype1Ascii()`，与 MSDN 定点逐位一致：'A'=0x0181、'a'=0x0182、'G'=0x0101、'0'=0x0084、' '=0x0048、'!'=0x0010）：U+3000 ⇒ `C1_SPACE|C1_BLANK`；U+3001–303F ⇒ `C1_PUNCT`；U+3040–309F / U+30A0–30FF / U+4E00–9FFF ⇒ `C1_ALPHA`（Lo 类字母；不区分 UPPER/LOWER）；U+FF01–FF5E ⇒ 映回 ASCII 后套 ASCII 的位；U+FF61–FF65 ⇒ `C1_PUNCT`（Unicode Po），U+FF66–FF9F ⇒ `C1_ALPHA`。区间外码位 ⇒ 该字符 0 位 + 一次日志（⛔ 不拿属性现算冒充 Windows 分类） | 文档 · `GetStringTypeW`（URL 待补；MSDN 明说「Japanese hiragana and katakana characters, and the kanji ideograph characters all have the C1_ALPHA attribute」）＋ `我们自定`（区间边界与全角回映口径见 `shims/kernel32_bootstrap.cpp` 的 `ctype1NonAscii` 注释） | `shims/kernel32_bootstrap.cpp:430-527` | `locale_ext.cpp / get-string-type-w-ctype1` |
| L1-CP-05 | `GetStringTypeW(CT_CTYPE2 / CT_CTYPE3)`（双向类 / 语言学类别）**明确不做**：⇒ 0 + `LastError=87` + 日志，且**不写** `lpCharType`（⛔ 不拿 CT_CTYPE1 的位冒充）。重开条件 = 出现消费者 | 文档 · `GetStringTypeW` 的 Ctype2 / Ctype3 表（URL 待补）＋ `我们自定`（Node 自带 ICU 不暴露 Bidi_Class / C3 所需数据，给不出可复现来源） | `shims/kernel32_bootstrap.cpp:470-484` | `locale_ext.cpp / get-string-type-w-info-type` |
| L1-CP-06 | `LCMapStringW` 的 `LCMAP_HIRAGANA` / `LCMAP_KATAKANA` / `LCMAP_HALFWIDTH` / `LCMAP_FULLWIDTH`：假名按 **±0x60 区间**（U+3041–3096 ⇄ U+30A1–30F6；ゝゞゟ U+309D–309F ⇄ ヽヾヿ U+30FD–30FF），ー 与组合/独立浊音点（共享记号）不动、ヷヸヹヺ 无平假名形式不动；宽度按纯算术（0x20 ⇄ U+3000、0x21–7E ⇄ U+FF01–FF5E，±0xFEE0）+ 半角片假名 U+FF61–FF9F ⇄ 全角的 **63 项 Unicode 标准 1:1 映射**。`HIRAGANA|FULLWIDTH` 才把半角片假名映到平假名（文档）；`HIRAGANA|KATAKANA`、`HALFWIDTH|FULLWIDTH` 互斥 ⇒ 0 + `LastError=1008`。**不做 NFC 合成**（文档只说 "may"；「は + U+3099」不合成「ば」），重开条件 = 出现消费者 | 文档 · `LCMapStringEx`（URL 待补；FULLWIDTH/HALFWIDTH 与 HIRAGANA/KATAKANA 互斥、HIRAGANA 需 FULLWIDTH 才能处理半角片假名）＋ `我们自定`（假名区间 / 63 项表的口径见 `shims/kernel32_bootstrap.cpp` 的 `kKanaWidth` 注释；不做 NFC） | `shims/kernel32_bootstrap.cpp:571-780` | `locale_ext.cpp / lcmap-string-kana-width` |
| L1-CP-07 | `LCMapStringW` 的 `LCMAP_BYTEREV` 逐 **16 位码元**反转字节（MSDN 例：0x3450 0x4822 ⇒ 0x5034 0x2248），**不是**反转字符顺序；可与映射 flag 组合（先映射后反转）；结尾 NUL 仍映射到自身 | 文档 · `LCMapStringEx` 的 LCMAP_BYTEREV 例子（URL 待补） | `shims/kernel32_bootstrap.cpp:762-776` | `locale_ext.cpp / lcmap-string-kana-width` |
| L1-CP-08 | `LCMapStringW` 的 `LCMAP_SORTKEY` **明确不做**：⇒ 0 + `LastError=1008` + 日志（⛔ 不返回伪排序键）。重开条件 = 出现消费者 | 文档 · `LCMapStringEx`（URL 待补）＋ `我们自定`（NLS 排序权重表没有可复现来源） | `shims/kernel32_bootstrap.cpp:715-727` | `locale_ext.cpp / lcmap-string-invalid-flags` |

## GDI —— 字体 / 文本度量 / GDI 对象

| id | 合同 | 依据 | 实现 | 守卫 |
|---|---|---|---|---|
| L1-GDI-01 | 同一 HDC + 同一选中字体，连续两次 `GetTextMetricsA` 写出的 `TEXTMETRIC` **逐字节相同**；恒有 `tmHeight = tmAscent + tmDescent`（`tmDigitizedAspectX/Y` 固定 96） | 文档 · `GetTextMetrics`（URL 待补） | `runtime/font.cpp:407-412` · `shims/gdi32_font.cpp:56-81` | `gdi_font.cpp / metrics-stable`（assets 档，真字体） |
| L1-GDI-02 | `GetGlyphOutlineA`：`cbBuffer==0` 返回**所需字节数**（行宽 DWORD 对齐：`BITMAP=((w+31)>>5)<<2`、`GRAY4/8=(w+3)&~3`；值域上界 16 / 64），带缓冲时返回同一数值；`fuFormat` 只认 `GGO_BITMAP/GRAY4/GRAY8/METRICS`，其它或非单位 `MAT2` ⇒ `GDI_ERROR` | 文档 · `GetGlyphOutlineA`（✅ 有 URL，见 `external-docs.md` MS-getglyphoutline） | `shims/gdi32_font.cpp:198-215`、`240-263` · `runtime/font.cpp:628-670` | `gdi.cpp / pack-mono-row-align`、`pack-gray4-range`、`pack-gray8-range` · `gdi_font.cpp / ggo-formats` |
| L1-GDI-03 | `GetTextExtentPoint32A` 的宽 = 逐字 `gmCellIncX` 之和 = `TextOutA` 的推进量（同一个字段）；高 = `tmHeight` | 文档 · `GetTextExtentPoint32`（URL 待补） | `runtime/font.cpp:610-623`、`675-737` | `gdi_font.cpp / extent-advance` · `gdi.cpp / extent-advance-self-consistency` |
| L1-GDI-04 | 字体对象只携带 `LOGFONT`：`lfHeight` 取绝对值当像素字号、`weight≥700` 当粗体、`LOGFONTA` 不可读 ⇒ 0；`SelectObject` 返回旧对象（首次 = stock `DEFAULT_GUI_FONT`）；`DeleteObject` 对仍被选中 / stock / DC / 已删对象一律 `FALSE`，已删对象再选入 ⇒ 0（**不复活**） | 文档 · `CreateFontIndirect` / `DeleteObject`（代码注释 `gdi32_object.cpp:139`） | `shims/gdi32_font.cpp:101-141` · `runtime/font.cpp:516-552` · `shims/gdi32_object.cpp:54-176` | `gdi.cpp / objects`、`delete-object-dc` |

## MSG —— 消息 / 输入（user32）

| id | 合同 | 依据 | 实现 | 守卫 |
|---|---|---|---|---|
| L1-MSG-01 | `PostMessageA` 入队的消息被 `PeekMessageA(PM_REMOVE)` 取走后**不再重复投递**；`PM_NOREMOVE` 只窥视、不取走 | 文档 · `PeekMessage`（URL 待补） | `shims/user32_window.cpp:545-574`、`620-632` | `user32_window.cpp / post-peek-dispatch` |
| L1-MSG-02 | `PostQuitMessage` ⇒ 队列里出现 `WM_QUIT`，`GetMessageA` 取到它返回 0；`WM_CLOSE` 由 `DefWindowProcA` 回 0，**不自己销毁窗口** | `我们自定`（真机 `DefWindowProc(WM_CLOSE)` 会 `DestroyWindow`；本线销毁权在宿主） | `shims/user32_window.cpp:576-588`、`656-661`、`90` | `无`（`msg-wait` 只间接守 WM_QUIT 在队列里） |
| L1-MSG-03 | 输入是**单一影子状态**的读点：`GetAsyncKeyState` 返回 `0x8000`（当前按下）或 0（低字节"自上次查询后按过"**未建模**）；`GetCursorPos` 写当前虚拟光标（headless 初值 (0,0)），`lpPoint` 不可写 ⇒ `FALSE` + 87 | 文档 · `GetAsyncKeyState` 最高位语义（代码注释 `user32_window.cpp:480`）＋ `我们自定`（低字节） | `shims/user32_window.cpp:478-492` · `shims/user32_input.cpp:84-119` | `input_injection.cpp / script-priority` · `user32_window.cpp / cursor-pos` |
| L1-MSG-04 | `MsgWaitForMultipleObjects`：有匹配 `dwWakeMask` 的消息 ⇒ `WAIT_OBJECT_0+nCount` 且**不取走**消息；无消息且 0 ms ⇒ `WAIT_TIMEOUT`；有限超时先推进虚拟时钟再超时；`INFINITE` 或需等对象 ⇒ 响亮失败 | `我们自定`（"要等对象就硬错"是偏差） | `shims/user32_input.cpp:279-343` | `user32_window.cpp / msg-wait` |

## SYNC —— 同步原语

| id | 合同 | 依据 | 实现 | 守卫 |
|---|---|---|---|---|
| L1-SYNC-01 | 命名对象：`Create*` 命中同名 ⇒ 同一对象的**新句柄** + `LastError=183`；`Open*` 找不到（含只差大小写）⇒ `NULL` + `LastError=2`；名字比较**大小写敏感**，`Local\`/`Global\` 前缀剥掉 | 文档 · `OpenMutexA`/`OpenEventA`/`OpenSemaphoreA` 原话（代码注释 `kernel32_sync.cpp:11-13`、`407-409`） | `shims/kernel32_sync.cpp:256-272`、`410-420`、`432-454` | `sync_ext.cpp / named-event-open`、`named-semaphore-mutex` |
| L1-SYNC-02 | `WaitForSingleObject`：已 signaled ⇒ `WAIT_OBJECT_0` 并**消费一个信号**（自动复位事件复位 / 信号量 −1 / 互斥量归本线程）；未 signaled 且 `dwMilliseconds==0` ⇒ `WAIT_TIMEOUT`；未 signaled 且非 0 ⇒ **进程级响亮失败**（⛔ 不假装超时）；`WAIT_ABANDONED` 永不出现 | `我们自定`（无人释放的互斥量不判 abandoned；阻塞等待不在本线） | `shims/kernel32_sync.cpp:686-714`、`655-672` | `shims_core.cpp / event-set-reset-wait`、`mutex-release-owner`（`unsignaled-wait-traps` 守的是**我们的**硬错） |
| L1-SYNC-03 | `CRITICAL_SECTION`：权威账本在 **guest 结构**（`Initialize*` 清 0x18 字节、spin count 写 `+0x14`）；同线程 `Enter` 可重入（递归计数 +1），`Leave` 递减到 0 清 owning thread；未配对 / 非拥有者 `Leave` ⇒ 响亮失败 | 文档 · critical-section-objects（✅ 有 URL，可重入 / 必须等次数 Leave） | `shims/kernel32_sync.cpp:285-391` · `runtime/critical_sections.h:19-36` | `shims_core.cpp / critical-section-reentrant` |

## D3D9 —— 设备 / 纹理 / 状态

| id | 合同 | 依据 | 实现 | 守卫 |
|---|---|---|---|---|
| L1-D3D9-01 | `LockRect` 成功时回填 `D3DLOCKED_RECT{Pitch = 宽×bpp, pBits = guest 像素}`（`top/left` 已计入 `pBits`）；同一 level 二次 Lock、RECT 越界/空 ⇒ `D3DERR_INVALIDCALL`；`UnlockRect` 后写入的 texel 对后续采样**可见**，未锁时 Unlock 也 ⇒ `D3DERR_INVALIDCALL` | 文档 · `IDirect3DSurface9::LockRect`（URL 待补） | `runtime/d3d9.cpp:1071-1078`、`1028-1030`、`1063-1069`、`1103-1108` | `d3d9_pipeline.cpp / d3d9p/textured-quad` · `d3dx9.cpp / d3dx9/texture+draw` |
| L1-D3D9-02 | `Present` 把当前后缓冲**整帧**送出显示目标，**不交换**前后台、不改后缓冲像素；`frameCount` 与设备的 `Presents` 各 +1，同输入两次跑同 hash；`pSourceRect/pDestRect/pDirtyRegion` 被忽略（整帧）；呈现失败 ⇒ 返回失败码（⛔ 不静默成功） | 文档 · `IDirect3DDevice9::Present`（URL 待补）＋ `我们自定`（无真 swap chain） | `runtime/present.cpp:47-54` · `runtime/d3d9.cpp:723`、`688-691` | `d3d9.cpp / d3d9/present` |
| L1-D3D9-03 | `Clear` 按掩码：无 `D3DCLEAR_TARGET` ⇒ 颜色不动且返回 `S_OK`；`Count==0` 清整幅、否则只清给定 `D3DRECT`（空矩形跳过）；`ZBUFFER` 位真清深度；`STENCIL` 位**没有模板缓冲** ⇒ 只记录，⛔ 不谎称清了 | 文档 · `IDirect3DDevice9::Clear`（URL 待补） | `runtime/d3d9.cpp:525-532`、`547-552`、`591-626` | `d3d9.cpp / d3d9/clear` |
| L1-D3D9-04 | 资源耗尽（纹理像素 / 状态块分配失败）⇒ 返回 `E_OUTOFMEMORY(0x8007000E)`，**不算"未实现"**、不计 `unimplemented_calls`；随后本线**硬停**（真机会继续跑） | 文档 · D3DX/D3D9 的资源失败码（URL 待补）＋ `我们自定`（硬停，理由见 `external-docs.md` 偏离 §1） | `shims/d3dx9.cpp:1076-1083`、`104-127` · `runtime/d3d9.cpp:848-849` | `无` |
| L1-D3D9-05 | 对象 `Release` 到 0 ⇒ **立即归还** guest 侧资源：设备带走后缓冲 + 深度两块 arena，纹理带走像素 arena 与状态块（`surfaceCount` / `bytesInUse` 回基线） | 文档 · COM `IUnknown::Release` 的所有权语义（URL 待补） | `runtime/d3d9.cpp:990-997` · `shims/ddraw.cpp:184-201` | `d3d9.cpp / d3d9/texture-bound-then-guest-release` |
| L1-D3D9-06 | `CreateStateBlock` 在**建块那一刻**捕获设备状态（不是等到 `Capture`）；`Capture` 用当前状态重捕；`Apply` 把快照写回，**含绑定重建**（纹理 / 流 / 索引 / 顶点声明走正常绑定路径，⛔ 不裸写槽）；`D3DSBT_PIXELSTATE` / `VERTEXSTATE` 只回滚自己那一侧（像素块不碰 FVF / 变换 / 视口 / 裁剪矩形，反之亦然）；快照在块活着期间替 guest 持一份绑定引用（guest 放掉自己那份后 `Apply` 仍能写回），块 `Release` 到 0 时成对还回 | 文档 · `IDirect3DStateBlock9::CreateStateBlock` / `Capture` / `Apply`（URL 待补；`shims/d3d9.cpp:991` 注释引"a state block contains state data … when the object is created"） | `shims/d3d9.cpp:1057-1104`、`1143-1211` · `runtime/d3d9_pipeline.cpp:3669-3822` | `d3d9.cpp / d3d9/stateblock-capture-apply` |
| L1-D3D9-07 | 缩小采样（每屏幕像素覆盖 > 1 texel）且 `MIPFILTER != NONE` 时，采样取对应的派生 mip 级（**最近级**，⛔ 不做三线性：1.5:1 ⇒ level 1、1.2:1 仍 level 0）；**1:1 与放大恒 level 0**；`MIPFILTER=NONE` ⇒ 只采 level 0（关掉 mipmap）；`Unlock` 改了 texel ⇒ 下一次绘制用按新内容重算的链（⛔ 不用旧链），纹理 `Release` 后链随之回收 | 文档 · D3D9 采样器的 MIPFILTER / `D3DTEXF_NONE` = 关掉 mipmap（URL 待补）＋ `我们自定`（最近级近似；多级链是采样侧内部派生的） | `runtime/d3d9.cpp:1359-1447` · `runtime/d3d9_pipeline.cpp:1299-1322`、`5149-5169` | `d3d9_mip.cpp / d3d9-mip/lod-levels` · `d3d9_pipeline.cpp / d3d9p/mip-levels` |
| L1-D3D9-08 | `DrawPrimitive(UP)` 的 `LINELIST` / `LINESTRIP` 真的写像素：线宽恒 1px（主轴每列一个像素，起点 / 终点列都画、相邻列不画）；顶点顺序（右→左 / 下→上）不改变覆盖，颜色与 z 沿段按位置插值；线的深度测试 / 深度写 / alpha blend 与三角形**同一份**。点 / 线与三角形共用同一条状态准入：只接受"结果恒等于顶点色"的无纹理路径 —— 任一纹理级绑了纹理、或 `COLOROP` / `ALPHAOP` 需要采样 ⇒ `D3DERR_INVALIDCALL` 且**一个像素都不写**（⛔ 不画成纯色） | 文档 · `IDirect3DDevice9::DrawPrimitive` / `DrawPrimitiveUP` 的线图元规则（URL 待补）＋ `我们自定`（点尺寸 / 抗锯齿线未建模；带纹理的点 / 线不接受） | `runtime/d3d9_pipeline.cpp:1941-1989`、`4106-4141`、`4459-4510` | `d3d9.cpp / d3d9/linelist-writes-pixels`、`d3d9/linestrip-16`、`d3d9/line-axes-and-direction`、`d3d9/line-textured-loud-reject` |
| L1-D3D9-09 | `CheckDeviceFormat` 只对**真的建得出来**的组合答 `S_OK`（支持集与纹理创建侧同一份，⛔ 不另写表）：`TEXTURE` 的 `X8R8G8B8` / `A8R8G8B8`（`Usage=0` 或 `RENDERTARGET`）答支持、`RENDERTARGET` 只接受 32bpp；`DXT1`、`D24S8` 当纹理、16bpp 当 `RENDERTARGET`、**未知格式**一律 `D3DERR_NOTAVAILABLE`；`SURFACE` 只接受 `DEPTHSTENCIL` + `D24S8`（`D16` / 纹理格式不支持）；其它 `RType` / 联合 `Usage` 位不支持；答 no 也留诊断 | 文档 · `IDirect3D9::CheckDeviceFormat`（URL 待补；判定规则见 `runtime/d3d9.cpp:543-555` 注释） | `runtime/d3d9.cpp:556-573` · `shims/d3d9.cpp:278-298` | `d3d9.cpp / d3d9/checkdeviceformat-truthful` |
| L1-D3D9-10 | 挂了 GPU 光栅后端时，`Clear` 的 `D3DCLEAR_ZBUFFER` 位必须把"清深度"请求**穿过呈现壳**送到后端（一次 `Clear` 一次请求）；只带 `D3DCLEAR_TARGET` 的 `Clear` **不发**清深度请求。⛔ 不许出现"CPU 深度镜像清了、GPU 的 D32 没清"的静默分叉（下一帧的绘制会因此被上一帧的深度挡住） | 文档 · `IDirect3DDevice9::Clear` 的 `D3DCLEAR_ZBUFFER`（与 `L1-D3D9-03` 同一份语义；URL 待补）＋ `我们自定`（GPU 的 D32 与 guest 深度缓冲是两块内存，见 `external-docs.md` §3 #11） | `runtime/d3d9.cpp:603-609` · `runtime/present_sdl.h:91-97`、`runtime/present_sdl.cpp:786-789` | `gpu_batch.cpp / gpu-batch/depth-clear-forwarded` |

## DDRAW —— 表面 / Blt / Flip

| id | 合同 | 依据 | 实现 | 守卫 |
|---|---|---|---|---|
| L1-DDRAW-01 | `Blt(DDBLT_KEYSRC)`：源表面 SRCBLT 键色范围内的像素**不写入**目标；不带该位时连键色一起拷（键色只影响本次调用，不改表面状态） | 文档 · `IDirectDrawSurface7::Blt`（URL 待补） | `runtime/ddraw_surface.cpp:506`、`521`、`539` | `ddraw.cpp / ddraw/blt-colorkey` |
| L1-DDRAW-02 | `BltFast` 把源矩形按源表面裁剪、目标逐像素判界写入；源矩形完全在外或目标全在界外 ⇒ `DD_OK` 且**一个字节都不写**（不越界写邻块） | 文档 · `IDirectDrawSurface7::BltFast`（URL 待补） | `runtime/ddraw_surface.cpp:562-586` | `ddraw.cpp / ddraw/bltfast-clip`（邻块 canary 逐字节未变） |
| L1-DDRAW-03 | `Lock` 回填 `lPitch = 宽×bpp`、`lpSurface` 指向 guest 像素；**嵌套 Lock 返回同一指针**，Unlock 逐层匹配该层指针；多余 Unlock ⇒ `DDERR_NOTLOCKED`；有未释放 DC ⇒ `DDERR_SURFACEBUSY`；越界矩形 ⇒ `DDERR_INVALIDRECT` | 文档 · `IDirectDrawSurface7::Lock`（URL 待补） | `runtime/ddraw_surface.cpp:617-665` | `ddraw.cpp / ddraw/lock-unlock`、`getdc-release` |
| L1-DDRAW-04 | `Flip` 只翻 primary：非 primary ⇒ `DDERR_NOTFLIPPABLE`；指定 back buffer ⇒ 失败（未实现）；成功时呈现 primary 且 `frameCount+1`，**不交换**前后台 | 文档 · `IDirectDrawSurface7::Flip`（URL 待补）＋ `我们自定`（无 back buffer） | `runtime/ddraw_surface.cpp:757-793` · `shims/ddraw.cpp:646-649` | `ddraw.cpp / ddraw/flip` |

## DSHOW —— DirectShow 图

| id | 合同 | 依据 | 实现 | 守卫 |
|---|---|---|---|---|
| L1-DSHOW-01 | **形状层**（Movie / `AMultiMediaStream`）：`RenderFile`/`Run`/`Pause`/`Stop` 返回 `S_OK`，`WaitForCompletion` **立即** `EC_COMPLETE`，时长/位置恒 0（引擎据此认为"已放完"） | `我们自定`（口径见 `apps/emulator-lift/docs/movie-dshow-protocol.md`） | `shims/quartz_video.cpp:13-15`、`921`、`939-943` | `quartz_video.cpp / quartz-qi-mediaevent-wait`、`quartz-mediacontrol-run-stop` |
| L1-DSHOW-02 | **真图**：`Render` 先接引擎自己的源 pin（QI → `Length` → `SyncRead`）再接渲染器（`EnumPins` → `ReceiveConnection` → QI(`IMemInputPin`) → `NotifyAllocator`）；**接线没成功 ⇒ 泵一帧都不送**（Warn，无帧）。`IMediaControl::Run` = Pause(渲染器) + Pause(源) + Run(源)，计时从链尾起算，渲染器保持 Paused | 文档 · `IGraphBuilder::Render` / `IMediaControl::Run`（URL 待补） | `runtime/dshow/graph.cpp:1376-1388`、`1216-1241` · `runtime/dshow/dshow.h:276-287` | `dshow_graph.cpp / dshow-render-connect`、`dshow-broken-wiring`、`dshow-control-order` |
| L1-DSHOW-03 | `IMemInputPin::Receive` 逐帧交付：泵按虚拟时钟推进，pts 单调、**同一拍重复调用不重复送**；分配器忙时**不阻塞**、丢这一帧（`dropped_busy` 增长），缓冲交回后继续 | 文档 · `IMemInputPin::Receive`（URL 待补） | `runtime/dshow/graph.cpp:1390-1395`、`1440-1465` | `dshow_graph.cpp / dshow-pump-frames`、`dshow-allocator-busy` |

## AUDIO —— DirectSound buffer

| id | 合同 | 依据 | 实现 | 守卫 |
|---|---|---|---|---|
| L1-AUDIO-01 | `IDirectSoundBuffer::Lock` 给出**真 guest 内存**窗口（跨回绕给两段指针）；`offset ≥ 缓冲` 或 `dwBytes > 缓冲` ⇒ `DSERR_INVALIDPARAM`；`DSBLOCK_ENTIREBUFFER` 只接受 `offset=0`；`Unlock` 的指针/长度与 Lock 记的不一致 ⇒ `DSERR_INVALIDPARAM`，无未完成 Lock ⇒ `DSERR_INVALIDCALL`；`Play` 置 PLAYING(+LOOPING) 并返回 `DS_OK`；`GetCurrentPosition` 两个出参同值 | 文档 · `IDirectSoundBuffer::Lock`（取自 SDK `dsound.h`，代码注释 `dsound.cpp:50`；URL 待补）＋ `我们自定`（silent 档游标口径） | `shims/dsound.cpp:876-895`、`1081-1204`、`1253-1263` | `dsound.cpp / dsound-edge-lock-write-unlock-play-accounting` |

## PE —— 资源

| id | 合同 | 依据 | 实现 | 守卫 |
|---|---|---|---|---|
| L1-PE-01 | 资源查找失败一律 `false` + 具体原因（"没有这一项" / 类型不存在 / 目录声明损坏**都是失败**，⛔ 不当成"没有资源"）；资源名比较**大小写不敏感**；`RT_GROUP_ICON` 返回组声明的 `RT_ICON` ID 列表，**不保证每个 ID 都存在**（调用方自查） | 文档 · `FindResource`（代码注释 `pe_resource.cpp:54`） | `runtime/pe_resource.cpp:54-64`、`87-91`、`150-152`、`362-384` | `pe_resource.cpp / pure-find-by-id`、`pure-find-by-name`、`pure-icon-group` |

## 覆盖范围（这份账**没有**写到的部分）

- **x86 语义**（指令 / 标志位 / x87）不是 Win32 合同，归 CTest `conformance` 与 spec 的 ISA 章。
- **内核对象其余族**（registry / version / imagehlp / shell32 / 定时器）：只有形态层，没到"对外承诺"的密度 ⇒ 未取证。
- **`LoadImage` / 图标尺寸**：全仓没有 `LoadImage`（只有伪 `LoadIconA`/`LoadCursorA`）⇒ 给不出合同。
- **`FindResourceA`** 仍是 `addUnimplemented`（`shims/kernel32_bootstrap.cpp:864`）⇒ Win32 层的"资源查找失败码"没有可观察面。
- **`D3DERR_OUTOFVIDEOMEMORY`** 全仓未出现；`CreateDevice` 的资源失败走 `kFail(0x80004005)`（`shims/d3d9.cpp:345-348`）—— 与文档不符，**待定**，未写成合同。
