// emulator-lift · AOT 宿主（bring-up + 宿主服务装配）—— 实现
//
// ★ 本文件只做**装配与读数**；执行循环在 `aot_run.cpp`（同一个类，方法分开只为"装配"与"执行"两件事
//   各自可读）。常量一律照 `src/runtime/machine.cpp` 的 `Machine::init` / `wire` / `loadImage` /
//   `prepare`（那是解释器臂的**唯一**真源），⛔ 不在这里另写一套。
#include "aot/aot_host.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <thread>
#include <utility>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
// ---- 采样式内部 profile（`--profile`）的取时件：x86 用 TSC，其它平台退到 steady_clock ----
//   ★ 关档时这些一行都不执行（见 `AotProfile` 的三条硬口径）。
#define LIFT_AOT_HAS_TSC 1
#endif
#if defined(LIFT_AOT_HAS_TSC) && defined(_WIN32)
#include <intrin.h>  // __rdtsc（clang-cl / cl.exe）
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // GetProcessTimes（frame-ms 的"自进程启动"基准）
#endif

#include "aot/aot_dispatch.h"
#include "aot/aot_feeder.h"       // feeder 的 IR 执行器（音频真线程用）
#include "aot/call_diag.h"        // 临时诊断：`LIFT_CALL_DIAG=1`（缺省零输出）
#include "aot/memcpy_diag.h"      // 临时诊断：`LIFT_MEMCPY_DIAG=1`（缺省零输出）
#include "runtime/lift_intrinsics.h"  // 构建期库内建的收口计数（一次性输出）
#include "runtime/clipboard.h"     // 剪贴板（切片 3：overlay 的 PROFILE 桥在 profile_end 里写）
#include "runtime/control.h"       // 控制层 Present 闸门 / Context（轻头：⛔ 无 beast / nlohmann）
#include "runtime/decode_cache.h"  // `liftOneDecodeCache()`：只读命中率（`decode-cache:` 行）
#include "runtime/fault_state.h"   // guest fault 现场（AOT 没有 Machine ⇒ 本臂自己登记；见该头注）
#include "runtime/guest_token.h"   // Phase-0（`arm()` 在 `AotHost::run`；这里只读）
#include "runtime/input.h"         // S4：`controlOnResume` 的 `input::dropOsLevels`（暂停恢复收口）
#include "runtime/script_globals.h"  // `global-get/set` 的 AGE 引擎层实现（控制层只转发）
#include "runtime/token_phase0_report.h"  // tokenhold/holdhist/waithist/shimmax 的唯一打印点
#include "shims/register.h"

