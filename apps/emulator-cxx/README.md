# emulator-cxx

《天結いキャッスルマイスター》原生 C++ 宿主（clean-room）。

目标不是保留 Electron，而是由应用自己拥有窗口与 surface：

- SDL3：窗口 / 输入 / 音频；
- SDL_GPU（当前 POC）：显式 `submit` + `present`，无浏览器合成器；
- bgfx（可选 feature）：后续 2D sprite/mesh/混合渲染；
- FreeType（可选）：文字；Shift-JIS 用平台代码页转换（Windows `MultiByteToWideChar(932)` / 其它平台 iconv）；
- 影片：`third_party/pl_mpeg`（**vendored 单文件**，MIT，MPEG-1 program stream + 只视频）；
  FFmpeg 仍留作可选 feature，但当前没有用到它。

> 覆盖范围与显式限制（字体 / 全屏 / 已知限制）见 [SCOPE.md](SCOPE.md)。

## 目录

```text
apps/emulator-cxx/
  CMakeLists.txt
  CMakePresets.json
  vcpkg.json
  vcpkg/                    git submodule（vcpkg 本体，不入库）
  third_party/pl_mpeg/      影片解码（vendored 单文件，来源与为什么见该目录 README）
  src/main.cpp              **只有 CLI 解析与分派**
  src/app.h                 宿主内部 API（常量 / 小类型 / 跨 TU 函数；不是 core、不是公共库）
  src/gpu_device.cpp        SDL_GPU 设备 / 读回 / 纹理上传 / 着色器路径判定
  src/gpu_quad.cpp          贴图四边形（冒烟 / 兜底）+ 着色器源（HLSL + MSL）与编译分派
  src/gpu_scene.cpp         场景批次：顶点构建 · 纹理按 revision 缓存 · 离屏导出
  src/smoke.cpp             `--smoke` 的离屏自检（测试，不是产品路径）
  src/boot.cpp              boot 流程（根脚本 → LOGO → 影片 → TITLE）与单 AGF 解码
  src/window.cpp            窗口播放循环（按 core 的渲染请求呈现）
  src/core/                 C++ core（按 spec 重写）：ALF / LZSS / AGF / 脚本头 / opcode 解码 / 影片后端
                            `vm.cpp` 只做分派表 + 主循环；handler 按族分在 `vm_ops_*.cpp`，
                            值/池原语 `vm_value.cpp`、计时窗 `vm_timing.cpp`、影片 `vm_movie.cpp`
```

## 依赖（vcpkg submodule + manifest）

vcpkg 以 **git submodule** 固定在 `apps/emulator-cxx/vcpkg`；依赖由同目录的
`vcpkg.json` 声明（manifest mode）。基础依赖是 `sdl3`、`nlohmann-json`；
Windows 预设再开 `shader` + `text` 两个 feature，macOS 只开 `text`（原因见下一节）。

| feature | 依赖 | 用途 |
|---|---|---|
| `shader` | `sdl3-shadercross` | HLSL → DXIL/SPIR-V（Windows/Linux 默认开启；macOS 不需要，见下节） |
| `text` | `freetype` | 字形光栅化；Shift-JIS 用平台代码页转换（Windows `MultiByteToWideChar(932)`，其它平台系统 iconv）（默认开启） |
| `renderer` | `bgfx` | 2D 渲染后端（可选） |
| `video` | `ffmpeg` | 影片解码（可选、当前未用；实际走 vendored pl_mpeg） |

首次拉取：

```powershell
git submodule update --init --recursive
.\apps\emulator-cxx\vcpkg\bootstrap-vcpkg.bat -disableMetrics
```

## 着色器与平台路径（★ 三个平台都走 GPU）

SDL_GPU 的后端各认各的着色器格式，本仓的搭配是：

| 平台 | 后端 | 着色器格式 | 从哪来 |
|---|---|---|---|
| Windows | D3D12 | DXIL | HLSL →（运行时）`SDL_shadercross` |
| Linux | Vulkan | SPIR-V | 同一份 HLSL →（运行时）`SDL_shadercross` |
| macOS | Metal | MSL | MSL **源码文本**，由 Metal 运行时编译（不用 shadercross） |

为什么 macOS 不需要 `shader` feature：`SDL_GPU_SHADERFORMAT_MSL` 收的是**源码**，
Metal 后端自己 `newLibraryWithSource:` 编译它；而 `sdl3-shadercross` 依赖 `directx-dxc`，
那个 vcpkg port 的 supports 只有 `windows | linux-x64` ⇒ 在 macOS 上装不上，也没必要装。
（这也是 SDL 生态的标准做法：设备支持 MSL 就发 MSL 源码。）

⇒ 每条管线有**两套源**（HLSL + MSL），落在 `src/gpu_quad.cpp` 与 `src/gpu_scene.cpp`，
`compile_shader()` 按 `SDL_GetGPUShaderFormats(device)` 现场挑：
**改一个管线要同时改两份源**（两份描述同一个管线，不同步就会两个平台画出不一样的东西）。
MSL 是**手写**的（不是生成物）：两份源并排放在同一个文件里，就是为了"改一个时另一个就在眼前"。
骨架照 `SDL_shadercross` 的输出写（入口 `main0`、varying `[[user(locnN)]]`、
纹理 `[[texture(0)]]` / 采样器 `[[sampler(0)]]`、顶点属性 `[[attribute(n)]]` 与
vertex attribute location 一一对应）⇒ 将来若改成"用 shadercross CLI 在 Windows 上把 HLSL
生成成 MSL 再提交"的做法，调用点一个字都不用改。

## 构建（Windows）

本机 Visual Studio 18 的 `cl.exe` 在 CMake 探测阶段会报
`c1.dll ... failed to get command line into debug records`，因此默认 preset 使用
`clang-cl`（LLVM），SDL3 由 vcpkg 构建：

```powershell
cd apps\emulator-cxx
cmake --preset windows-x64
cmake --build --preset windows-x64
```

macOS / Linux 用同一份 manifest，preset 已按平台选好 feature（macOS 只有 `text`）：

```bash
cmake --preset macos-arm64
cmake --build --preset macos-arm64
ctest --test-dir out --output-on-failure
```

依赖 feature 打开方式：

```powershell
cmake --preset windows-x64 -DVCPKG_MANIFEST_FEATURES="renderer;text;video"
cmake --preset windows-x64 -DEMULATOR_CXX_WITH_BGFX=ON -DEMULATOR_CXX_WITH_TEXT=ON -DEMULATOR_CXX_WITH_VIDEO=ON
```

## C++ core（按 spec 重写）

第一层已落地，不调用 `apps/emulator-new` 的 TypeScript：