namespace lift {
namespace aot {

bool AotHost::init(std::string* err) {
    // ---- ① 4 GiB 预留 + 平台 fault handler ----
    // ★ `GuestMemory` 的构造就 reserve 了一次；这里**显式**再 reserve 一次：幂等（已 reserve 直接
    //   返回 true），但把"预留失败"变成**这一步**的返回值 —— 而不是让后面某个 commit 报一句
    //   看不出根因的错。平台 fault handler 也在 `reserve` 里装（见 memory.cpp）。
    if (!mem_.reserve()) {
        if (err) *err = std::string("预留 guest 4 GiB 地址空间失败：") + mem_.lastError();
        return false;
    }
    // ★ `LIFT_SAFE_MEMORY=1` 是**同一条** env 口径（不是给 AOT 另开一个开关）：判据 A1 要求
    //   "再跑一遍 safe memory 全绿"对两臂都成立，两种内存护栏必须能互相核对。
    mem_.setSafeMode(GuestMemory::safeModeFromEnv());

    // ---- ② surface arena（真源 = `GuestLayout::arena_base/arena_size`，与 `Machine::init` 同一份）----
    // ★ 图形 shim 的 `requireMachine`（`d3d9.cpp:115` / `ddraw_surface.cpp:107`）第一件事就是查
    //   `surfaces().ready()`；不建它 ⇒ 图形路径**必停**。基址 / 大小从布局常量读（⛔ 不另写字面量：
    //   这份容量是自定模型，两臂必须逐位相同，见 `image.h` 该字段的注释）。
    if (!surfaces_.init(&mem_, layout_.arena_base, layout_.arena_size)) {
        if (err) *err = std::string("surface arena 初始化失败：") + surfaces_.lastError();
        return false;
    }
    // ---- ③ 解码 / 执行服务：`lift_one` 依赖 `HostServices::decoder()`（不提供就必停）----
    if (!decoder_.ok()) {
        if (err) *err = std::string("解码器初始化失败：") + decoder_.lastError();
        return false;
    }

    // ---- ④ 把服务指针接到 cpu 上（真源 `Machine::wire`，`machine.cpp:333-364`）----
    cpu_.mem = &mem_;
    cpu_.heap = &heap_;
    cpu_.handles = &handles_;
    cpu_.clock = &clock_;
    cpu_.shims = &shims_;
    cpu_.vfs = &vfs_;
    cpu_.image = &image_;
    cpu_.com = &com_;
    cpu_.calls = &calls_;
    cpu_.windows = &windows_;
    cpu_.fonts = &fonts_;
    cpu_.cache = nullptr;    // ★ AOT 没有 BlockCache（块缓存是解释器的执行件）
    cpu_.lst = nullptr;      // `.lst` 是诊断/对照语料，执行不需要
    cpu_.machine = nullptr;  // ★ AOT **有意**没有 Machine：`machineOf()` 就是 nullptr，⛔ 不伪造
    // ★ guest `CRITICAL_SECTION` ↔ 宿主递归锁（与 `Machine::wire` 的 `cpu_.cs_table = &critical_sections_`
    //   同形）。这是音频**真 feeder 线程**的硬前提：没有它，`guestTryEnterCriticalSectionAs` 会走
    //   "guest 字段当锁"的退化路径（争用时返回非空 err ⇒ `feederEvent` 当致命 ⇒ 第一次并发就打死 feeder）。
    cpu_.cs_table = &cs_;
    // ★ 宿主服务视图（`runtime/host_services.h`）：`AotHost` 自己实现那份接口 ⇒ shim 侧
    //   `hostOf(cpu)` / `hostOf(cpu)->ownsCpu(cpu)` 的判据对 AOT 成立（D4 的解耦面）。
    cpu_.host = this;
    // ★ `present-tail` 的 insns 源：与 `Machine::wire` 同一个注入点（只读指针）。
    presenter_.setInsnSource(&cpu_.insns);
    // ★ 音频：`attach` 之后先落**缺省档**（`configure(off,"",false)`）——它与改动前的形状逐位相同。
    //   ★ CLI 的 `--audio` / `--audio-dump` / `--sound` 由 `aot_main` 在 `init()` 之后调
    //     `configureAudio()` 覆盖（`configure` 只要求"还没 begin"）；缺省档不会走到那一步的差异上。
    //   ★ 生命周期（begin / end）在 `run()` 的入口 / 出口（与解释器 `machine.cpp:1059` / `:1854` 同口径）。
    //   ★ **四参 attach**：feeder 的 TEB / 专用 guest 栈来自 `GuestLayout`（`runtime/image.h` 唯一真源）。
    //     这是结构前提（不是运行时选项）；⛔ 不要再退回"按 `cpu_.machine != nullptr` 判能不能起 feeder"。
    audio_.attach(&cpu_, &mem_, &clock_, layout_);
    // ★ 音频 feeder 的 guest 执行器 = **AOT IR 派发**（`AGENTS.md` §12.8：执行来源只有 IR + shim）。
    //   `entry/return` 常量由 `AudioEngine` 传入（`kSub4B6C30` / `kFeederReturnEa` 的唯一来源在 audio.cpp）。
    //   ⛔ 不要在这里注入解释器路径（那会让 AOT 臂长出第三份执行来源）。
    audio_.setFeederGuestRunner(FeederGuestRunner{&AotHost::feederRunnerThunk, this});
    {
        std::string audio_err;
        if (!audio_.configure(AudioMode::Off, std::string(), false, &audio_err) && err != nullptr) {
            *err = "音频后端 configure(off) 失败：" + audio_err;
            return false;
        }
    }
    cpu_.audio = &audio_;
    // ★ `WindowManager` 的 owner 线程 = 跑执行循环的这个线程（`Machine::run` 的第一句同口径）。
    windows_.setOwnerThread(std::this_thread::get_id());
    // ★ 时钟 = **墙钟**（唯一档）：这是**这台宿主**的属性，不是某一轮的选项 ⇒ 在装配期就定下来。
    //   `Clock` 没有第二个档（⛔ 不要再加"虚拟时钟"）。
    seh_.reset();
    cpu_.fpu.reset();
    // ★ 音频 feeder 的**硬前提**自检：没有 `cs_table` 时 guest CS 会走退化路径（见上面的接线）。
    //   这里只做一次判空（装配顺序错 ⇒ 响亮失败，而不是等到第一次并发才炸）。
    if (cpu_.cs_table == nullptr) {
        if (err != nullptr) *err = "AOT 装配不完整：cpu_.cs_table 为空（音频 feeder 需要宿主临界区表）";
        return false;
    }
    // ★ guest fault 现场（`runtime/fault_state.h`）：解释器臂在 `Machine::init` 装自己的钩子，AOT 臂
    //   没有 Machine ⇒ 必须在这里装一次（`guestFault` 的 `g_hook` 是同一个槽位）。**只装钩子**；
    //   现场数据由 `run()` 在入口登记（⛔ 不是每块 / 每帧 ⇒ 执行循环零成本）。
    installAotFaultHook();
    // ★ 帧列表读数源（`game.local_frames()`）：装配期只 bind 两个非 owning 指针
    //   （`mem_` 已 reserve；`control_marker_` 是成员 ⇒ 同一生命周期）。⛔ 这里不读 guest。
    frame_source_.bind(&mem_, &control_marker_);
    initialized_ = true;
    return true;
}

bool AotHost::feederRunnerThunk(void* ctx, X86& cpu, std::uint32_t entry_ea, std::uint32_t return_ea,
                                std::string* err) {
    // ★ 只转发：guest 语义在生成物（IR）与 shim 里，本函数一行语义都不写。
    //   `ctx` 是本宿主（`init()` 里注入）；`mem_` / `shims_` 都是**同一台宿主**的那一份。
    auto* host = static_cast<AotHost*>(ctx);
    if (host == nullptr) {
        if (err != nullptr) *err = "音频 feeder：执行器 ctx 为空";
        return false;
    }
    return runAotFeederGuest(cpu, host->mem_, host->shims_, entry_ea, return_ea, err);
}

bool AotHost::configureAudio(AudioMode mode, const std::string& dump_path, bool sound, std::string* err) {
    // ★ 唯一入口（`aot_main`）：把 CLI 的三个事实落进 `AudioEngine`。`configure` 只要求"还没 begin"
    //   ⇒ 在 `init()` 之后、`run()` 之前调。⛔ 不要在这里做值域校验（那是 `audio_policy.h` 的
    //   `parseAudioMode`，唯一一份；这里只收已经合法的 `AudioMode`）。
    if (!initialized_) {
        if (err != nullptr) *err = "AotHost 未 init";
        return false;
    }
    std::string local;
    if (!audio_.configure(mode, dump_path, sound, &local)) {
        if (err != nullptr) *err = local.empty() ? std::string("AudioEngine::configure 失败") : local;
        return false;
    }
    return true;
}

bool AotHost::loadImage(const std::string& exe_path, std::string* err) {
    if (!initialized_) {
        if (err) *err = "AotHost 未 init";
        return false;
    }
    // ★ 解析 PE（头 / 节表 / 导入表 / entry / sha256）：`loadPeFile` 是**自由函数**，不需要 Machine。
    if (!loadPeFile(exe_path, &image_, err)) return false;
    exe_path_ = exe_path;

    // ---- D1：**先核对**构建期 pin（sha256 + entry），再往 guest 里映射 ----
    // ★ 为什么在映射之前：pin 不一致意味着"这一份 exe 不是 lifted 代码焊死的那一份" ⇒ 后面每一个
    //   EA / 块边界 / 操作数语义都可能落在错的字节上。核对放在第一个写 guest 的动作之前，
    //   这样**失败时 guest 里一个字节都没动**（可诊断、可重试）。
    // ⛔ 不降级、不"警告后继续"：调用方按 `pinMismatch()` 走 exit 2。
    if (generatedPresent()) {
        pin_checked_ = true;
        if (image_.sha256 != std::string(pinnedSha256()) || image_.entry_ea != pinnedEntry()) {
            pin_mismatch_ = true;
            if (err != nullptr) {
                char buf[512];
                std::snprintf(buf, sizeof(buf),
                              "执行源与构建期 pin 不一致（D1）\n"
                              "  构建期（lift-gen 读的 PE）：sha256=%s entry=%08X\n"
                              "  本次（--exe / env 给的 PE）：sha256=%s entry=%08X\n"
                              "  ⇒ lifted 代码是把 EA / 块边界 / 操作数语义焊死在构建期那份字节上的；"
                              "换一份 exe 只会**静默跑错**。⛔ 不降级、不继续。",
                              pinnedSha256(), pinnedEntry(), image_.sha256.c_str(), image_.entry_ea);
                *err = buf;
            }
            return false;
        }
    }

    // ★ 与 `Machine::loadImage` 同一口径：布局的缺省基址只作缺省，**按 PE 头为准**（M1 不做重定位）。
    //   打 stderr 而不是静默改：这是一条会改变 guest 里一切绝对地址的事实。
    if (image_.image_base != layout_.image_base) {
        std::fprintf(stderr, "[aot] 镜像基址 %.8X 与布局缺省 %.8X 不同 ⇒ 按 PE 头为准（M1 不做重定位）\n",
                     image_.image_base, layout_.image_base);
        layout_.image_base = image_.image_base;
    }
    // ★ 映射节（提交 + 清 .bss 尾巴）+ 把 PE 头也拷进去：guest 可能按 MZ 头反查模块。
    //   失败时 guest 里**一个字节都没写**（两遍校验，见 image.cpp）。
    if (!mapPeImage(mem_, image_, err)) return false;
    image_loaded_ = true;
    // ★ 主模块的身份：`GetModuleHandle(NULL)` 的返回值 / `GetModuleHandleEx(FROM_ADDRESS)` 的上界。
    cpu_.shim.module_handle = image_.image_base;
    cpu_.shim.module_size = image_.size_of_image;
    return true;
}

bool AotHost::prepare(std::string* err) {
    if (!image_loaded_) {
        if (err) *err = "还没有载入镜像";
        return false;
    }

    // ---- ① VFS overlay：guest 打开 `<guest 根>\AGE.EXE` 读到的是**我们真正在跑的那份字节** ----
    // ★ 为什么在这一步（而不是 loadImage）：此时 `--game-dir` / `--guest-root` 都已装配完，
    //   guest 路径才定得下来。⛔ 不让 guest 读到 `--game-dir` 下那份加壳原件。
    vfs_.setExeOverlay(vfs_.guestExePath(), exe_path_, image_.sha256);

    // ---- ② guest 进程结构：栈（1 MiB + guard 页）+ TEB / PEB / TLS 数组 + 命令行 / 环境块 ----
    // ★ `setupProcessStructures` 是自由函数：它只认 `GuestMemory` / `PeImage` / `GuestLayout` / `Vfs`。
    if (!setupProcessStructures(mem_, image_, layout_, vfs_, &setup_, err)) return false;

    // ---- ③ guest 堆：Win32 形状 API 的唯一底层分配器（`HeapAlloc` / `LocalAlloc` / … 都落这里）----
    if (!heap_.init(&mem_, layout_.heap_base, layout_.heap_size)) {
        if (err) *err = heap_.lastError();
        return false;
    }

    // ---- ④ shim 桩页 + 注册 + bind ----
    // ★ 桩页（`GuestLayout::shim_page`）必须先**提交**：bind 只写"每个导入一个唯一地址"进 IAT，
    //   而 guest 的 `call [IAT]` 落到那个地址时，那里的字节必须存在（解释器臂里它是块缓存里的
    //   shim 块；AOT 里它是 `ShimRegistry::byStubEa` 能反查到的桩 EA）。
    if (!mem_.commitZeroed(layout_.shim_page, static_cast<std::size_t>(layout_.shim_slots) * 4u,
                           RegionKind::ShimPage, "shim-page")) {
        if (err) *err = "提交 shim 桩页失败";
        return false;
    }
    // ★ 注册顺序 = `ShimDef::id` = trace 顺序 ⇒ 与 `Machine::prepare` 逐字相同（改顺序 = 改 trace）。
    //   ★ 这两步已经抽成 `registerAotShims`（`shims/register.h`）：生成期（`lift-gen`）算
    //     "`call *<IAT 槽>` 的清理字节"时用的是**同一份**序列（REQ-01M86NMT0YJ9V78XS2DZZV364Q）。
    registerAotShims(shims_);
    // ★ bind：给每个导入分配唯一 guest 地址并把地址写回 IAT 槽（未实现的导入**也**分到地址，
    //   执行到就响亮停机 —— 那是解释器臂的语义，AOT 只把地址铺好）。
    if (!shims_.bind(mem_, image_, layout_.shim_page, layout_.shim_slots, err)) return false;
    // ⛔ **不** `cache_.installShim`：AOT 没有 BlockCache。桩 EA → id 由 `lift_shim` 的
    //   `ShimRegistry::idForStub` / `byStubEa` 反查（`lift_abi.cpp:46-58` 的回退路径）。

    // ---- ⑤ cpu 初始寄存器 / shim 侧的 EA ----
    // ★ 常量全部来自 `Machine::prepare`（int 解释器臂的真源），⛔ 不在这里重新发明：
    //   · eip   = PE entry；
    //   · fs    = TEB（`fs:[0]` 是 SEH 链头、`fs:[0x2C]` 是 TLS 数组、`fs:[0x30]` 是 PEB）；
    //   · esp   = 栈顶 - 4，并在那里写 0xFFFFFFFF —— 那是**返回地址哨兵**：真回到它就停；
    //   · cmdline / exe_path / env 的 EA 来自 `ProcessSetup`（CRT 启动会读它们）；
    //   · va_cursor / va_limit = VirtualAlloc bump 指针的区间。
    cpu_.eip = image_.entry_ea;
    cpu_.fs_base = setup_.teb;
    cpu_.gs_base = 0;
    cpu_.esp() = setup_.stack_top - 4u;
    mem_.wr32(cpu_.esp(), 0xFFFFFFFFu);  // 栈顶的返回地址哨兵
    cpu_.shim.cmdline_ea = setup_.cmdline_ea;
    cpu_.shim.exe_path_ea = setup_.exe_path_ea;
    cpu_.shim.env_ea = setup_.env_ea;
    cpu_.shim.va_cursor = 0x20000000u;
    // ★ 上界**贴着** arena 的基址（与 `Machine::prepare` 同一条口径）：两者必须相邻而不相交。
    cpu_.shim.va_limit = layout_.arena_base;

    prepared_ = true;
    return true;
}

// AGERC.DLL：真实 guest 模块装载（真源 `Machine::loadAgercModule`，`machine.cpp:404-493`）。
// ★ 为什么 AOT 也必须做：guest 在启动早期就 `LoadLibraryA("AGERC.DLL")`（`ret=0048E751`），
//   拿不到镜像 ⇒ 它返回 0 + 126 ⇒ guest 走**错误对话框**分支（`wsprintfA` → `MessageBoxA`）。
//   实测读数：不装 AGERC 时 AOT 在 36,489 条指令处停在 `poly:005C3627`（= `j_MessageBoxA_15` thunk，
//   它不在闭包里 ⇒ 响亮停），而解释器在同一个 `LoadLibraryA` 上拿到换基址后的 fallback base 并继续
//   `GetProcAddress`。⇒ 这是**宿主 bring-up 缺口**，不是 lift 语义问题。
bool AotHost::loadAgercModule(const std::string& path, std::string* err) {
    auto setErr = [err](const char* step, const std::string& why) {
        if (err) *err = std::string("AGERC 装载失败（") + step + "）：" + why;
    };
    if (!initialized_ || !prepared_) {
        setErr("装配顺序", "AotHost 还没 init / prepare");
        return false;
    }
    PeImage img;
    std::string perr;
    if (!loadPeFile(path, &img, &perr)) {
        setErr("读/解析 PE", perr + "（路径 " + path + "）");
        return false;
    }
    if (img.size_of_image == 0) {
        setErr("校验 PE", "SizeOfImage 为 0（无法确定映射长度）");
        return false;
    }

    // ---- 选一个不与现有 guest 布局冲突的 base ----
    // ★ 判据与 `Machine::loadAgercModule` **逐项相同**：优先用 PE 头里的 ImageBase（真机 loader 也尽量
    //   尊重它）；冲突 ⇒ 用 `GuestLayout::agerc_fallback_base`。冲突表 = 主镜像 / 堆 / 栈（含 guard）/
    //   TEB·PEB·TLS / 命令行块 / 桩页 / VirtualAlloc 区间 / **surface arena** + 假模块句柄空间
    //   + 所有已登记区域。
    const std::uint32_t kFallbackBase = layout_.agerc_fallback_base;
    struct Range {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
    };
    std::vector<Range> used;
    auto addRange = [&used](std::uint64_t begin, std::uint64_t end) {
        if (end > begin) used.push_back({begin, end});
    };
    if (image_loaded_) {
        addRange(image_.image_base,
                 static_cast<std::uint64_t>(image_.image_base) + image_.size_of_image);
    }
    addRange(layout_.heap_base, static_cast<std::uint64_t>(layout_.heap_base) + layout_.heap_size);
    // ★ arena 显式在表里（理由同 `Machine::loadAgercModule`）：它在 `init()` 里整段 commit，
    //   "AGERC 的基址不许落进 arena"应是结构判断，⛔ 不依赖装配顺序。
    addRange(layout_.arena_base, static_cast<std::uint64_t>(layout_.arena_base) + layout_.arena_size);
    addRange(static_cast<std::uint64_t>(layout_.stack_top) - layout_.stack_size - layout_.stack_guard,
             layout_.stack_top);
    addRange(layout_.teb_ea, static_cast<std::uint64_t>(layout_.teb_ea) + 0x1000u);
    addRange(layout_.peb_ea, static_cast<std::uint64_t>(layout_.peb_ea) + 0x1000u);
    addRange(layout_.tls_array_ea, static_cast<std::uint64_t>(layout_.tls_array_ea) + 0x1000u);
    addRange(layout_.argv_block, static_cast<std::uint64_t>(layout_.argv_block) + 0x1000u);
    addRange(layout_.shim_page, static_cast<std::uint64_t>(layout_.shim_page) +
                                    static_cast<std::uint64_t>(layout_.shim_slots) * 4u);
    // ★ 音频 feeder 的两页（`GuestLayout::feeder_teb` / `feeder_stack_top-size..top`）也是"已用区间"：
    //   `AudioEngine::setupFeederContext()` 会在**运行中**提交它们（装配期 AGERC 已经选完基址，
    //   但"谁占哪段"的清单必须完整 —— 否则 AGERC 恰好落在 feeder 页时只能靠 commit 时的占用检查兜底）。
    addRange(layout_.feeder_teb, static_cast<std::uint64_t>(layout_.feeder_teb) + 0x1000u);
    if (layout_.feeder_stack_size != 0) {
        addRange(static_cast<std::uint64_t>(layout_.feeder_stack_top) - layout_.feeder_stack_size,
                 layout_.feeder_stack_top);
    }
    addRange(cpu_.shim.va_cursor, cpu_.shim.va_limit);
    // 假模块句柄空间（kernel32_process.cpp 的 `0x7F000000 + 序号*64KiB`）⇒ AGERC 的 base 不能落在里面。
    addRange(0x7F000000u, 0x7FF00000u);
    for (const Region& r : mem_.regions()) addRange(r.begin, r.end);

    auto fits = [&](std::uint32_t base) {
        if (base == 0) return false;
        const std::uint64_t end = static_cast<std::uint64_t>(base) + img.size_of_image;
        if (end > GuestMemory::kSpace) return false;  // 越界（含 uint32 回绕）
        for (const Range& r : used) {
            if (static_cast<std::uint64_t>(base) < r.end && r.begin < end) return false;
        }
        return true;
    };

    std::uint32_t base = img.image_base;
    if (!fits(base)) {
        std::fprintf(stderr, "[aot] AGERC 的 ImageBase %08X（+%X）与现有 guest 布局冲突 ⇒ 改用 %08X\n",
                     img.image_base, img.size_of_image, kFallbackBase);
        base = kFallbackBase;
        if (!fits(base)) {
            char why[160];
            std::snprintf(why, sizeof(why),
                          "PE 的 ImageBase 冲突，备选 0x%08X 也冲突（没有可用的 guest 基址）",
                          kFallbackBase);
            setErr("选基址", why);
            return false;
        }
    }
    if (!mapPeImageAt(mem_, img, base, &perr)) {
        setErr("映射镜像", perr);
        return false;
    }

    agerc_image_ = std::move(img);
    agerc_path_ = path;
    agerc_base_ = base;
    agerc_loaded_ = true;
    // 交给 shim 的**唯一**令牌：真实 ImageBase + 镜像指针（资源 / 版本从这里读）。
    // `agerc_loaded` 仍是 false：只有 guest 真的调 LoadLibrary("AGERC.DLL") 才置 true。
    cpu_.shim.agerc_image = &agerc_image_;
    cpu_.shim.agerc_base = agerc_base_;
    cpu_.shim.agerc_loaded = false;
    return true;
}

GpuRaster* AotHost::gpuRaster() {
    // ★ 与 `Machine::gpuRaster()`（`machine.cpp:3311-3315`）**逐字同形**：显式设置的后端优先，
    //   否则取 presenter 的 sink。两个档位（headless 隐藏窗口 / window 可见窗口）都要能拿到它 ——
    //   `nullptr` 不是"少画一点"，而是 `d3d9_pipeline.cpp:3983` 的整批拒批（3D 一个像素都没有）。
    if (gpu_raster_ != nullptr) return gpu_raster_;
    PresentSink* s = presenter_.sink();
    return s != nullptr ? s->gpuRaster() : nullptr;
}

const GpuRaster* AotHost::gpuRaster() const {
    if (gpu_raster_ != nullptr) return gpu_raster_;
    const PresentSink* s = presenter_.sink();
    return s != nullptr ? const_cast<PresentSink*>(s)->gpuRaster() : nullptr;
}

void AotHost::printReadout(std::FILE* out, bool region_log, bool detail) const {
    if (out == nullptr) return;

    // ---- 启动 readout（**只给 `--log-level info` 及以上**；缺省 warn ⇒ 一行都不打）----
    //   ★ 这些是"这台机器怎么装配"的**展开**（PE 头 / 节表 / 导入模块表 / guest 地址空间 /
    //     服务面 / `pin`），一次 run 里不变 ⇒ 默认档一行都不打（2026-11-27 用户判据：默认档只留
    //     出错 / 警告 + 插件日志 + 收口那 1 行 `stop`）。
    if (detail) {
        // ---- 镜像身份（一行）+ 节表 ----
        std::fprintf(out, "exe       : %s\n", exe_path_.c_str());
        std::fprintf(out,
                     "image     : base=%08X entry=%08X size_of_image=%08X headers=%08X sections=%zu "
                     "imports=%zu sha256=%s\n",
                     image_.image_base, image_.entry_ea, image_.size_of_image, image_.size_of_headers,
                     image_.sections.size(), image_.imports.size(), image_.sha256.c_str());
        for (const PeSectionInfo& s : image_.sections) {
            // 格式照 main.cpp 的 Verbose 节表行（同一份读数，⛔ 不另立一套字段名）
            std::fprintf(out, "  %-8s va=%08X vsize=%8X raw=%8X flags=%08X\n", s.name, s.va, s.vsize,
                         s.raw_size, s.characteristics);
        }

        // ---- 导入表：总数 + 按模块分组（分组是"静态缺口在哪一族"的第一手读数）----
        std::map<std::string, std::size_t> by_module;
        for (const PeImportInfo& imp : image_.imports) by_module[imp.module]++;
        std::fprintf(out, "imports   : total=%zu modules=%zu\n", image_.imports.size(), by_module.size());
        for (const auto& kv : by_module) {
            std::fprintf(out, "  %-14s %zu\n", kv.first.c_str(), kv.second);
        }

        // ---- shim：注册条数 / 绑定到的唯一桩地址数 / 未实现缺口 ----
        std::size_t with_stub = 0;
        std::size_t implemented_imported = 0;
        for (const ShimDef& d : shims_.all()) {
            if (d.stub_ea != 0) ++with_stub;
            if (d.imported && d.fn != nullptr) ++implemented_imported;
        }
        std::fprintf(out,
                     "shim      : registered=%zu imported=%zu implemented=%zu gaps=%zu bound-stub-ea=%zu | "
                     "page=%08X slots=%u next-slot=%u\n",
                     shims_.size(), shims_.importedCount(), implemented_imported,
                     shims_.importedButUnimplemented().size(), with_stub, shims_.stubPage(),
                     shims_.stubSlots(), shims_.nextStubSlot());

        // ---- 栈 / 堆 / TEB ----
        const std::uint32_t esp_sentinel =
            mem_.committed(cpu_.esp(), 4u) ? mem_.rd32(cpu_.esp()) : 0xDEADBEEFu;
        std::fprintf(out,
                     "stack     : top=%08X bottom=%08X guard=[%08X,%08X) esp=%08X [esp]=%08X%s\n",
                     setup_.stack_top, setup_.stack_bottom,
                     layout_.stack_top - layout_.stack_size - layout_.stack_guard,
                     layout_.stack_top - layout_.stack_size, cpu_.esp(), esp_sentinel,
                     esp_sentinel == 0xFFFFFFFFu ? "（0xFFFFFFFF = 返回地址哨兵）" : "（⚠ 哨兵没写上）");
        std::fprintf(out, "heap      : base=%08X end=%08X size=%08X blocks=%zu\n", heap_.begin(), heap_.end(),
                     heap_.end() - heap_.begin(), heap_.blockCount());
        std::fprintf(out, "teb       : teb=%08X peb=%08X tls_array=%08X fs_base=%08X\n", setup_.teb, setup_.peb,
                     layout_.tls_array_ea, cpu_.fs_base);
        std::fprintf(out, "guest-exe : \"%s\"（cmdline=\"%s\"）\n", setup_.guest_exe_path.c_str(),
                     setup_.cmdline.c_str());
        std::fprintf(out,
                     "argv      : cmdline_ea=%08X exe_path_ea=%08X env_ea=%08X（env 项 %zu 个：%s）\n",
                     setup_.cmdline_ea, setup_.exe_path_ea, setup_.env_ea,
                     vfs_.guestEnvironment().size(), setup_.environment_ansi.c_str());

        // ---- cpu 初始状态（"起点"读数）+ 宿主侧装配面 ----
        std::fprintf(out,
                     "cpu       : eip=%08X esp=%08X ebp=%08X eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X "
                     "edi=%08X eflags=%08X gs_base=%08X\n",
                     cpu_.eip, cpu_.esp(), cpu_.ebp(), cpu_.eax(), cpu_.ebx(), cpu_.ecx(), cpu_.edx(),
                     cpu_.esi(), cpu_.edi(), cpu_.eflags, cpu_.gs_base);
        std::fprintf(out, "va        : va_cursor=%08X va_limit=%08X vm=%s\n", cpu_.shim.va_cursor,
                     cpu_.shim.va_limit, mem_.safeMode() ? "safe-memory" : "fault-handler");
        // ---- 宿主服务装配面（shim 依赖的是**接口**，不是解释模型 —— D4）----
        // ★ `gpuRaster` 现在是**真读数**（不再是硬 nullptr）：`presenting` 转给 sink 之后，
        //   3D 的像素路径与解释器臂才是同一条（W0 C10 Ⓐ）。
        const GpuRaster* gr = gpuRaster();
        std::fprintf(out,
                     "services  : host=%s ownsCpu=1 hasNestedGuestExecutor=0 | surfaces.ready=%d "
                     "presenter=%s seh=ready decoder=ready gpuRaster=%s"
                     " | clock=%s handles=%s com=%s "
                     "calls=%s windows=%s fonts=%s audio=%s\n",
                     cpu_.host != nullptr ? "self" : "nullptr", surfaces_.ready() ? 1 : 0,
                     presenter_.hasSink() ? "sink" : "headless(no-sink)",
                     gr == nullptr ? "nullptr"
                                   : (gr->rasterActive() ? gr->rasterName() : "inactive（后端建了但没激活）"),
                     "wall",
                     cpu_.handles != nullptr ? "yes" : "no", cpu_.com != nullptr ? "yes" : "no",
                     cpu_.calls != nullptr ? "yes" : "no", cpu_.windows != nullptr ? "yes" : "no",
                     cpu_.fonts != nullptr ? "yes" : "no",
                     // ★ 读**配置**事实（`configuredMode()`），不是 `mode()`：readout 打在 `run()` 的
                     //   `begin()` **之前**，那时 `mode_` 还是 Off ⇒ 用 `mode()` 会让 `--audio sdl`
                     //   的 bring-up 读数写一句假的 `audio=off`（真实的档位 / `device=ok|none`
                     //   在收口的 `audio     :` 一行）。缺省档两者同为 `off` ⇒ 默认档读数逐字不变。
                     cpu_.audio != nullptr ? audioModeName(audio_.configuredMode()) : "no");
        std::fprintf(out, "owner     : cpu.machine=nullptr（AOT 有意没有 Machine）+ cpu.host=this\n");

        // ---- 派发表（构建期生成物的真源是 lift-gen --table）----
        //   ★ 也是**开发者读数**（2026-11-27 用户判据：默认档只留出错 / 警告）⇒ 归 `detail`。
        //     `pin` 核对**失败**是硬错（启动期 exit 2 + stderr），不靠这一行回答。
        std::fprintf(out,
                     "pin       : generated=%s sha256=%s entry=%08X roots=%llu entries=%llu funcs=%llu "
                     "checked=%s\n",
                     generatedPresent() ? "yes" : "NO（构建期缺 clang / 执行源 / closure.txt）",
                     generatedPresent() ? pinnedSha256() : "(none)", pinnedEntry(),
                     static_cast<unsigned long long>(pinnedRoots()),
                     static_cast<unsigned long long>(pinnedEntries()),
                     static_cast<unsigned long long>(pinnedFunctions()),
                     pin_checked_ ? "yes" : "skipped");
    }


    // ---- guest 区域表（"谁占了哪一段"的机械读数；复用既有的 dumpRegions）----
    //   ★ **opt-in**（`--region-log`，缺省关；见 `AotRunOptions::region_log`）：这 28 行是静态布局快照，
    //     一次 run 里不变 ⇒ 属噪音。它与上面的 `detail`（`--log-level info`）是**两个独立门**：
    //     `region_log` 只加这一块，`detail` 只加"装配展开"那一块。
    if (region_log) {
        std::fprintf(out, "regions   : %zu 段（committed=%llu 字节）\n", mem_.regions().size(),
                     static_cast<unsigned long long>(mem_.committedBytes()));
        dumpRegions(mem_.regions(), out);
    }
}

void AotHost::printRunSummary(const AotRunResult& r, std::FILE* out, bool detail) const {
    if (out == nullptr) return;

    // ===== 默认档（`detail == false`）：只留**出错 / 警告（可操作）** + 1 行"怎么结束的" =====
    //   ⛔ 不是"少打几行"：insns / frames / 派发 / 派发表 / arena / present / recovery /
    //   window / skip-logo / synthetic / intrinsics / audio 这些**开发者读数一行都不打**
    //   （2026-11-27 用户判据：默认档只留出错 / 警告与插件自己的日志；开发者读数走 `--log-level info`）。
    //   下面两条是**可操作的警告**（不是 readout）：
    //     · WndProc 不在派发表 ⇒ 第一条消息就会停（用户会看到"输入没反应"而不是一条错）；
    //     · `--skip-logo` 命中但写不进去 ⇒ 开关给了却没生效。
    // ★ **唯一**在默认档也打的收口行：**这一跑是怎么结束的**（停因 / EA / `detail`）。
    //   为什么留这 1 行（用户判据允许"最多留 1 行怎么结束的"）：exit code 只说"成功 / 失败"，
    //   而玩家需要区分"正常关窗 / 到了 --time-limit / 崩在某个 EA"——那是可操作的信息，
    //   且一次 run 只有一行。其余收口 readout 全部只在 `--log-level info`。
    std::fprintf(out, "stop      : %s @ eip=%08X（%s）%s%s\n", aotStopName(r.reason), r.ea,
                 r.ok() ? "正常结局" : "非正常结局", r.detail.empty() ? "" : " — ", r.detail.c_str());
    if (!detail) {
        const std::uint32_t primary = windows_.primaryWindow();
        const HostWindow* win = primary != 0 ? windows_.find(primary) : nullptr;
        const std::uint32_t proc = win != nullptr ? win->proc : 0u;
        if (primary != 0 && proc != 0) {
            std::uint64_t cmp = 0;
            if (lookup(proc, &cmp) == nullptr) {
                std::fprintf(out, "window    : ⚠ guest WndProc=%08X 不在派发表里 ⇒ 第一条消息会停\n",
                             proc);
            }
        }
        if (skip_logo_ && skip_logo_checked_ && !skip_logo_applied_) {
            std::fprintf(out,
                         "skip-logo : ⚠ NOT applied（钩子 @ 004BAA33 命中，但对象/字段不可写；对象=0x%08X）\n",
                         skip_logo_object_);
        }
    } else {
        const DispatchStats ds = dispatchStats();
        std::fprintf(out, "insns     : %llu（`lift_tick` 记账；与解释器同一份 `X86::insns` 语义）\n",
                     static_cast<unsigned long long>(r.insns));
        std::fprintf(out, "frames    : %llu（present 计数；本臂 --max-frames=%u）\n",
                     static_cast<unsigned long long>(r.frames), cpu_.shim.max_frames);
        std::fprintf(out, "dispatch  : total=%llu（lifted=%llu shim=%llu）\n",
                     static_cast<unsigned long long>(r.dispatches),
                     static_cast<unsigned long long>(r.lifted_calls),
                     static_cast<unsigned long long>(r.shim_calls));
        // ★ `comparisons` / `avg_cmp` 只在 `--profile` 开档时记账（`DispatchStats::comparisons_armed`）：
        //   缺省档打 `n/a`，⛔ 不打 `0`（那是"查找 0 次比较"的假读数）。`lookups/hits/misses` 不变
        //   （语义指纹里的 `table misses` 就取这几个数）。
        char cmp_buf[96];
        if (ds.comparisons_armed) {
            const double avg_cmp =
                ds.lookups != 0 ? static_cast<double>(ds.comparisons) / static_cast<double>(ds.lookups)
                                : 0.0;
            std::snprintf(cmp_buf, sizeof(cmp_buf), "comparisons=%llu avg_cmp=%.2f",
                          static_cast<unsigned long long>(ds.comparisons), avg_cmp);
        } else {
            std::snprintf(cmp_buf, sizeof(cmp_buf), "comparisons=n/a avg_cmp=n/a（缺省档未记）");
        }
        std::fprintf(out, "table     : entries=%zu lookups=%llu hits=%llu misses=%llu %s\n", tableSize(),
                     static_cast<unsigned long long>(ds.lookups),
                     static_cast<unsigned long long>(ds.hits),
                     static_cast<unsigned long long>(ds.misses), cmp_buf);
        std::fprintf(out, "region    : arena in_use=%u peak=%u surfaces=%zu heap_blocks=%zu\n",
                     surfaces_.bytesInUse(), surfaces_.peak(), surfaces_.surfaceCount(),
                     heap_.blockCount());
        // ---- 呈现读数（可测性口径；与解释器 `present-tail` / `d3d9.cpp:845-863` 同一组字段名）----
        //   ★ `lastHash` 是**最后一次 Present 的 guest 后缓冲哈希**（`arena.hash(surface)`）；逐帧读数由
        //     `run()` 在每次 Present 之后打印（`[frame] n=… hash=…`）。`[present-readback]` Phase A 起它
        //     **惰性**：`run()` 收口在打印前补算一次（`captureLastFrameHash`）⇒ `0x0` = **没算**（⛔ 不是旧值）。
        //   ★ `sinkFailures` 与解释器同口径（提交失败**不改** guest 的 Present 结果）。
        //   ★ `freezeFailures` / `freezeNs`（`REQ-01M7JAGEKMYWWQPMWGV01Y1FNQ` 的 F1）：**一次性**读数，
        //     只在这一行出现（⛔ 不加周期性输出，`AGENTS.md` §12.8 第 3 条）。`freezeNs` = **最近一次**
        //     `present()` 冻像素副本的墙钟（`PresentBreakdown::freeze_ns`；缺省/没开控制层 = 0）。
        //     这两个字段只回答"冻结这条路有没有真的走通、花了多少"，⛔ 不参与任何语义判据。
        std::fprintf(out,
                     "present   : sink=%s frames=%llu lastHash=0x%016llX sinkFailures=%llu "
                     "freezeFailures=%llu freezeNs=%llu%s\n",
                     presenter_.sink() != nullptr ? presenter_.sink()->describe().c_str() : "headless(no-sink)",
                     static_cast<unsigned long long>(r.frames),
                     static_cast<unsigned long long>(presenter_.lastHash()),
                     static_cast<unsigned long long>(presenter_.sinkFailures()),
                     static_cast<unsigned long long>(presenter_.freezeFailures()),
                     static_cast<unsigned long long>(presentBreakdown().freeze_ns),
                     presenter_.sinkFailures() == 0 ? "" : "（最后一次错误见 stderr）");
        // ★ 默认档就改写 guest 寄存器的那个钩子（T2 §1.2 #14 / `machine.cpp:1369`）的实际结局：
        //   "命中了并置 0" 与 "根本没走到" 是两件事，⛔ 不许只写一行模糊的 `bypassed`。
        if (recovery_bypass_applied_) {
            std::fprintf(out,
                         "recovery  : applied @ 004BA718 (EAX %u -> 0)（--accept-recovery-dialog 默认绕过）\n",
                         recovery_bypass_prior_eax_);
        } else if (recovery_bypass_checked_) {
            std::fprintf(out, "recovery  : 命中但未应用（EAX=%u）\n", recovery_bypass_prior_eax_);
        } else {
            std::fprintf(out, "recovery  : 未命中（本轮没走到 004BA718）\n");
        }
        // ★ guest 窗口 → 它的 WndProc EA，以及**它在不在派发表里**（人类手动测试的第一道门槛）：
        //   所有 SDL 输入（键 / 鼠标 / 关窗）最终都经 `DispatchMessageA` → `GuestCallStack::start(proc)`
        //   ⇒ 下一个块边界 EA 就是 `wndproc`。它不在表里 ⇒ **第一条被派发的消息**就会响亮停
        //   （不是"输入没反应"，而是 `decode-error` + 点名的 EA）。headless 也一样建 guest 窗口。
        {
            const std::uint32_t primary = windows_.primaryWindow();
            const HostWindow* win = primary != 0 ? windows_.find(primary) : nullptr;
            const std::uint32_t proc = win != nullptr ? win->proc : 0u;
            if (primary != 0) {
                std::uint64_t cmp = 0;
                const bool in_table = proc != 0 && lookup(proc, &cmp) != nullptr;
                std::fprintf(out,
                             "window    : guest primary=%08X class=\"%s\" wndproc=%08X in-dispatch-table=%s\n",
                             primary, win != nullptr ? win->class_name.c_str() : "?", proc,
                             proc == 0 ? "n/a（没有 WndProc）" : (in_table ? "yes" : "NO ⇒ 第一条消息会停"));
            } else {
                std::fprintf(out, "window    : guest 还没有窗口（primary=0）\n");
            }
        }
        // ★ `--skip-logo` 的实际结局（**三种形态必须能机械区分**，口径照 `machine.cpp:2733-2742`）：
        //   未要求这个开关时**不打**这一行（与解释器同口径：没有 `skip-logo :` 行 = 本轮没开）。
        if (skip_logo_) {
            if (skip_logo_applied_) {
                std::fprintf(out,
                             "skip-logo : applied @ T_Command=0x%08X (+0x%05X=0；写前 %u；钩子 EA=004BAA33)\n",
                             skip_logo_object_, kSkipLogoFieldOffset, skip_logo_prior_value_);
            } else if (skip_logo_checked_) {
                std::fprintf(out,
                             "skip-logo : NOT applied（钩子 @ 004BAA33 命中，但对象/字段不可写；对象=0x%08X）\n",
                             skip_logo_object_);
            } else {
                std::fprintf(out, "skip-logo : 未命中（run 在 004BAA33 之前结束）\n");
            }
        }
        // ---- 三个合成 EA 的命中数（0 = 本轮根本没撞到；⛔ 不拿"实现了"冒充"跑过了"）----
        std::fprintf(out,
                     "synthetic : 定时器回调=%llu 续延=%llu SEH 派发(raise=%llu handler=%llu unhandled=%llu)\n",
                     static_cast<unsigned long long>(timerFires_),
                     static_cast<unsigned long long>(callResumes_),
                     static_cast<unsigned long long>(seh_.raiseCount()),
                     static_cast<unsigned long long>(seh_.handlerCalls()),
                     static_cast<unsigned long long>(seh_.unhandledCount()));
    }

    // ★ `--dump-frame` 写盘失败的**一次性停机读数**（`REQ-01M73YBCC3DZ05Y9NSDTRKTMMY`）：
    //   **只在 > 0 时打**（这是**错误**，⛔ 不受 `detail` 影响）；第 1 次失败已由 `present.cpp` 打
    //   默认级别 warn（之后只计数）。惰性回读之后，"本帧没回读却要 dump"（needs 门漏了）也记进这个计数。
    if (presenter_.dumpFailures() != 0) {
        std::fprintf(out, "dump      : dumpFailures=%llu（--dump-frame 写盘失败次数；第一次已 warn，见 stderr）\n",
                     static_cast<unsigned long long>(presenter_.dumpFailures()));
    }
    // ---- S2 的**机械判据**：音频**被请求**时打一行与解释器 `machine.cpp:2955` 同形的 `audio     :`
    //   （`mode` / `device=ok|none` / `muted` / fills / pushed / underruns / …）。
    //   ★ 门是 `audioRequested()`（不是 `enabled()`）：`--audio sdl` 打不开设备时 `enabled()` 为假、
    //     但这一跑**要过**音频 ⇒ 摘要仍要如实写 `mode=off device=none muted=1`（⛔ 不许把
    //     "没有输出"静默成"没要音频"）。缺省档（不给任何音频 flag）门为假 ⇒ **一行都不打**。
    //   ★ 音频读数是**开发者读数**（2026-11-27 用户判据）⇒ 还要 `--log-level info`；只给
    //     `--audio sdl` 的默认档一行都不打。判据面见 CTest `aot-audio-wiring`（带 `--log-level info`）。
    //   ★ 一次性输出（不是周期性）：落点与 `--profile` 块同处"stdout 末尾"的前一刻。
    if (detail && audio_.audioRequested()) {
        audio_.dumpSummary(out);
    }
    // ★ Phase-0 四行（`tokenhold / holdhist / waithist / shimmax`）：**只读计量**，与解释器
    //   `Machine::dumpRunSummary` 调的是**同一个** `dumpTokenPhase0`（唯一实现 = 唯一格式）。
    //   开关由 `AotHost::run` 里的 `guestToken().arm(opts.profile)` 决定（`--profile` ∪ `LIFT_AUDIO_TRACE`）
    //   ⇒ **显式 opt-in**：不受 `detail` 影响（用户点名要的读数，⛔ 不静默收掉）。
    //    ★ 行首那一条 `token     : violations=… max-concurrent=…` 与解释器 `Machine::dumpRunSummary`
    //      （`machine.cpp:2732`）**同形**：同一个计数器、同一组字段、`first=` 只在非 0 时出现。
    //      为什么放在 Phase-0 四行**前面**：`token` 是不变式的总账、四行是它的分解 ⇒ 先总账后分项；
    //      位置本身不带语义（两处都在同一个 `if` 里）。
    if (guestToken().phase0Enabled()) {
        const GuestToken& token = guestToken();
        std::fprintf(out, "token     : violations=%llu max-concurrent=%u%s%s\n",
                     static_cast<unsigned long long>(token.violations()), token.maxConcurrent(),
                     token.violations() == 0 ? "" : " first=",
                     token.violations() == 0 ? ""
                                             : (token.firstViolationSite() ? token.firstViolationSite() : "?"));
        dumpTokenPhase0(out, shims_.all());
    }
    // ---- `--profile` 的整块读数（**关档时一行都不打**；开档 = 显式 opt-in ⇒ 不受 `detail` 影响）----
    // ★ 落点为什么在这里：`aot_main.cpp` 在 `printRunSummary` 之后只有 stderr 回声 + `return`，
    //   所以这里就是"stdout 末尾（进程退出前）"。⛔ 不改 main 的打印顺序。
    printProfile(out);
    // ★ `--profile-slow-frames`：**一次性**慢帧表（run 结束时打一次；⛔ 不每帧打）。
    //   关档时 `printSlowFrames` 第一行就返回 ⇒ 不开档一行都不打。
    printSlowFrames(out);
    // ★ 临时诊断（`LIFT_MEMCPY_DIAG=1`）：`_memcpy` 画像 + `rep_bulk` 命中/decline。
    //   一次性输出；缺省档 `on()==false` ⇒ 一行都不打。
    memcpy_diag::print(out);
    // ★ 临时诊断（`LIFT_CALL_DIAG=1`）：库函数候选的 per-function 计数（每窗口一块）。
    //   一次性输出；缺省档 `on()==false` ⇒ 一行都不打。
    call_diag::print(out);
    // ★ 构建期库内建（v1：`_memcpy`）的**一次性**计数（⛔ 不周期打）。表为空 ⇒ 一行都不打；
    //   表非空但一条都没走（例如这轮没进到那个 EA）⇒ 三个 0 也打（否则"没生效"与"没调用"分不清）。
    //   ★ 开发者读数 ⇒ 只给 `--log-level info`（与启动那条 `intrinsics: … table=` 同一个门）。
    if (detail && pinnedIntrinsicCount() != 0) {
        const lift::IntrinsicCounters c = lift::intrinsicCounters();
        std::fprintf(out, "intrinsics: fast=%llu decline=%llu disabled=%llu\n",
                     static_cast<unsigned long long>(c.fast),
                     static_cast<unsigned long long>(c.decline),
                     static_cast<unsigned long long>(c.disabled));
    }
}

// =====================================================================================
// 采样式内部 profile（`--profile`）—— 取时 / 频率校准 / 表块
//
// ★ 采样点在 `aot_run.cpp`（派发 / shim / 每帧时间戳）；本文件只负责**取时原语**、
//   **机器频率校准**与**表块打印**。三条硬口径见 `aot_host.h` 的 `AotProfile`。
// =====================================================================================

namespace {

std::uint64_t readTsc() {
#if defined(LIFT_AOT_HAS_TSC)
#if defined(_MSC_VER) || (defined(__clang__) && defined(_WIN32))
    return static_cast<std::uint64_t>(__rdtsc());
#else
    return static_cast<std::uint64_t>(__builtin_ia32_rdtsc());
#endif
#else
    return 0;
#endif
}

std::uint64_t steadyNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

bool platformHasTsc() {
#if defined(LIFT_AOT_HAS_TSC)
    return readTsc() != 0;
#else
    return false;
#endif
}

// ★ `lift_one` 采样钩子的转发点：钩子是 C 风格的函数指针（`lift_abi.h`），账本在宿主实例上
//   ⇒ 用"当前开档的宿主"这**一个**指针转接（单进程里同时只有一台宿主在跑）。
//   ★ 关档时钩子恒 `nullptr`（`stopProfile()` 摘掉）⇒ `lift_one` 里只多一次指针判空。
AotHost* g_profile_host = nullptr;
void aotLiftOneProfileHook(const lift::LiftOneProfileSample& s) {
    if (g_profile_host != nullptr) g_profile_host->noteProfileLiftOne(s);
}

}  // namespace

std::uint64_t profileTicks() {
#if defined(LIFT_AOT_HAS_TSC)
    return readTsc();
#else
    return steadyNs();
#endif
}

double profileProcessMs() {
#if defined(_WIN32)
    // ★ Windows：真"自进程启动"（`GetProcessTimes` 的 creation 时间），不用静态初始化时刻近似。
    FILETIME create{}, exit_t{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &create, &exit_t, &kernel, &user)) {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER a{};
        ULARGE_INTEGER b{};
        a.LowPart = create.dwLowDateTime;
        a.HighPart = create.dwHighDateTime;
        b.LowPart = now.dwLowDateTime;
        b.HighPart = now.dwHighDateTime;
        if (b.QuadPart >= a.QuadPart) {
            return static_cast<double>(b.QuadPart - a.QuadPart) / 10000.0;  // 100ns → ms
        }
    }
#endif
    // 回退（非 Windows / 取不到进程时间）：程序静态初始化时刻 ⇒ 少算了进程加载那几 ms（登记在这里）。
    static const auto origin = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - origin).count();
}

double profileTicksToMs(std::uint64_t ticks, const AotProfile& p) {
    if (ticks == 0) return 0.0;
    if (p.tsc_ok && p.ghz > 0.0) {
        // ghz = ticks/ns ⇒ ns = ticks / ghz ⇒ ms = ns / 1e6
        return static_cast<double>(ticks) / p.ghz / 1.0e6;
    }
    return static_cast<double>(ticks) / 1.0e6;  // 非 TSC 平台：ticks 就是 ns
}

void AotHost::beginProfile() {
    prof_.reset();
    prof_.on = true;
    prof_.tsc_ok = platformHasTsc();
    // ---- 仪器自校准：一次 `steady_clock::now()` 的耗时（ns）----
    // ★ 为什么必须量它：`lift_one` 的采样区间 = 入口 t0（**不含**）到出口 t1（**部分含**）；
    //   如果一次取时就有几百 ns，那"lift_one 平均 5xx ns"里就有仪器的一份 ⇒ 读数会误导。
    //   ⛔ 不猜这个数，也不拿它去"修正"测量值（只报出来，让读的人自己判断）。
    {
        constexpr int kReps = 2000;
        volatile std::int64_t sink_ns = 0;
        const auto c0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kReps; ++i) {
            const auto t = std::chrono::steady_clock::now();
            // 防优化：把某一位折进一个 volatile（否则整轮可能被删）
            sink_ns ^= static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
        }
        const auto c1 = std::chrono::steady_clock::now();
        const double total_ns = std::chrono::duration<double, std::nano>(c1 - c0).count();
        prof_.clock_read_ns = total_ns / static_cast<double>(kReps);
    }
    // ★ `lift_one` 的采样钩子（唯一接缝；契约见 `runtime/lift_abi.h`）。关档时它保持 nullptr。
    g_profile_host = this;
    lift::liftOneProfileHook = &aotLiftOneProfileHook;
    if (!prof_.tsc_ok) {
        // 非 x86（例如 macOS arm64）：没有 TSC ⇒ dispatch 区间用 steady_clock ns。
        // ★ 不编一个 GHz 出来：那会让 cycles→ms 的换算变成假的（`freq:` 行会明说不可用）。
        return;
    }
    // ---- 机器频率校准：**已知长度的忙循环**，实测 ticks / 墙钟 ⇒ GHz ----
    // ★ 为什么必须实测（⛔ 不硬编码 3.2 GHz）：TSC 是**恒定速率**（通常 = 标称基频），不同机器
    //   基频不同（也可能被 BIOS / 虚拟化改），硬编码的系数会让 ms 读数整体偏几个百分点到一倍。
    // ★ 循环体是**依赖链**（每次乘加依赖上一次）⇒ 不会被乱序 / 向量化；`volatile` sink 防整段被删。
    volatile std::uint64_t sink = 0;
    auto bench = [&](std::uint64_t iters) {
        std::uint64_t x = 0x9E3779B97F4A7C15ull;
        const auto t0 = std::chrono::steady_clock::now();
        const std::uint64_t c0 = profileTicks();
        for (std::uint64_t i = 0; i < iters; ++i) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
        }
        const std::uint64_t c1 = profileTicks();
        const auto t1 = std::chrono::steady_clock::now();
        sink = sink ^ x;
        return std::make_pair(c1 - c0,
                              std::chrono::duration<double, std::milli>(t1 - t0).count());
    };
    // pilot：先跑一小段，把主测量定在 ~30 ms（快慢机器差一个数量级也不会跑成 0 或跑 10 s）。
    const auto pilot = bench(200000ull);
    const double iters_per_ms = pilot.second > 0.0 ? 200000.0 / pilot.second : 5.0e5;
    std::uint64_t iters = static_cast<std::uint64_t>(iters_per_ms * 30.0);
    if (iters < 200000ull) iters = 200000ull;
    // 3 轮取**最大** ticks/ms：被调度抢占的那几轮只会偏小（取最大 = 最接近真实频率）。
    double best_ghz = 0.0;
    for (int round = 0; round < 3; ++round) {
        const auto r = bench(iters);
        if (r.first == 0 || r.second <= 0.0) continue;
        const double ghz = static_cast<double>(r.first) / (r.second * 1.0e6);
        if (ghz > best_ghz) {
            best_ghz = ghz;
            prof_.calib_ticks = r.first;
            prof_.calib_ms = r.second;
        }
    }
    prof_.ghz = best_ghz;
}