| 模块 | 规范 | 状态 |
|---|---|---|
| ALF 索引 / TOC / 尾部默认配置 | `20-alf`、`FMT-ALF` | S3/S4 族已解析；真实 `SYS4INI.BIN` 21109 条 |
| ALF LZSS | `20-alf` | 4 KiB 环形窗口、写指针 `0xFEE` |
| AGF/ACGF | `30-agf` | 8/24/32bpp、ACIF alpha、颜色键口径 |
| 脚本头 | `CORE-ISA` | `SYS4`/`SYS3`、`450 ` 子头、三组表指针 |
| opcode 静态解码 | `CORE-ISA` | 574 条 opcode 表；无处理器即停止并给偏移 |
| 值 / 池 / 帧（最小） | `CORE-POOL` / `CORE-FRAME` | 全局/局部 int/float、ENC/DEC、40 帧槽、返回栈 |
| 最小 VM | `CORE-VM` | 取指 → 域检查 → 长度槽 → ip；exit/ret/call/jump/mov/整数算术 |
| 主循环呈现 | `CORE-VM-LOOP`（取证：门之前） | 每次迭代最多一次渲染：绘制开关 = 1 且场景需要重绘时请求；`0x20C` 是脚本主动再要一次，不是唯一提交路径 |
| GPU 呈现 | SDL_GPU 场景批次 | 绘制项 / 4 顶点网格直接走 GPU（纹理缓存 + 每帧顶点缓冲）；命令档 / 文字暂回退软件合成 |
| 配置库 | `CORE-CONFIG` | 词表 + ALF 发行默认表 + `SYS4REG.INI` 覆盖 + 脚本键表 |
| 已接处理器族 | `CORE-OPS-*` | no-op、语音、标量、列表、字符串、数组/引用地址、纹理/绘制项/网格、`0x3` call-script |
| 启动链 | `CORE-BOOT` | 真实 `SYSTEM4.BIN` 越过 LOGO 影片，进入 `TITLE.BIN`（id 21092）主循环 |
| 影片 | `CORE-OPS-MOVIE`、`CORE-VM-MOVIE` | `0x20F` 对象表 / 纹理绑定门 / 影片位；门之前的对象表服务按引擎时钟推进 pl_mpeg 解码，新帧写进**同槽纹理**并标脏；收尾 `(选项字 & 3) != 2` 填最后一帧，循环位原地重启 |
| 渲染接入 | `CORE-OPS-TEXTURE-DRAW`、`CORE-TIMING-WINDOW` | AGF 像素进入纹理记录；绘制项 / 网格按有符号 key 软件合成；颜色窗四档派生；`0x33F` tint + 混合模式 |
| 场景命令档 | `CORE-OPS-SCENE-COMMAND-RECORDS`、`CORE-SCENE-DRAW-ORDER` | `0x223/24D/24F/250/251` 写表与校验；命令档 → 离屏画布 36/37 → 四种类合成（Fade / 遮罩 / 12 种抹除图案 / SlideBlur / ZoomBlur）→ 收尾扫描 |
| 命名表 / 状态字 / 模式族 | `CORE-OPS-TABLE-RESET-A1` / `-BIND-A2` / `-DISPATCH-A3`、`CORE-OPS-STATUS-WORD`、`CORE-OPS-MODE-BIT`、`CORE-OPS-STATUS-CLEAR-D9`、`CORE-OPS-TEXT-CELL-REVEAL-1CE` | `0xa1/0xa2/0xa3` 文本键命名表（清空 / 绑定 / 分派）；`0x1cf/0x1bf` 状态字（第 16 位 ⇒ **整字**清 0）；`0x19b/0x19c` 模式位与四个分支；`0x0d9` 清位；`0x1ce` 逐格揭示当前文本的启停（活动位 + 当前格号 + 500 ms 节拍器；非 0 = 开，0 = 只清活动位） |
| 输入派发 | `CORE-INPUT-WORD`、`CORE-INPUT-KEY-DISPATCH`、`CORE-INPUT-TOUCH-POINT` | `0xff` 清字 + 平台采样（无设备 ⇒ 保持 0）+ 按帧上限；`0x100` 派发一个按键回调（无按键走"编号 = 键总数"表项，有按键压**当前位置**重派发）；`0x2fc` 取一个触点（无触点只写 0；桌面鼠标-only 恒无触点）；`0x12e` 命中查找：按 `start+1` 起扫框表，命中写项下标、未命中写 −1（`CORE-INPUT-HIT-TEST-12E`） |
| 键盘 / 鼠标（宿主） | `CORE-INPUT-MOUSE-POS`、`-MOUSE-BUTTON`、`-CALLBACK-REGISTER`、`-MOUSE-CALLBACK-PUMP` | 窗口路径把 SDL 的键盘 / 鼠标归约成输入字、客户区光标、按钮位图；`0x109` 光标（输入停用 ⇒ −100000；客户区外仍写真实坐标）、`0x108` 按钮位图（SDL 左右键归约到引擎位 0/1、输入停用 ⇒ 0）、`0xcc` 登记 + `0xcd` 时间闸派发（`elapsed >= interval`，含等号）。★ **不接摇杆**；物理键 ↔ 输入位号的映射是**宿主策略**（`window.cpp` 的 `kKeyBits` 一张表，⛔ 不是规范结论） |
| 脚本化输入 / 无窗口播放 | （宿主策略，⛔ **不进 spec**） | `input_script.cpp` 把指令式时间线（`at move / down / up / key / wheel`）归约成四个采样口 + 滚轮增量；`play_headless.cpp` 按仿真 tick 推进（与窗口路径同一个门、同一套输入注入），可 `--expect-script` / `--require-script` 收口、可导出 BMP。`--anchor-script` 把时间线的 0 点锚在某个脚本**首次进入**时（否则 boot 期间就把光标搬好，TITLE 的"坐标变了才命中"不成立）。★ 与 `apps/emulator-new` 的 `parseInputScript` 同语法：两条实现线互为对照，**不互为真源** |
| TITLE → Option → 设置界面 | `CORE-INPUT-*`、`CORE-OPS-*`（逐条见 `data/spec`） | 实测链：`0x12e` 命中框表（局部 205 / 基准 5、105）→ `0xa1/0xa2/0xa3` 菜单分派 → `0x3 call-script 52`（CONFIG.BIN）→ `0x3 call-script 20940`（CONFIG1.BIN）。CTest：`emulator-core-title-option`（进 52）· `...-config1`（进 20940）· `emulator-core-config-screen`（在 20940 里跑 30 万步不许停） |
| 设置界面：命中 / 开关 / 分页 | `CORE-INPUT-HIT-TEST-12E`、`CORE-OPS-ARRAY-*`、`CORE-OPS-FORWARD-DISPLAY-MODE`、`-NAMED-KEY`、`CORE-CONFIG-NAMED-WRITE`、`-NAMED-AUTOTIME`、`CORE-OPS-AUDIO-*` | ★ **数组族五条的目标 / 基址走「取址原语」**（`0x61/0x12c/0x64/0x6c/0x2d8`；引用族的落点是"它指向的那一格"，不是引用槽）——这一条曾让右列控件的命中表一个框都建不出来。右列 ON/OFF ⇒ `0x2cf`（显示模式请求：目标 ∉ {0,1} 静默；三条早退都不写键；写 `display:ScreenMode` = 归一键值）。命名键：`0x307`（`system:EffectSkipOnClick`，缺键**不建键**）、`0x1b5`（`message:MessageSpeed` + 参数格同值）、`0x1b8/0x1b9`（`AutoMessageTime0/1` 读 / 写，越界只记日志）。声道：`0x1ba`（1..4 ⇒ `sound:*` 四支语义各不相同）。音频请求：`0xb6`（停一个音效槽，**后端不存在时连槽格都不清**）、`0xb7`（BGM 播放，**无同曲判据**）、`0xb8`（BGM 停止，**无在播判据**）、`0xba`（与 `0xb5` 同一请求、循环位 = 1）、`0xb4`（成功路径写「该槽资源号」）。CTest：`emulator-core-config-hover-hit`（命中 ≥ 0 / −1 两半）· `-config-toggle`（ON/OFF 的配置快照必须不同）· `-config-pages`（六页各：页内命中项数量 + 滚 6 格无停机） |
| 帧 / 本地块 | `CORE-OPS-FRAME-CALL-FRAME`、`-LOCAL-SET-7B`、`-LOCAL-RET-7C`、`CORE-OPS-STATE-199` | `0x08` 切到**已装载**帧（只改父帧号 / ip / 长度槽，不清返回栈）；`0x7b` 两个目标位置（初值 −1）；`0x199` 进块（存档字带第 25/26 位、返回点 = `here + 1`、深度判据存**本帧身份编号**）；`0x7c` 离开（成功后门字带第 26 位 ⇒ `GateStalled` 是规范表现） |
| 标量 / 数值 / 随机 / 时钟 / 取址 / 池拷贝 | `CORE-OPS-FLAG-CELLS`、`-MODE-BIT`、`-STATUS-CELL-READ`、`-LAYER-CELL`、`CORE-OPS-RANDOM-60`、`-CLOCK-D0`、`-PARAM-74-7F`、`-FLOAT-ABS`、`-SCALAR-FLOAT-ARITH`、`-POOL-MEMCPY`、`-POOL-LEA-63` | `0x19a/1b6/1b7` 三个**互不相同**的格（Y 归一成 0/1）、`0x1c7`（位 27 → 0/1）、`0x1cc`（状态格原值）、`0x1ad`（落点格复位值 −1）、`0x60`（计数器 → 随机 → 范围 → 落点；范围 0 先写 0 再抛）、`0xd0`（本迭代时钟）、`0x74/0x7f`（参数格，未写过 ⇒ 响亮失败）、`0x191/2d0/2d1/2d3`（浮点族）、`0x1b0`（池拷贝，按编码位模式）、`0x63`（lea：先取址、再校验目标必须是引用族） |
| 3D 记录 | `CORE-OPS-SCENE3D-GET-VEC1` / `-GET-VEC2` / `-GET-LOCAL-TRANS` / `-21D` / `-21E` / `-21F` | `0x218/21a` 两个三元向量（**只查不建**，查不到写 0）、`0x228` 平移分量（标志最后写）、`0x21d` 三张表各自复制（成功才标脏）、`0x21e`（三个浮点 ÷100）、`0x21f`（轴角另存 + 度 → 弧度）。★ `0x21e/21f` 记录「已存在」位为 0 ⇒ 只建记录、一个字段都不写；旋转矩阵的元素布局 / 归一化规范**未成篇** ⇒ 实现选择（行主序 + Rodrigues，注释已标明） |
| 按住派发族 | `CORE-INPUT-RESET-FC`、`-102-SLOT`、`-103-MODULUS`、`CORE-OPS-INPUT-104`、`-105-MASK` | `0xfc` 清两张回调表（**所有帧**）+ 模数写 0，不动掩码 / 键总数 / 扫描上限 / 游标；`0x102` 键 0..31、槽 0..9 两个**独立**上界（越界一个槽都不写）；`0x103` 模数原样（0 / 负数照写）+ **只清当前帧槽**的游标；`0x104` 反向取最高置位（`limit ≤ 0` 不扫描 ⇒ 编号 = 键总数）+ 上下文闸（**只比编号**）+ 掩码命中夹扫描上限（未登记也不回滚）+ **每条出口**推进游标；`0x105` 32 位掩码原样 |
| 池翻页 | `SPC-POOL-MEMFLIP` | `0x6d` 六个全局族主 / 备**整体对调**（两组 bank 共用同一张表、键带 bank；不搬数据）；`0x09` 只清**当前选中 bank**。★ 发行数据把 `set:EnableMemFlip` 显式置 0 ⇒ 本作的翻页指令**只有异常支可达** |
| 存档族 | `CORE-OPS-SAVE-WRITE`、`-READ`、`-FILES`、`FMT-SAVE-SLOT`、`FMT-SAVE-STH` | `0x19e` 写（覆写确认只在「已存在且头读不回来」时；成功 = **0**）、`0x1a0` 读头（0/1/2 + 七个字段）、`0x1a1` 读回（打不开 ⇒ 1；成功**不写** op1；打开成功后**无条件**重编码全局整型池）、`0x1ab`/`0x1ac` 删 / 拷（两文件都尝试、后一步覆盖前一步）、`0x1ae`/`0x1af` 缩略图写 / 读（`.STH` 里的 BMP；先建空文件后捕捉、捕捉失败不删）。★ 文件系统与表面捕捉全走 `core/save_host.h` 的 `SaveHost`（核心不碰 `std::fstream`）；`0x19e` 的写侧内容构造与 `0x1a1` 的整档读回是另几张单的 seam（没注入就响亮失败） |
| 控制流 / 执行区间 / 纹理查询 | `CORE-OPS-FRAME-EXIT-SCRIPT`、`CORE-OPS-RUN-BRACKET`、`CORE-OPS-TEXTURE-SIZE` | `0x09` 丢全部帧 + 复位引擎级状态 + 以 id 0 重装根脚本；`0x1f4/0x1f5` 区间忙 / 嵌套计数（忙标记与 `0x20c` 的「别处驱动」是同一格）；`0x208` 纹理逻辑尺寸（>999 上报、槽空静默） |
| 离屏文字 | `CORE-OPS-RENDER-TEXT-204` | Shift-JIS（平台代码页 / iconv）→ FreeType 字形；主色 / 副色 / 绘制模式 / 对位状态来自字体对象。★ **宿主策略**：引擎的缓存 ascent 量的是重建入口硬编码的参考字体 "ＭＳ ゴシック"，本 emulator **固定改用渲染字体（Amayui CN）量度**（引擎在赌两者 ascent 相近，替换字体后不成立；见 `text_raster.cpp` 与 ops-forensics §28）。⚠ 副作用：Amayui 的 ascent 比参考字体大（20px 20 vs 17、28px 28 vs 24）⇒ 文字整体偏低；因此画字前把基线**抬 2px**（`EMULATOR_CXX_BASELINE_LIFT`，0 = 关）——补偿后 20px 与真机一致、28px 低 1px，数值见 ops-forensics §42。投影偏移是引擎内建常量 (1,1)（FontVWindow 样式表只抄当前值、应用借用后归还，见 §29）。**字形格式一律按引擎的 AA 三格**（`set:EnableAntiFont` + `message:UseAntiFont` → 1-bit / GRAY4 / GRAY8；级数 `message:AntiFontLevel`）：两个开关都非 0 才开、级数 == 1 才是 8 位；灰度档的级换算按引擎的 **17 / 65 级**（`core/text_alpha.h`）⇒ 满覆盖 alpha 是 **240 / 251**，⛔ 不是 255。★ 这一条**不分字形来源**（bake / GDI / FreeType 回退用同一个格式判据）——格式是引擎侧的选择（`Font+0x548` / `Font+0x35584`），不是宿主观感开关；旧口径"同色 ⇒ 恒定灰度"会让配置关掉抗锯齿时我们仍画抗锯齿（满覆盖 255 ⇒ 比真机亮一档）。合成按 `sub_46D9F0` 的 max/覆写（§32/§33）。**字形来源分流（只管"字形从哪来"，不管格式）**：`EMULATOR_CXX_GLYPH_ATLAS` 的离线 bake 数据存在时两趟照引擎；无 bake 走 FreeType（同色 = fake bold + **单趟**，异色 = 灰度 + **两趟**）。`EMULATOR_CXX_SAME_COLOR_MONO=1` 是**对照开关**（同色时强制 1 位 + 单趟，看观感差）。bake loader 目前是 stub（会警告，仍用 FT 字形）；见 §37/§38。★ **绘制模式 3 的副色趟按引擎是"径向一圈"**（半径 = 投影偏移 (1,1)、步长 = 360 ÷ (√(偏移和) × 8) ⇒ 12 趟、位移落在 3×3 邻域 ⇒ **副色只外扩 1px**），不是"沿对角线逐级加粗"（那是旧实现的宿主猜测，真机黑边 1px、我们曾画到 4px）；见 §40。★ **Windows 真机字形来源**：`EMULATOR_CXX_GLYPH_GDI=1` 时改用 GDI 的 `GetGlyphOutline`（`text_gdi.cpp`）——字型 / hinting / 栅格化 / 灰度级数都来自平台，格式按 AA 配置、两趟照引擎，与真机同源；字面名仍走 §1.1 的宿主替换（默认 `Amayui CN`），`EMULATOR_CXX_GLYPH_GDI_FACE=engine` 才按引擎的 `0x1a5` 取字（对照用）；默认关闭、只在 Windows 有实体；见 §41 |
| 源矩形窗 | `CORE-OPS-SCENE3D-239` | `0x239` 步进 / 延迟 / 到期回写四条边 |
| 安装文件切片 | `CORE-FS` 临时引导 | 松散文件优先，否则 `DATA*.ALF` 按 offset/length 切片 |

★ 引擎字符串（ALF 名字 / 配置值 / 路径）是 **CP932（ANSI）字节**，核心内部保留原始字节；
显示与宿主路径统一经 `core/encoding` 解码为 UTF-8（Windows 路径再走 UTF-16），
控制台输出在 Windows 上切 `CP_UTF8`。⛔ 不要把 CP932 字节直接 `printf` 或塞进 `std::filesystem::path`。

★ 呈现分层：绘制项 / 4 顶点网格走 **SDL_GPU 场景批次**（纹理按 `TextureRecord::revision` 缓存）；
命令档与离屏文字还没有 GPU 实现，命中时回退到软件 `rasterize_scene`（它是像素 oracle，不是交互呈现路径）。

```powershell
# 唯一的游戏流程：从根脚本走完整 boot（LOGO → 影片 → TITLE），窗口按宿主时钟推进
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install"

# 无人值守：呈现 N 帧后退出（固定分辨率 1280×720）
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --play-frames 40

# 跳过 LOGO：把引擎的 ShowLogo 格置 0（根脚本用 0x130 读它），仍走 boot 到 TITLE
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --skip-logo

# 引擎的退时保存：玩家文件 = 本地应用数据根（%LOCALAPPDATA%，⛔ 不是漫游）+ set:RegFilePath + SYS4REG.INI
#   （`core::player_config_root`；配置根口径见 data/spec 的「窗口与呈现」配置根一条，存档目录同源）
# ★ 宿主安全闸（`core::should_save_player_config`）：真实玩家配置是**外部只读数据** ⇒ **默认不写**；
#   要验证保存语义就显式指一个落点（--config <副本>）或 --config-save；--no-config-save 强制不写。
# --play-frames N 只是宿主帧数上限，不触发保存。
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --config out\SYS4REG.INI

# 单步跟踪指定条数（调试主循环 / 门服务）；跟踪完就退出，不开窗
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --trace 40

# 执行足迹：跑到 --max-steps 或停机，按首次出现顺序汇总「真的被取指过的 opcode」（不开窗）
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --footprint --max-steps 2000000

# 影片：跑到 LOGO 的 play-movie，再逐帧导出 GPU 合成（`0` 帧 = 只跑到收尾）
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --render-logo-gpu out\logo --logo-stride 40 --logo-frames 6
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --render-logo-gpu out\logo --logo-frames 0

# ★ 无窗口 + 脚本化输入：把光标搬到 TITLE 的 Option 上点一次，判据是"真的进了 CONFIG.BIN（id 52）"
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --skip-logo --play-headless ^
    --input-script tests\input\title-option.txt --anchor-script 21092 --expect-script 52 --max-steps 400000

# 再往前一步：设置界面脚本 CONFIG1.BIN（id 20940）**留在自己的输入环里**跑 30 万步，期间不许停
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --skip-logo --play-headless ^
    --input-script tests\input\title-option.txt --anchor-script 21092 --require-script 20940 --max-steps 300000

# 同一个装置的诊断口：盯脚本把输入读成了什么、只跟某个脚本的取指、把每帧导出成 BMP
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --skip-logo --play-headless ^
    --input-script tests\input\title-option.txt --anchor-script 21092 --max-steps 400000 ^
    --watch-locals 1008,1009,1010,1013,1015 --trace-script 21092 --trace-opcodes 0xcd,0x12e,0x108,0x109 ^
    --bmp out\title-bmp --bmp-stride 60 --bmp-limit 4

# 诊断：一次跑遍界面，把「走到过、但还没有 handler」的 opcode 按首次出现顺序列出来
# （缺 handler 的指令当空操作跳过 ⇒ ⛔ 这不是引擎语义，只是**发现缺口**的工具）
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --skip-logo --play-headless ^
    --input-script .tmp\probe\walk-b.txt --anchor-script 21092 --sweep-missing

# 诊断：把命名键表导成 key=value 纯文本（"界面点一下到底改了哪个配置键"的可比对产物）
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --skip-logo --play-headless ^
    --input-script apps\emulator-cxx\tests\input\config-toggle-on.txt --anchor-script 21092 --dump-config out\cfg.txt

# 单个 AGF
.\out\build\windows-x64\emulator-cxx.exe --agf "E:\Games\Eushully\天結\install\MI040.AGF"
```