void AotHost::stopProfile() {
    // ★ 关档：摘掉 `lift_one` 的钩子（`nullptr` ⇒ 热路径上只剩一次指针判空）并清空账。
    //   为什么要显式摘：`run()` 可以被调多轮，上一轮开着、这一轮不带 `--profile` 时
    //   ⛔ 不许它继续在后台采样（那会让"关档"不再是关档）。
    lift::liftOneProfileHook = nullptr;
    g_profile_host = nullptr;
    prof_.reset();
}

void AotHost::noteProfileLiftOne(const lift::LiftOneProfileSample& s) {
    if (!prof_.on) return;
    AotProfile::LiftOneStat& st = prof_.liftone_by_ea[s.ea];
    ++st.n;
    st.ns += s.ns;
    // 一个 EA 只对应一条静态指令 ⇒ op 名只有第一次需要记（SMC 改了字节也不会改 EA→op 的用途）。
    if (st.op.empty()) st.op = (s.op != nullptr && *s.op != '\0') ? s.op : "?";
    ++prof_.liftone_samples;
    // 四桶（A/B/C/D）：按 op 累计 + 全体累计。⛔ 只用于归因。
    auto& phase = prof_.liftone_phase_by_op[st.op];
    for (int i = 0; i < AotProfile::kLiftOnePhaseCount; ++i) {
        phase[static_cast<std::size_t>(i)] += s.phase_ns[i];
        prof_.liftone_phase_total[static_cast<std::size_t>(i)] += s.phase_ns[i];
    }
}