★ **`--input-script` / `--play-headless` 的口径**（宿主测试装置，⛔ 不进 spec）：
- 时间线语法与 `apps/emulator-new` 的 `parseInputScript` 一致：`<毫秒> move x y` / `down N` / `up N` /
  `key N down|up` / `wheel D`，换行或 `;` 分隔，`#` 起注释。键与按钮都是**引擎编号** 0..31。
- 采样口与窗口路径**同一层**：输入字 / 光标 / 按钮位图 / 触点。没写 `move` 之前光标不可用
  （`0x109` 写 −100000、`0x108` 写 0），⛔ 不伪造坐标。
- `--anchor-script <id>`：时间线的 0 点 = **首次进入该脚本**的时刻；不给就以"播放开始"为 0 点。
  用处是让输入脚本与 boot 的快慢解耦（boot 到 TITLE 用了多久不影响脚本）。
- `--expect-script <id>`：进入该脚本即算达成（退出码 0），没进 ⇒ 退出码 1 —— 这就是 CTest 的判据。
- `--require-script <id>`：**收口时**当前脚本必须仍是它（配合 `--max-steps` 用：跑 N 步后断言"还在这个脚本里"）。
- 未实现 / 非法 opcode / 内部错误停机 ⇒ 退出码 1（正常的 `exit` 停机不算失败）。
- `--bmp <dir>`：每次渲染请求导出一张（`--bmp-stride` / `--bmp-limit` 限流）；达成时另存 `reached-<id>.bmp`。
- 诊断口（⛔ 不是产品路径）：`--watch-locals 1008,1013`（看脚本把输入读成了什么）、
  `--trace-script <id> --trace-opcodes 0xcd,0x12e`（只看该脚本里这些 opcode 的取指）。

★ 当前是 **最小可执行层**：`CORE-ISA` / `CORE-POOL` / `CORE-FRAME` / `CORE-VM` / `CORE-CONFIG` + `FMT-ALF` / `FMT-AGF`。`--config` 可指定玩家 `SYS4REG.INI`；缺省按 `set:UseAppDataFolder` 选配置根（`%APPDATA%` / `XDG_CONFIG_HOME` / `~/.config`，否则安装根），
再拼 `set:RegFilePath` 与文件名。
门 / 状态位服务：**等待位与影片位**已建模（影片位只管关门，收尾在门之前的影片对象表服务里）；其余状态位、换帧 / 换脚本、容器句柄尚未完整建模。
★ 由此有一条**看起来像卡死、其实是规范要求**的行为：`0x19c` 的置位支与 `0x1ce` 的非 0 支会把门字第 27 / 30 位置起，而这两个位在门里**都没有清位分支**（第 30 位 = 「逐格揭示活动中」：门之前的每迭代服务只按节拍请求画一格并推进格号、**不负责清它**；第 27 位连服务分支都没有）⇒ 门一直关着，`run()` 走到 step 上限。那不是 handler 写错，是「位是待服务标记，清不清由服务分支决定」的直接后果；要推进只能由外部清位写点（可达性未取证）。
★ 输入字形 / 触点都是**平台边界**：没有窗口时默认没有输入后端（`0xff` 恒采到 0、`0x2fc` 恒"没有触点"、
`0x109` 恒写 −100000），`0x12e` 的命中查找因此不会进入。**无人值守要驱动 TITLE 的输入环，用 `--input-script`**
（见下）：它把一份**指令式时间线**归约成引擎要的四个量（输入字 / 光标 / 按钮位图 / 触点），
与 `apps/emulator-new` 的 `parseInputScript` 同一套语法。测试用 `set_input_word()` / `set_key_sampler()` /
`set_touch_source()` 注入，⛔ 不要把这几个注入点当产品路径。