void AotHost::printProfile(std::FILE* out) const {
    if (out == nullptr || !prof_.on) return;  // ★ 关档：连表头都不打（关/开两档的 stdout 只差这一块）
    const AotProfile& p = prof_;

    std::fprintf(out,
                 "\n=== profile (sampling: dispatch 1/%llu, lift_one 1/%llu, shim 1/%llu-per-shim) ===\n",
                 static_cast<unsigned long long>(AotProfile::kDispatchStride),
                 static_cast<unsigned long long>(AotProfile::kLiftOneStride),
                 static_cast<unsigned long long>(AotProfile::kShimStride));

    // ---- freq：机器频率校准（把 cycles 换算成 ms 的唯一系数）----
    if (p.tsc_ok && p.ghz > 0.0) {
        std::fprintf(out,
                     "freq: ~%.2f GHz（TSC 实测：依赖链忙循环 %.1f ms 走 %llu ticks；TSC 恒定速率 ⇒ "
                     "核心降频不影响 cycles→ms）\n",
                     p.ghz, p.calib_ms,
                     static_cast<unsigned long long>(p.calib_ticks));
    } else {
        std::fprintf(out,
                     "freq: 不可用（这台平台读不到 TSC ⇒ dispatch 区间用 steady_clock ns；"
                     "⛔ 不硬编码 GHz）\n");
    }
    // 仪器自身的量级（读数时必须摆出来：`lift_one` 的平均 ns 里有一部分是取时的尾巴）
    std::fprintf(out,
                 "freq: 取时开销 %.1f ns/次（steady_clock，2000 次实测；lift_one 的采样区间**不含**入口取时、"
                 "出口取时的尾巴会计进测量值）\n",
                 p.clock_read_ns);

    // ---- frame-ms：每帧一行已在 Present 时打过，这里只报行数（⛔ 不重复刷屏）----
    std::fprintf(out,
                 "frame-ms: %llu 行（每帧一行，Present 时打；t_ms = 自进程启动到该帧 present 完成）\n",
                 static_cast<unsigned long long>(p.frame_lines));

    // ---- ② 派发归因：自计时间隔法（权重记到该次采样的入口 EA 上）----
    {
        struct Row {
            std::uint32_t ea;
            std::uint64_t hits;
            std::uint64_t ticks;
        };
        std::vector<Row> rows;
        rows.reserve(p.dispatch_by_ea.size());
        for (const auto& kv : p.dispatch_by_ea) rows.push_back({kv.first, kv.second.first, kv.second.second});
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.ticks != b.ticks) return a.ticks > b.ticks;
            return a.ea < b.ea;
        });
        const double total_ms = profileTicksToMs(p.dispatch_total, p);
        const double total_pct_of_wall = p.wall_ms > 0.0 ? 100.0 * total_ms / p.wall_ms : 0.0;
        std::fprintf(out,
                     "dispatch-top: 自计时间隔法 %llu 个样本 / %llu 次派发；采样区间合计 %.1f ms"
                     "（占覆盖到的墙钟 %.1f%%；未覆盖 = 收尾那不到一个 stride）\n",
                     static_cast<unsigned long long>(p.dispatch_samples),
                     static_cast<unsigned long long>(p.dispatch_calls), total_ms, total_pct_of_wall);
        constexpr std::size_t kTop = 20;
        const std::size_t shown = std::min(kTop, rows.size());
        for (std::size_t i = 0; i < shown; ++i) {
            const double ms = profileTicksToMs(rows[i].ticks, p);
            const double pct = p.dispatch_total != 0
                                   ? 100.0 * static_cast<double>(rows[i].ticks) /
                                         static_cast<double>(p.dispatch_total)
                                   : 0.0;
            std::fprintf(out, "dispatch-top: %2zu. ea=%08X 采样=%llu ms=%9.3f 占比=%5.1f%%\n", i + 1,
                         rows[i].ea, static_cast<unsigned long long>(rows[i].hits), ms, pct);
        }
        if (rows.size() > shown) {
            std::fprintf(out, "dispatch-top: 另有 %zu 个 EA（未展开）\n", rows.size() - shown);
        }
        if (rows.empty()) std::fprintf(out, "dispatch-top: （没有样本）\n");
    }

    // ---- ③ lift_one 归因（每 1/1024 采样一次单次耗时，按 op 类别 + EA 聚合）----
    //   ★ 输出两块：按 op 汇总（`op / 次数 / 平均 ns / 总 ns`）+ 按 (op, EA) 的 top-20。
    //     后者回答"热点落在那条指令上"，前者回答"开销集中在哪类 op"。
    if (p.liftone_by_ea.empty()) {
        std::fprintf(out,
                     "liftone: （没有样本 —— 本构建里 `lift_one` 一次都没被调用，或采样钩子没接上）\n");
    } else {
        struct OpRow {
            std::string op;
            std::uint64_t n;
            std::uint64_t ns;
        };
        std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> by_op;
        for (const auto& kv : p.liftone_by_ea) {
            auto& slot = by_op[kv.second.op];
            slot.first += kv.second.n;
            slot.second += kv.second.ns;
        }
        std::vector<OpRow> ops;
        ops.reserve(by_op.size());
        for (const auto& kv : by_op) ops.push_back({kv.first, kv.second.first, kv.second.second});
        std::sort(ops.begin(), ops.end(), [](const OpRow& a, const OpRow& b) {
            if (a.ns != b.ns) return a.ns > b.ns;
            return a.op < b.op;
        });
        std::fprintf(out,
                     "liftone: 每 %llu 次采样一次单次耗时（%llu 个样本 / %zu 个 EA）—— 按 op 类别汇总：\n",
                     static_cast<unsigned long long>(AotProfile::kLiftOneStride),
                     static_cast<unsigned long long>(p.liftone_samples), p.liftone_by_ea.size());
        for (const OpRow& o : ops) {
            const double avg = o.n != 0 ? static_cast<double>(o.ns) / static_cast<double>(o.n) : 0.0;
            std::fprintf(out, "liftone: op=%-10s 次数=%-7llu 平均ns=%8.1f 总ns=%12llu\n", o.op.c_str(),
                         static_cast<unsigned long long>(o.n), avg,
                         static_cast<unsigned long long>(o.ns));
        }
        // top-20 (op, EA)
        struct EaRow {
            std::uint32_t ea;
            std::uint64_t n;
            std::uint64_t ns;
            std::string op;
        };
        std::vector<EaRow> eas;
        eas.reserve(p.liftone_by_ea.size());
        for (const auto& kv : p.liftone_by_ea) {
            eas.push_back({kv.first, kv.second.n, kv.second.ns, kv.second.op});
        }
        std::sort(eas.begin(), eas.end(), [](const EaRow& a, const EaRow& b) {
            if (a.ns != b.ns) return a.ns > b.ns;
            return a.ea < b.ea;
        });
        constexpr std::size_t kTop = 20;
        const std::size_t shown = std::min(kTop, eas.size());
        std::fprintf(out, "liftone: 按 (op, EA) 的 top-%zu（总 ns 降序）：\n", shown);
        for (std::size_t i = 0; i < shown; ++i) {
            const double avg = eas[i].n != 0 ? static_cast<double>(eas[i].ns) / static_cast<double>(eas[i].n)
                                             : 0.0;
            std::fprintf(out, "liftone: %2zu. op=%-10s ea=%08X 次数=%-5llu 平均ns=%8.1f 总ns=%llu\n",
                         i + 1, eas[i].op.c_str(), eas[i].ea,
                         static_cast<unsigned long long>(eas[i].n), avg,
                         static_cast<unsigned long long>(eas[i].ns));
        }
        if (eas.size() > shown) std::fprintf(out, "liftone: 另有 %zu 个 EA（未展开）\n", eas.size() - shown);

        // ---- ③b 四桶归因：A 换入 / B 解码 / C 执行 / D 回读 ----
        //   ★ 为什么要有它（父级任务书"任务 1"）：`lift_one` 的**每种 op 的下限都极高**
        //     （`Mov` 517 ns / `Div` 496 ns，本该几十 ns），而那个地板与 op 无关 ⇒ 只能靠"分段的读数"
        //     定位，不能按猜测动手。四段之和 == 该次采样的总 ns（同一条取时链）。
        //   ⚠ 每段各含**一次**取时（`freq: 取时开销` 那一行的 ns）⇒ 短段的读数有那个量级的地板。
        //   ⛔ 这些数字只用于归因；⛔ 不进任何判据、⛔ 不改变任何语义。
        {
            static const char* const kPhaseName[AotProfile::kLiftOnePhaseCount] = {
                "A 换入(liftRegsToCpu+eip)",
                "B 解码(Insn/why+decode)",
                "C 执行(noteInsn+execInsn)",
                "D 回读(liftCpuToRegs+eip)",
            };
            const double samples = static_cast<double>(p.liftone_samples);
            std::uint64_t total_ns = 0;
            for (const auto& kv : p.liftone_by_ea) total_ns += kv.second.ns;
            std::fprintf(out,
                         "liftone-phase: 四桶归因（%llu 个样本；每段含一次取时 ≈ %.1f ns 的地板；"
                         "四段之和 = 采样区间）\n",
                         static_cast<unsigned long long>(p.liftone_samples), p.clock_read_ns);
            for (int i = 0; i < AotProfile::kLiftOnePhaseCount; ++i) {
                const std::uint64_t ns = p.liftone_phase_total[static_cast<std::size_t>(i)];
                const double avg = samples > 0.0 ? static_cast<double>(ns) / samples : 0.0;
                const double pct = total_ns > 0 ? 100.0 * static_cast<double>(ns) / static_cast<double>(total_ns)
                                                : 0.0;
                std::fprintf(out, "liftone-phase: %-28s 平均ns=%8.1f 占比=%5.1f%% 总ns=%12llu\n",
                             kPhaseName[i], avg, pct, static_cast<unsigned long long>(ns));
            }
            // 按 op 的四段平均（取"总 ns 降序"的前若干个 op，再补一行全体）——这是"x87 的地板在哪一段"的答案。
            struct PhaseRow {
                std::string op;
                std::uint64_t n;
                std::array<std::uint64_t, AotProfile::kLiftOnePhaseCount> ns;
            };
            std::map<std::string, std::uint64_t> op_n;
            for (const auto& kv : p.liftone_by_ea) op_n[kv.second.op] += kv.second.n;
            std::vector<PhaseRow> rows;
            rows.reserve(p.liftone_phase_by_op.size());
            for (const auto& kv : p.liftone_phase_by_op) {
                rows.push_back({kv.first, op_n[kv.first], kv.second});
            }
            std::sort(rows.begin(), rows.end(), [](const PhaseRow& a, const PhaseRow& b) {
                std::uint64_t ta = 0, tb = 0;
                for (int i = 0; i < AotProfile::kLiftOnePhaseCount; ++i) {
                    ta += a.ns[static_cast<std::size_t>(i)];
                    tb += b.ns[static_cast<std::size_t>(i)];
                }
                if (ta != tb) return ta > tb;
                return a.op < b.op;
            });
            constexpr std::size_t kPhaseTop = 12;
            const std::size_t phase_shown = std::min(kPhaseTop, rows.size());
            std::fprintf(out, "liftone-phase: 按 op 的四段平均 ns（总 ns 降序，前 %zu 个；A/B/C/D）\n",
                         phase_shown);
            for (std::size_t i = 0; i < phase_shown; ++i) {
                const PhaseRow& r = rows[i];
                const double n = r.n != 0 ? static_cast<double>(r.n) : 1.0;
                std::uint64_t sum = 0;
                for (int k = 0; k < AotProfile::kLiftOnePhaseCount; ++k) sum += r.ns[static_cast<std::size_t>(k)];
                std::fprintf(out,
                             "liftone-phase: op=%-10s 次数=%-6llu 平均ns=%7.1f  A=%6.1f B=%6.1f C=%6.1f D=%6.1f\n",
                             r.op.c_str(), static_cast<unsigned long long>(r.n),
                             static_cast<double>(sum) / n, static_cast<double>(r.ns[0]) / n,
                             static_cast<double>(r.ns[1]) / n, static_cast<double>(r.ns[2]) / n,
                             static_cast<double>(r.ns[3]) / n);
            }
        }

        // ---- ③c 解码缓存命中率（`lift_one` 的 B 桶由它决定；只作读数）----
        //   ★ `invalidated` = "EA 相同但字节变了" ⇒ **SMC 自愈真的在工作**的直接证据
        //     （AOT 臂 `lift_note_write` 是 no-op，所以这条路径是唯一的防线）。
        //   ⛔ 这些数字只作读数，不进任何判据。
        {
            const lift::DecodeCache& c = lift::liftOneDecodeCache();
            const std::uint64_t lookups = c.hits + c.misses + c.invalidated;
            const double rate = lookups != 0 ? 100.0 * static_cast<double>(c.hits) / static_cast<double>(lookups)
                                             : 0.0;
            std::fprintf(out,
                         "decode-cache: 槽=%zu 命中=%llu 未命中=%llu 字节被改(重解)=%llu 不可缓存=%llu "
                         "命中率=%.1f%%（%llu 次查找）\n",
                         lift::kDecodeCacheSlots, static_cast<unsigned long long>(c.hits),
                         static_cast<unsigned long long>(c.misses),
                         static_cast<unsigned long long>(c.invalidated),
                         static_cast<unsigned long long>(c.uncached), rate,
                         static_cast<unsigned long long>(lookups));
        }
    }

    // ---- ④ shim 归因（**总量**口径，与解释器 `--profile` 的 `shimmax` 不同）----
    //   ★ `shimmax` = 最慢一次；这里要的是"这个 shim 一共花了多少" ⇒ 采样 ns 之和 + 按总调用数折算。
    //   ★ 总调用数**不逐次记账**：直接读 `ShimRegistry` 的 `call_count`（`invoke` 本来就在记）。
    {
        std::map<std::string, std::uint64_t> call_counts;
        for (const ShimDef& d : shims_.all()) {
            call_counts[d.module + "!" + d.name] = d.call_count;
        }
        struct Row {
            std::string key;
            std::uint64_t n;
            std::uint64_t ticks;
            std::uint64_t calls;
        };
        std::vector<Row> rows;
        rows.reserve(p.shim_by_name.size());
        for (const auto& kv : p.shim_by_name) {
            const auto it = call_counts.find(kv.first);
            rows.push_back({kv.first, kv.second.n, kv.second.ticks,
                            it != call_counts.end() ? it->second : 0ull});
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.ticks != b.ticks) return a.ticks > b.ticks;
            return a.key < b.key;
        });
        constexpr std::size_t kTop = 15;
        const std::size_t shown = std::min(kTop, rows.size());
        std::fprintf(out,
                     "shim: 每个 shim **各自**每 %llu 次调用采样一次（top-%zu，按采样到的总耗时降序；"
                     "`总ns` = 采样 ns 之和，`折算ms` = 平均 × 总调用数）\n",
                     static_cast<unsigned long long>(AotProfile::kShimStride), shown);
        for (std::size_t i = 0; i < shown; ++i) {
            const double avg_ns = rows[i].n != 0 ? profileTicksToMs(rows[i].ticks, p) * 1.0e6 /
                                                        static_cast<double>(rows[i].n)
                                                  : 0.0;
            const double total_ns = profileTicksToMs(rows[i].ticks, p) * 1.0e6;
            const double est_ms = rows[i].calls != 0 ? avg_ns * static_cast<double>(rows[i].calls) / 1.0e6
                                                     : 0.0;
            // ★ 样本少（调用数不到两个 stride）时 `折算ms` 靠 1 个样本外推 ⇒ 必须当场标出来，
            //    ⛔ 不让它冒充"这个 shim 一共花了多少"。
            const char* warn = rows[i].n <= 1 ? " ⚠ 仅 1 个样本（调用数 < stride）⇒ 折算值不可当真"
                                              : "";
            std::fprintf(out,
                         "shim: %2zu. %-42s 样本=%-5llu 调用=%-8llu 平均ns=%10.1f 总ns=%12.0f 折算ms=%9.3f%s\n",
                         i + 1, rows[i].key.c_str(), static_cast<unsigned long long>(rows[i].n),
                         static_cast<unsigned long long>(rows[i].calls), avg_ns, total_ns, est_ms, warn);
        }
        if (rows.empty()) std::fprintf(out, "shim: （没有样本）\n");
        if (rows.size() > shown) std::fprintf(out, "shim: 另有 %zu 个 shim（未展开）\n", rows.size() - shown);
    }
    std::fflush(out);
}