★ **窗口路径已接键盘 + 鼠标**（不接摇杆）：`window.cpp` 每个外层迭代泵事件，
`SDL_GetKeyboardState` → 输入字（`kKeyBits` 表：位号 ↔ 扫描码）、`SDL_GetMouseState` → 光标 / 按钮位图。
鼠标点击边沿由 `0x108` 的位图差分得到；**不把鼠标按钮合成触点**（桌面鼠标-only 的 `0x2fc` 恒为 0）。
它们只在脚本执行到 `0xff` / `0x109` / `0x108` / `0x2fc` 时才被取用
（引擎不在等待期间自己泵输入）。鼠标回调按 `0xcd` 的时间闸派发。
★ 键位映射是**宿主策略**：规范明说「物理键与输入位号的映射」属平台边界，所以它**不进 spec**；
要换键位只改 `window.cpp` 的 `kKeyBits`。Esc 仍然直接关窗（与"取消"位同时映射到 1）。
★ 光标"可用"的判据照规范：**窗口存在且输入可用**才查询；不再用"光标是否落在客户区内"当哨兵——
客户区外写**真实坐标（可为负 / 超界）**。有鼠标焦点时用 `SDL_GetMouseState`（客户区坐标），否则用全局光标减窗口位置；
两种都按 `逻辑分辨率 / SDL 窗口尺寸` 缩放。没有窗口 / 输入停用 ⇒ `0x109` 写 −100000（`0x108` 写 0）。
诊断：`EMULATOR_CXX_INPUT_TRACE=1` 时每 30 次采样往 stderr 打一行 `[input] cursor inside=…`。
★ 根帧执行 `exit`（TITLE 退出路径的 `0x2`）⇒ 脚本结束 ⇒ 关窗并返回；不再"保留最后一帧等用户关窗"。
`--stop-at <opcode>` 可用于在任意 opcode 前停止（`0` = 不设停止线，缺省）。
根脚本静态解码会在数据区遇到非法 opcode 后停止，这是规范允许的静态解码边界，不是执行边界。