// =====================================================================================
// 按帧的 shim 分桶（`--profile-slow-frames <ms>`；缺省关）
//
// ★ 为什么打**一张**表而不是每帧两行：`AGENTS.md` §12.8 第 3 条——周期性输出默认禁；
//   一次 run 一张表 = 一次性输出（与 `--profile` 的整块读数同一个落点、同一条口径）。
// ★ 数据面（墙钟 / insns / shim 时间）在 `aot_run.cpp` 的 `frame_prof_.endFrame(...)` 收口；
//   这里只**打印**（唯一一份格式）。⛔ 不要在这里重新取时 / 重新分桶。
// =====================================================================================
std::string AotHost::shimDisplayName(std::uint32_t id) const {
    const ShimDef* def = shims_.byId(id);
    return def != nullptr ? (def->module + "!" + def->name) : std::string("(unknown)");
}

void AotHost::printSlowFrames(std::FILE* out) const {
    const FrameBucketProf& f = frame_prof_;
    if (out == nullptr || !f.on) return;  // ★ 关档：一行都不打（默认档逐字节不变）
    std::fprintf(out,
                 "\n=== profile slow-frames: 阈值 ≥%llu ms（按帧的 shim 分桶；每帧 top-%zu）===\n",
                 static_cast<unsigned long long>(f.threshold_ms),
                 FrameBucketProf::kTopN);
    std::fprintf(out,
                 "slow-frames: frames=%llu slow=%llu listed=%zu dropped=%llu（上限 %zu）\n",
                 static_cast<unsigned long long>(f.frames_seen),
                 static_cast<unsigned long long>(f.slow_frames), f.rows.size(),
                 static_cast<unsigned long long>(f.frames_dropped),
                 FrameBucketProf::kMaxSlowFrames);
    std::fprintf(out,
                 "slow-frames: 口径：wall_ms = 该帧 `frame-ms` 的 t_ms 差（同一把尺）· insns = 该帧"
                 " guest 指令数 · shim_ms = 该帧**主线程** shim 的精确墙钟（音频 feeder 的调用不计；"
                 "每次调用含一次取时的仪器税，未扣）\n");
    if (f.rows.empty()) {
        std::fprintf(out, "slow-frames: （没有 ≥%llu ms 的帧）\n",
                     static_cast<unsigned long long>(f.threshold_ms));
        std::fflush(out);
        return;
    }
    for (const SlowFrameRow& r : f.rows) {
        std::fprintf(out,
                     "slow-frame: n=%llu wall_ms=%9.3f insns=%llu shim_ms=%9.3f shim_calls=%llu"
                     " presents=%llu\n",
                     static_cast<unsigned long long>(r.frame), r.wall_ms,
                     static_cast<unsigned long long>(r.insns), static_cast<double>(r.shim_ns) / 1.0e6,
                     static_cast<unsigned long long>(r.shim_calls),
                     static_cast<unsigned long long>(r.presents));
        for (std::size_t i = 0; i < r.top.size(); ++i) {
            std::fprintf(out, "slow-frame:   %2zu. %-42s ms=%9.3f calls=%llu\n", i + 1,
                         r.top[i].name.c_str(), static_cast<double>(r.top[i].ns) / 1.0e6,
                         static_cast<unsigned long long>(r.top[i].calls));
        }
        if (r.shim_rows > r.top.size()) {
            std::fprintf(out, "slow-frame:   另有 %zu 个 shim（未展开）\n", r.shim_rows - r.top.size());
        }
    }
    if (f.frames_dropped != 0) {
        std::fprintf(out, "slow-frames: 另有 %llu 帧未列出（上限 %zu；⛔ 不是没有慢帧）\n",
                     static_cast<unsigned long long>(f.frames_dropped),
                     FrameBucketProf::kMaxSlowFrames);
    }
    std::fflush(out);
}

// =====================================================================================
// 控制层（S2/S3/S4 + typed global）的**主线程取数口**：全部在主线程被调（`Control::serve` 在汇点里跑）
//   ★ 这里只**读**宿主 / guest；唯一的写 = `controlGlobalSet`（一个 global 数据槽，经
//     `runtime/script_globals.h`）。⛔ 不改 `cpu_`、⛔ 不进执行循环。
// =====================================================================================
bool AotHost::presentGateThunk(void* user) {
    auto* self = static_cast<AotHost*>(user);
    Control* control = self->control_;
    if (control == nullptr) return false;
    // 没有命令、也没武装谓词 ⇒ 一次判空就回（窗口档每帧都会走到这里）。
    if (!control->pending() && !control->watching()) return false;
    Control::Context ctx;
    ctx.clock = &self->clock_;
    ctx.windows = &self->windows_;
    ctx.stop_requested = &self->control_stop_;
    ctx.paused_at = "present";
    ctx.host = self;
    // 返回 true = 已置停（`quit` / idle / orphan）；块边界读 `control_stop_` 后收口。
    return control->serve(ctx);
}

void AotHost::fillControlSnapshot(ControlSnapshot& out) {
    out = ControlSnapshot{};
    out.insns = cpu_.insns;
    out.frame = presenter_.frameCount();
    // ★ 惰性 hash（`[present-readback]` Phase A）：**读 `last_hash` 之前**补一次。
    //   `--control-root` 已开 freeze ⇒ 命中冻结副本路径（零 GPU 等待、⛔ 不碰 guest 后缓冲）；
    //   还没 Present 过 ⇒ `hasLastFrame()` 为假、保持 0、不 warn（不是错误）。
    //   控制层每次 `sample` / `screenshot` 才走到这里，⛔ 不是每帧。
    if (presenter_.hasLastFrame()) {
        std::string hash_err;
        (void)presenter_.captureLastFrameHash(gpuRaster(), &hash_err);
    }
    out.last_hash = presenter_.lastHash();
    out.present = present_state_;
    out.guest_ms = clock_.nowMs();
    out.paused_total_ms = clock_.pausedCommittedMs();
    // ★ 位置 + 身份 + script_sha 的**唯一**填充点（位置公式只在 engine_marker 的
    //   `readScriptPositionSample`；本文件不重写公式）。
    fillControlScriptSample(mem_, control_marker_, vfs_, &out.script);
    // `paused` / `paused_at` / `state_seq` 由 `Control` 填（状态机在控制层，不在宿主）。
}

ControlShotStatus AotHost::controlScreenshot(const std::string& path, ControlScreenshot& out,
                                              std::string* err) {
    // ★ **三态**（判据 5 的客户端契约）：`io` 只留给"有像素但写不出去"；一次 Present 都没有 ⇒
    //   数据**没到** ⇒ `NoFrameYet`（协议层 `unavailable`），⛔ 不冒充 IO 失败。
    if (!presenter_.hasLastFrame()) {
        if (err != nullptr) *err = "no frame presented yet";
        return ControlShotStatus::NoFrameYet;
    }
    // ★ 写的是 `present()` **当场冻结的那一帧**（`dumpLastFrame`）：控制层启用时是像素副本
    //   （`REQ-01M7JAGEKMYWWQPMWGV01Y1FNQ`）；没冻结 / 冻结失败时退回写现场（= 修复前的行为）。
    //   ⛔ 不是窗口回读 —— 两者差叠层 / letterbox / 光标。`hash_still_matches` = 在**实际写出的
    //   那份字节**上算 hash 与记录的 `last_surface_hash_` 比（副本路径下恒 true：这张 BMP 就是
    //   presenter 记录的那一帧），`dumpLastFrame` 自己算。
    bool hash_still_matches = false;
    std::string dump_err;
    if (!presenter_.dumpLastFrame(path, &hash_still_matches, &dump_err)) {
        if (err != nullptr) *err = dump_err.empty() ? "dumpLastFrame 失败（没有给出原因）" : dump_err;
        return ControlShotStatus::IoFailed;
    }
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        // ★ 写成功但读不到大小：照实报 io（⛔ 不静默回 0 当"空文件"）。
        if (err != nullptr) {
            *err = "截图已写但读不到文件大小：" + path + "（" + ec.message() + "）";
        }
        return ControlShotStatus::IoFailed;
    }
    out.path = path;
    out.bytes = static_cast<std::uint64_t>(size);
    out.frame = presenter_.frameCount();
    out.hash_still_matches = hash_still_matches;
    out.source = "guest-backbuffer";
    return ControlShotStatus::Ok;
}