★ **没有"直接跳到 TITLE"之类的捷径**：早期那批 `--to-title` / `--render-title*` / `--play-title` 选项已删除 ——
它们会跳过 boot，看到的东西不是引擎真实会走出来的画面。`--skip-logo` **不是**这种捷径：它只把引擎自己的
`ShowLogo` 格置 0，根脚本仍按 `0x130` 的分支走 boot（只是不 `call-script` LOGO）。现在唯一流程是从根脚本跑 boot，
按引擎的「门前渲染」节奏呈现（场景脏 / 有动画 / 影片新帧 ⇒ 下一次满足闸门的迭代渲染一次）。
`--play-frames N` 只是给这个流程一个帧数上限，便于无人值守。

## 运行 / 门禁

```powershell
.\out\build\windows-x64\emulator-cxx.exe --install "<游戏安装目录>"   # boot 窗口，Esc 退出
.\out\build\windows-x64\emulator-cxx.exe --smoke   # 离屏 clear + 读回 + BMP
ctest --test-dir out\build\windows-x64 --output-on-failure

# 可选：把真实安装目录挂进 CTest（boot-play / logo-movie / logo-gpu）
cmake --preset windows-x64 -DEMULATOR_CXX_INSTALL_DIR="E:\Games\Eushully\天結\install"
ctest --test-dir out\build\windows-x64 --output-on-failure
```

★ **CTest 的分级由「注册位置」决定**（不是 pragma，也不另建清单）：`add_test` 写在**顶层** ⇒
免安装、默认 `ctest` 就会跑 ⇒ 可当 spec 的 `reviewed` 级证据；写进任何
`if` / `foreach` / `function` / `macro` / `while` / `block` 块里 ⇒ 默认门禁不跑 ⇒ 只能当 `draft` 证据。
判据在 `tools/lib/ledger.mjs` 的 `cmakeTestTier`，由 `tools/test/ledger.test.mjs` 与
`tools/test/spec.test.mjs` 两侧钉住。⇒ 想让一条行为级用例成为规范证据，**把它注册在最外层**。

`--smoke` 会：

1. 创建 1280×720 `R8G8B8A8_UNORM` 离屏纹理；
2. `SDL_BeginGPURenderPass` clear 到固定颜色；
3. `SDL_SubmitGPUCommandBufferAndAcquireFence` 等待完成；
4. `SDL_DownloadFromGPUTexture` 读回并断言中心像素；
5. 2×2 纹理 upload → download 往返断言；
6. 2×2 纹理四边形：alpha 混合与 reverse-subtract 混合，各自读回断言
   （混合是 fixed-function，所以三个平台该给出同一组像素）；
7. 四边形 / 场景两条 pipeline 各按 RGBA8（离屏）与 BGRA8（呈现用的 swapchain 格式）建一遍；
8. 打印本次实际走的着色器路径（`shader=dxil|spirv|msl`）；
9. 写 `emulator-cxx-smoke.bmp`。

它不依赖窗口合成器，因此是后续渲染回归的确定性基线。

## 当前 POC 验证什么

- vcpkg manifest + submodule 构建；
- SDL3 窗口创建；
- SDL_GPU 设备 / swapchain / 显式 present；
- 离屏 clear、GPU 命令提交、download 读回与像素断言；
- 纹理四边形：alpha / reverse-subtract 混合（HLSL sampler 走 SDL 约定的 `space2`，MSL 走 `[[texture(0)]]` / `[[sampler(0)]]`）；
- 2×2 纹理 upload/download 往返；
- HLSL → DXIL 的 shadercross 编译（Windows / Linux）与 MSL 源文本直编（macOS）；
- 程序不依赖 Electron / Chromium / WebGL。

下一步在 `renderer` feature 打开后接入 bgfx：
纹理四边形 → alpha / add / reverse-subtract 三种 blend → 与现有像素对照。

## 规范边界

`data/spec` 仍是唯一真源。C++ 实现只消费规范结论，不反向把实现细节写进 spec。