void AotHost::controlOnResume() {
    // ★ S4 / Q1b（§3.10）：暂停期间没有读点，真人在暂停中松开的按钮**不会**被送给设备
    //   （`pumpOneEvent` 的 stale 闸门会丢弃早于恢复时刻的 OS 事件）⇒ 电平会卡在按下。
    //   这里在设置陈旧截点**之前**清 **OS-owned** 电平并逐条 post up（Program 的 hold 保留，
    //   §3.2 #20）⇒ guest 看到的是「正常的 up」，而不是永久按下。
    input::dropOsLevels(windows_, static_cast<std::uint32_t>(clock_.nowMs()));
    presenter_.noteControlResume();
}

// =====================================================================================
// `global-get` / `global-set`（控制层的主线程执行口）
//   ★ 唯一实现在 `runtime/script_globals.cpp`（寻址 / ENC-DEC / 类型编码）；这里只转发 `mem_`。
//   ★ 写的是**一个 guest 数据槽**：⛔ 不碰 `cpu_`，下一块从内存重新加载。
// =====================================================================================
GlobalError AotHost::controlGlobalGet(GlobalKind kind, std::uint32_t id, GlobalValue* out) {
    GlobalError err = GlobalError::None;
    if (!readGlobal(mem_, id, kind, out, &err)) return err;
    return GlobalError::None;
}

GlobalError AotHost::controlGlobalSet(GlobalKind kind, std::uint32_t id, const GlobalValue& value) {
    GlobalError err = GlobalError::None;
    if (!writeGlobal(mem_, id, kind, value, &err)) return err;
    return GlobalError::None;
}

// =====================================================================================
// 窗口 profile（`profile_begin/end` 驱动；桶与闭式在 `aot/window_prof.h`，这里只做
// "开/关 + 快照 + 打表"。⛔ 零新 CLI：不开窗 ⇒ 一个取时都不做、一行都不打。
// =====================================================================================
bool AotHost::beginWindow(const char* label) {
    // ★ 状态位先判：已开 ⇒ false 且**不 reset**（`AotWindowProf::beginWindow()` 的唯一实现）。
    if (!win_.beginWindow()) return false;
    win_.label = (label != nullptr && *label != '\0') ? label : "(unnamed)";
    // ★ 临时诊断（`LIFT_MEMCPY_DIAG=1`）：只对目标窗口（缺省 `move11`）开始收集；缺省档 no-op。
    memcpy_diag::beginWindow(win_.label.c_str());
    // ★ 临时诊断（`LIFT_CALL_DIAG=1`）：每个窗口都开一段计数（缺省档 / env 未设 ⇒ no-op）。
    call_diag::beginWindow(win_.label.c_str());
    win_.t_open_ns = windowNowNs();
    win_.insns_open = cpu_.insns;
    win_.frames_open = presenter_.frameCount();
    win_.liftdyn_open = ::liftDynCalls;
    // ★ 窗口级 EA 派发归因（`--profile` 开档才做）：快照既有采样表，收窗时做差。
    //   ⛔ 热路径零改动：`prof_.dispatch_by_ea` 的写点仍然只有 `profileDispatchSample`（每 stride 一次）。
    //   关档时连这份拷贝都不做（`win_` 的这块保持空 ⇒ `endWindowText` 不追加任何行）。
    if (prof_.on) {
        win_.dispatch_open_calls = prof_.dispatch_calls;
        win_.dispatch_open.clear();
        win_.dispatch_open.reserve(prof_.dispatch_by_ea.size());
        for (const auto& kv : prof_.dispatch_by_ea) {
            win_.dispatch_open.emplace_back(kv.first, kv.second.first);
        }
    }
    // 每个 shim 的精确账：按 registry id 索引（`byId` 是 O(1)；⛔ 不用 map，热路径里每次调用
    // 一次 map 查找在 init 那种 40 万次 shim 的窗口里是几 ms 级噪声）。
    const std::size_t nshim = shims_.size();
    win_.per_shim_ns.assign(nshim, 0);
    win_.per_shim_calls.assign(nshim, 0);
    // ★ 仪器自校准（同 `beginProfile` 的忙循环；窗口内每个采样迭代 3 次取时 ⇒ 必须量出来扣掉）。
    //   一次窗口只做一次（2000 次取时 ≈ 40 µs）。
    {
        constexpr int kReps = 2000;
        volatile std::int64_t sink_ns = 0;
        const auto c0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kReps; ++i) {
            const auto t = std::chrono::steady_clock::now();
            sink_ns ^= static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
        }
        const auto c1 = std::chrono::steady_clock::now();
        const double total_ns = std::chrono::duration<double, std::nano>(c1 - c0).count();
        win_.clock_read_ns = total_ns / static_cast<double>(kReps);
    }
    // ★ 再量一次"区间里那一刀"：`t0=now(); t1=now()` ⇒ `t1-t0` 就是一个**被计时区间**里的仪器税。
    //   这是 `netSpan` 要扣的那个数（⛔ 不要拿上面那个"调 now() 一圈"的数顶替，实测差 2×）。
    {
        constexpr int kPairs = 256;
        std::uint64_t acc = 0;
        for (int i = 0; i < kPairs; ++i) {
            const std::uint64_t t0 = windowNowNs();
            const std::uint64_t t1 = windowNowNs();
            acc += t1 - t0;
        }
        win_.span_overhead_ns = static_cast<double>(acc) / static_cast<double>(kPairs);
    }
    ::liftDynCountOn = true;
    return true;
}

bool AotHost::endWindowText(std::string* text) {
    // ★ 状态位是唯一判据（未开 ⇒ 没有表可取；幂等）。
    if (!win_.closeWindow()) return false;
    // ★ 临时诊断（`LIFT_MEMCPY_DIAG=1`）：收窗时结算 `rep_bulk` 的窗口增量；缺省档 no-op。
    memcpy_diag::endWindow();
    // ★ 临时诊断（`LIFT_CALL_DIAG=1`）：收窗时落一段计数；缺省档 no-op。
    call_diag::endWindow();
    win_.closeEdges(windowNowNs());
    win_.insns_close = cpu_.insns;
    win_.frames_close = presenter_.frameCount();
    win_.liftdyn_close = ::liftDynCalls;
    ::liftDynCountOn = false;

    // ★ 窗口级 EA 派发归因的**差分**（`--profile` 开档才做）：快照（按 EA 升序）与当前表
    //   （`std::map` 迭代序也是 EA 升序）归并，`本次 − 开窗时` = 本窗口的采样命中分布。
    //   ⛔ 这里只读 `prof_.dispatch_by_ea`，不写它；也不碰任何热路径计数器。
    if (prof_.on) {
        win_.dispatch_window_calls = prof_.dispatch_calls - win_.dispatch_open_calls;
        win_.dispatch_rows.clear();
        win_.dispatch_window_samples = 0;
        auto open_it = win_.dispatch_open.begin();
        for (const auto& kv : prof_.dispatch_by_ea) {
            while (open_it != win_.dispatch_open.end() && open_it->first < kv.first) ++open_it;
            const std::uint64_t before =
                (open_it != win_.dispatch_open.end() && open_it->first == kv.first) ? open_it->second : 0;
            const std::uint64_t now = kv.second.first;
            if (now > before) {
                win_.dispatch_rows.push_back({kv.first, now - before});
                win_.dispatch_window_samples += now - before;
            }
        }
        // 降序（同数按 EA 升序）⇒ 输出确定；格式层只按这个序打。
        std::sort(win_.dispatch_rows.begin(), win_.dispatch_rows.end(),
                  [](const AotWindowProf::DispatchEaRow& a, const AotWindowProf::DispatchEaRow& b) {
                      if (a.samples != b.samples) return a.samples > b.samples;
                      return a.ea < b.ea;
                  });
    }

    // ★ 文本由 `formatWindowReportText`（pure，`aot/window_prof.cpp`）**一份**实现产出 ⇒ `fputs` 写出的
    //   字节与旧 `printf` 块逐字相同（守卫 `aot-window-report-text` 拿"旧 printf 产出的金样"对齐）。
    //   ⛔ 不要在这里拼格式串：那会让文本重新长出第二份实现（切片 3 把同一份文本交给剪贴板）。
    std::string report = formatWindowReportText(win_, [this](std::uint32_t id) {
        const ShimDef* def = shims_.byId(id);
        return def != nullptr ? (def->module + "!" + def->name) : std::string("(unknown)");
    });
    // ★ `--profile` 开档才追加窗口级 EA 分布（关档 ⇒ 一个字节都不多；与旧窗口表逐字相同）。
    if (prof_.on) report += formatWindowDispatchText(win_);
    if (text != nullptr) *text = report;
    return true;
}

void AotHost::endWindow() {
    // ★ stdout 逐字不变：取文本 + `fputs`（切片 1 的金样判据继续有效）。
    std::string text;
    if (!endWindowText(&text)) return;
    std::fputs(text.c_str(), stdout);
    std::fflush(stdout);
}

const ui::UiHostOps* AotHost::uiHostOps() {
    ui_host_ops_.ctx = this;
    ui_host_ops_.profile_begin = &AotHost::uiProfileBeginThunk;
    ui_host_ops_.profile_end = &AotHost::uiProfileEndThunk;
    return &ui_host_ops_;
}

bool AotHost::uiProfileBeginThunk(void* ctx, const char* label, std::string* err) {
    auto* self = static_cast<AotHost*>(ctx);
    if (self == nullptr) {
        if (err != nullptr) *err = "profileBegin: 宿主上下文为空";
        return false;
    }
    // ★ 复用既有 `beginWindow` 语义：已开 ⇒ false 且**不 reset**（⛔ 不新开状态机）。
    if (!self->beginWindow(label)) {
        if (err != nullptr) *err = "已经有一段 profile 窗口开着（begin 不会 reset 上一段）";
        // 一行 stderr（一次性；与配方路径同样的可见性；⛔ stdout 不动）。
        std::fprintf(stderr, "[profile] profileBegin 被拒：已有一段窗口开着（label=%s）\n",
                     (label != nullptr && *label != '\0') ? label : "(unnamed)");
        return false;
    }
    return true;  // 成功**不打** stderr（每次点击都打就成了噪音）
}

bool AotHost::uiProfileEndThunk(void* ctx, std::string* text, std::string* err) {
    auto* self = static_cast<AotHost*>(ctx);
    if (self == nullptr || text == nullptr) {
        if (err != nullptr) *err = "profileEnd: 宿主上下文为空";
        return false;
    }
    if (!self->endWindowText(text)) {
        if (err != nullptr) *err = "没有打开的 profile 窗口";
        std::fprintf(stderr, "[profile] profileEnd：没有打开的窗口（忽略）\n");
        return false;
    }
    // ★ 裁决 3：剪贴板在 `profileEnd` **内部**完成 —— "INVALID 仍复制"由 C++ 保证（漏不掉）。
    std::string copy_err;
    if (lift::setSystemClipboard(*text, &copy_err)) {
        // 一次性 stderr（需求单"一行 stderr"）：不读回系统剪贴板，只报我们写了多少。
        std::fprintf(stderr, "[profile] 结论文本已写入剪贴板（%zu 字节）\n", text->size());
    } else {
        // ★ ok 仍为 true：窗口关了、文本已产出，HUD 仍要能显示它（并把失败如实说出来）。
        if (err != nullptr) *err = copy_err;
        std::fprintf(stderr, "[profile] 结论文本已生成，但写入剪贴板失败：%s\n", copy_err.c_str());
    }
    return true;
}

}  // namespace aot
}  // namespace lift
