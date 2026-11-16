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
#include "runtime/control.h"       // 控制层 Present 闸门 / Context（轻头：⛔ 无 beast / nlohmann）
#include "runtime/decode_cache.h"  // `liftOneDecodeCache()`：只读命中率（`decode-cache:` 行）
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

    // ---- ② surface arena（真源 `Machine::init`：`surfaces_.init(&mem_, 0x28000000, 0x08000000)`）----
    // ★ 图形 shim 的 `requireMachine`（`d3d9.cpp:115` / `ddraw_surface.cpp:107`）第一件事就是查
    //   `surfaces().ready()`；不建它 ⇒ 图形路径**必停**。基址 / 大小与解释器臂逐字相同。
    if (!surfaces_.init(&mem_, 0x28000000u, 0x08000000u)) {
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
    // ★ 宿主服务视图（`runtime/host_services.h`）：`AotHost` 自己实现那份接口 ⇒ shim 侧
    //   `hostOf(cpu)` / `hostOf(cpu)->ownsCpu(cpu)` 的判据对 AOT 成立（D4 的解耦面）。
    cpu_.host = this;
    // ★ `present-tail` 的 insns 源：与 `Machine::wire` 同一个注入点（只读指针）。
    presenter_.setInsnSource(&cpu_.insns);
    // ★ 音频：`attach` 之后先落**缺省档**（`configure(off,"",false)`）——它与改动前的形状逐位相同。
    //   ★ CLI 的 `--audio` / `--audio-dump` / `--sound` 由 `aot_main` 在 `init()` 之后调
    //     `configureAudio()` 覆盖（`configure` 只要求"还没 begin"）；缺省档不会走到那一步的差异上。
    //   ★ 生命周期（begin / end）在 `run()` 的入口 / 出口（与解释器 `machine.cpp:1059` / `:1854` 同口径）。
    audio_.attach(&cpu_, &mem_, &clock_);
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
    // ★ 时钟 = **wall clock**（D2b）：这是**这台宿主**的属性，不是某一轮的选项 ⇒ 在装配期就定下来
    //   （readout 因此也能如实打印它；`run()` 里那一行是幂等复核）。
    clock_.setRealTime(true);
    // ★ SEH 状态机与 x87 现场：与 `Machine::wire` 一样在装配末尾复位一次。
    seh_.reset();
    cpu_.fpu.reset();
    initialized_ = true;
    return true;
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
    registerKernel32Shims(shims_);
    registerBootstrapShims(shims_);
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
    cpu_.shim.va_limit = 0x28000000u;

    prepared_ = true;
    return true;
}

// AGERC.DLL：真实 guest 模块装载（真源 `Machine::loadAgercModule`，`machine.cpp:404-493`）。
// ★ 为什么 AOT 也必须做：guest 在启动早期就 `LoadLibraryA("AGERC.DLL")`（`ret=0048E751`），
//   拿不到镜像 ⇒ 它返回 0 + 126 ⇒ guest 走**错误对话框**分支（`wsprintfA` → `MessageBoxA`）。
//   实测读数：不装 AGERC 时 AOT 在 36,489 条指令处停在 `poly:005C3627`（= `j_MessageBoxA_15` thunk，
//   它不在闭包里 ⇒ 响亮停），而解释器在同一个 `LoadLibraryA` 上拿到 `0x30000000` 并继续
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
    //   尊重它）；冲突 ⇒ 用 0x30000000。冲突表 = 主镜像 / 堆 / 栈（含 guard）/ TEB·PEB·TLS / 命令行块 /
    //   桩页 / VirtualAlloc 区间 + 假模块句柄空间 + 所有已登记区域。
    constexpr std::uint32_t kFallbackBase = 0x30000000u;
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
    addRange(static_cast<std::uint64_t>(layout_.stack_top) - layout_.stack_size - layout_.stack_guard,
             layout_.stack_top);
    addRange(layout_.teb_ea, static_cast<std::uint64_t>(layout_.teb_ea) + 0x1000u);
    addRange(layout_.peb_ea, static_cast<std::uint64_t>(layout_.peb_ea) + 0x1000u);
    addRange(layout_.tls_array_ea, static_cast<std::uint64_t>(layout_.tls_array_ea) + 0x1000u);
    addRange(layout_.argv_block, static_cast<std::uint64_t>(layout_.argv_block) + 0x1000u);
    addRange(layout_.shim_page, static_cast<std::uint64_t>(layout_.shim_page) +
                                    static_cast<std::uint64_t>(layout_.shim_slots) * 4u);
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
            setErr("选基址", "PE 的 ImageBase 冲突，备选 0x30000000 也冲突（没有可用的 guest 基址）");
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

void AotHost::printReadout(std::FILE* out, bool region_log) const {
    if (out == nullptr) return;

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
    const std::uint32_t esp_sentinel = mem_.committed(cpu_.esp(), 4u) ? mem_.rd32(cpu_.esp()) : 0xDEADBEEFu;
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
                 clock_.realTime() ? "real-time" : "virtual",
                 cpu_.handles != nullptr ? "yes" : "no", cpu_.com != nullptr ? "yes" : "no",
                 cpu_.calls != nullptr ? "yes" : "no", cpu_.windows != nullptr ? "yes" : "no",
                 cpu_.fonts != nullptr ? "yes" : "no",
                 // ★ 读**配置**事实（`configuredMode()`），不是 `mode()`：readout 打在 `run()` 的
                 //   `begin()` **之前**，那时 `mode_` 还是 Off ⇒ 用 `mode()` 会让 `--audio sdl`
                 //   的 bring-up 读数写一句假的 `audio=off`（真实的档位 / `device=ok|fallback`
                 //   在收口的 `audio     :` 一行）。缺省档两者同为 `off` ⇒ 默认档读数逐字不变。
                 cpu_.audio != nullptr ? audioModeName(audio_.configuredMode()) : "no");
    std::fprintf(out, "owner     : cpu.machine=nullptr（AOT 有意没有 Machine）+ cpu.host=this\n");

    // ---- 派发表（构建期生成物的真源是 lift-gen --table）----
    std::fprintf(out,
                 "pin       : generated=%s sha256=%s entry=%08X roots=%llu entries=%llu funcs=%llu "
                 "checked=%s\n",
                 generatedPresent() ? "yes" : "NO（构建期缺 clang / 执行源 / closure.txt）",
                 generatedPresent() ? pinnedSha256() : "(none)", pinnedEntry(),
                 static_cast<unsigned long long>(pinnedRoots()),
                 static_cast<unsigned long long>(pinnedEntries()),
                 static_cast<unsigned long long>(pinnedFunctions()),
                 pin_checked_ ? "yes" : "skipped");


    // ---- guest 区域表（"谁占了哪一段"的机械读数；复用既有的 dumpRegions）----
    //   ★ **opt-in**（`--region-log`，缺省关；见 `AotRunOptions::region_log`）：这 28 行是静态布局快照，
    //     一次 run 里不变 ⇒ 属噪音。⛔ 只闸这一块：上面任何一条 readout 都不受影响。
    if (region_log) {
        std::fprintf(out, "regions   : %zu 段（committed=%llu 字节）\n", mem_.regions().size(),
                     static_cast<unsigned long long>(mem_.committedBytes()));
        dumpRegions(mem_.regions(), out);
    }
}

void AotHost::printRunSummary(const AotRunResult& r, std::FILE* out) const {
    if (out == nullptr) return;
    const DispatchStats ds = dispatchStats();
    std::fprintf(out, "stop      : %s @ eip=%08X（%s）%s%s\n", aotStopName(r.reason), r.ea,
                 r.ok() ? "正常结局" : "非正常结局", r.detail.empty() ? "" : " — ", r.detail.c_str());
    std::fprintf(out, "insns     : %llu（`lift_tick` 记账；与解释器同一份 `X86::insns` 语义）\n",
                 static_cast<unsigned long long>(r.insns));
    std::fprintf(out, "frames    : %llu（present 计数；本臂 --max-frames=%u）\n",
                 static_cast<unsigned long long>(r.frames), cpu_.shim.max_frames);
    std::fprintf(out, "dispatch  : total=%llu（lifted=%llu shim=%llu）\n",
                 static_cast<unsigned long long>(r.dispatches),
                 static_cast<unsigned long long>(r.lifted_calls),
                 static_cast<unsigned long long>(r.shim_calls));
    std::fprintf(out,
                 "table     : entries=%zu lookups=%llu hits=%llu misses=%llu comparisons=%llu "
                 "avg_cmp=%.2f\n",
                 tableSize(), static_cast<unsigned long long>(ds.lookups),
                 static_cast<unsigned long long>(ds.hits), static_cast<unsigned long long>(ds.misses),
                 static_cast<unsigned long long>(ds.comparisons),
                 ds.lookups != 0 ? static_cast<double>(ds.comparisons) / static_cast<double>(ds.lookups)
                                 : 0.0);
    std::fprintf(out, "region    : arena in_use=%u peak=%u surfaces=%zu heap_blocks=%zu\n",
                 surfaces_.bytesInUse(), surfaces_.peak(), surfaces_.surfaceCount(),
                 heap_.blockCount());
    // ---- 呈现读数（可测性口径；与解释器 `present-tail` / `d3d9.cpp:845-863` 同一组字段名）----
    //   ★ `lastHash` 是**最后一次 Present 的 guest 后缓冲哈希**（`arena.hash(surface)`，`present.cpp:68`）；
    //     逐帧读数由 `run()` 在每次 Present 之后打印（`[frame] n=… hash=…`）。
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
        std::fprintf(out, "recovery  : applied @ 004BA718 (EAX %u -> 0)（--accept-recovery-dialog 默认绕过）\n",
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
                 static_cast<unsigned long long>(timerFires_), static_cast<unsigned long long>(callResumes_),
                 static_cast<unsigned long long>(seh_.raiseCount()),
                 static_cast<unsigned long long>(seh_.handlerCalls()),
                 static_cast<unsigned long long>(seh_.unhandledCount()));
    // ---- S2 的**机械判据**：音频**启用**时打一行与解释器 `machine.cpp:2955` 同形的 `audio     :`
    //   （`mode` / `device=ok|fallback|none` / `muted` / fills / pushed / underruns / …）。
    //   ★ **只有在 `enabled()` 时**才打：缺省档（`--audio off`，包括不给任何音频 flag）**一行都不打**
    //     ⇒ 默认档 stdout 与改动前逐字相同（`AGENTS.md` §12.8 第 3 条 / 本单最硬的一条）。
    //   ★ 一次性输出（不是周期性）：落点与 `--profile` 块同处"stdout 末尾"的前一刻。
    if (audio_.enabled()) {
        audio_.dumpSummary(out);
        // ---- S3 泵的读数（**只在音频开启时打**；off 档这一行不存在 ⇒ 默认档 stdout 逐字不变）----
        //   为什么要另起一行而不塞进上面那行：`dumpSummary` 是**解释器与 AOT 共用**的一份口径
        //   （`machine.cpp:2955` 也在用），改它的字段顺序会让两条臂的 `audio     :` 行不可比；
        //   而 `calls=`（真调 pump 的次数）是 AOT 臂独有的量。⇒ 追加一行，⛔ 不动那一行。
        //   ★ 判据面：`fills>0 && fill_bytes>0 && pushed>0` = "通知真被消费、字节真的喂进去了"；
        //     `calls=0` 只在 off 档成立，而 off 档根本不打这一行 ⇒ 两者不会互相冒充。
        const AudioEngine::Snapshot a = audio_.snapshot();
        std::fprintf(out, "audio-pump: calls=%llu fills=%llu fill_bytes=%llu pushed=%llu\n",
                     static_cast<unsigned long long>(audio_pump_.calls()),
                     static_cast<unsigned long long>(a.fills),
                     static_cast<unsigned long long>(a.fill_bytes),
                     static_cast<unsigned long long>(a.pushed_bytes));
    }
    // ---- `--profile` 的整块读数（**关档时一行都不打** ⇒ 全文与基线逐字相同）----
    // ★ 落点为什么在这里：`aot_main.cpp` 在 `printRunSummary` 之后只有 stderr 回声 + `return`，
    //   所以这里就是"stdout 末尾（进程退出前）"。⛔ 不改 main 的打印顺序。
    printProfile(out);
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
// 控制层（S2/S3/S4）的**主线程取数口**：全部在主线程被调（`Control::serve` 在汇点里跑）
//   ★ 这里只**读**宿主 / guest；⛔ 不改 guest 状态、⛔ 不进执行循环。
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

void AotHost::controlOnResume() { presenter_.noteControlResume(); }

// =====================================================================================
// 窗口 profile（`profile_begin/end` 驱动；桶与闭式在 `aot/window_prof.h`，这里只做
// "开/关 + 快照 + 打表"。⛔ 零新 CLI：不开窗 ⇒ 一个取时都不做、一行都不打。
// =====================================================================================
void AotHost::beginWindow(const char* label) {
    win_.reset();
    win_.on = true;
    win_.label = (label != nullptr && *label != '\0') ? label : "(unnamed)";
    win_.t_open_ns = windowNowNs();
    win_.insns_open = cpu_.insns;
    win_.frames_open = presenter_.frameCount();
    win_.liftdyn_open = ::liftDynCalls;
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
}

void AotHost::endWindow() {
    if (!win_.on) return;
    win_.closeEdges(windowNowNs());
    win_.insns_close = cpu_.insns;
    win_.frames_close = presenter_.frameCount();
    win_.liftdyn_close = ::liftDynCalls;
    win_.on = false;
    ::liftDynCountOn = false;

    const AotWindowProf::Totals t = win_.totals();
    const auto ms = [](double ns) { return ns / 1.0e6; };
    const auto pct = [&](double v) {
        return t.wall > 0.0 ? (v / t.wall) * 100.0 : 0.0;
    };
    std::printf("=== profile window: %s ===\n", win_.label.c_str());
    std::printf("wall        : %10.3f ms   iter=%llu sampled=%llu（stride %llu%s）insns=%llu frames=%llu\n",
                ms(t.wall), static_cast<unsigned long long>(win_.iter),
                static_cast<unsigned long long>(win_.sampled),
                static_cast<unsigned long long>(AotWindowProf::kStride),
                win_.iter < AotWindowProf::kLowSampleBelow ? "，⚠ 窗口短 ⇒ 样本少" : "",
                static_cast<unsigned long long>(win_.insns_close - win_.insns_open),
                static_cast<unsigned long long>(win_.frames_close - win_.frames_open));
    std::printf("lift        : %10.3f ms  %5.1f%%  est\n", ms(t.lift), pct(t.lift));
    std::printf("boundary    : %10.3f ms  %5.1f%%  est\n", ms(t.boundary), pct(t.boundary));
    std::printf("shim        : %10.3f ms  %5.1f%%  exact(N=%llu)\n", ms(t.shim), pct(t.shim),
                static_cast<unsigned long long>(win_.shim_calls));
    // present 子桶（⊂ shim；⛔ 不加进闭式）
    if (win_.present_calls != 0) {
        const double psum = static_cast<double>(win_.present_readback_ns + win_.present_hash_ns +
                                                win_.present_freeze_ns + win_.present_sink_ns +
                                                win_.present_stats_ns);
        std::printf("  present   : %10.3f ms  %5.1f%%  n=%llu（readback %.3f / hash %.3f / freeze %.3f / sink %.3f / stats %.3f ms）\n",
                    ms(psum), pct(psum), static_cast<unsigned long long>(win_.present_calls),
                    ms(static_cast<double>(win_.present_readback_ns)),
                    ms(static_cast<double>(win_.present_hash_ns)),
                    ms(static_cast<double>(win_.present_freeze_ns)),
                    ms(static_cast<double>(win_.present_sink_ns)),
                    ms(static_cast<double>(win_.present_stats_ns)));
    }
    // 每个 shim 的精确行（降序，只打 ≥0.5% 的；余下合并成一行，⛔ 不给"看不见的残差"）
    {
        struct Row {
            double ns;
            std::uint64_t calls;
            std::string name;
        };
        std::vector<Row> rows;
        rows.reserve(win_.per_shim_ns.size());
        for (std::size_t id = 0; id < win_.per_shim_ns.size(); ++id) {
            if (win_.per_shim_calls[id] == 0) continue;
            const ShimDef* def = shims_.byId(static_cast<std::uint32_t>(id));
            rows.push_back(Row{static_cast<double>(win_.per_shim_ns[id]), win_.per_shim_calls[id],
                               def != nullptr ? (def->module + "!" + def->name) : std::string("(unknown)")});
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ns > b.ns; });
        double rest_ns = 0.0;
        std::uint64_t rest_calls = 0;
        int printed = 0;
        for (const Row& r : rows) {
            const double share = t.wall > 0.0 ? r.ns / t.wall : 0.0;
            if (share < 0.005 || printed >= 10) {
                rest_ns += r.ns;
                rest_calls += r.calls;
                continue;
            }
            std::printf("  shim:%-34s %10.3f ms  %5.1f%%  N=%llu\n", r.name.c_str(), ms(r.ns), pct(r.ns),
                        static_cast<unsigned long long>(r.calls));
            ++printed;
        }
        if (rest_calls != 0) {
            std::printf("  shim:(rest)                            %10.3f ms  %5.1f%%  N=%llu\n", ms(rest_ns),
                        pct(rest_ns), static_cast<unsigned long long>(rest_calls));
        }
    }
    std::printf("partial     : %10.3f ms  %5.1f%%  exact\n", ms(t.partial), pct(t.partial));
    std::printf("instrument  : %10.3f ms  %5.1f%%  （%.1f ns/取时 × %llu 次）\n", ms(t.instrument),
                pct(t.instrument), win_.clock_read_ns,
                static_cast<unsigned long long>(win_.sampled * 3 + win_.shim_calls * 2));
    std::printf("unaccounted : %10.3f ms  %5.1f%%  %s\n", ms(t.unaccounted),
                t.wall > 0.0 ? (t.unaccounted / t.wall) * 100.0 : 0.0,
                t.suspect ? "⚠ 五项之和偏离墙钟 >15% ⇒ 本次读数作废（见 coverage）"
                          : (t.valid() ? "ok(≤3%)" : "INVALID: unaccounted 超 3% ⇒ 本次读数作废"));
    std::printf("coverage    : %.2f×  （五项之和 / 墙钟；1.00 = 外推与墙钟吻合）\n", t.coverage);
    // `lift_dyn`：只报次数（尾调用 ⇒ 区间含目标体，⛔ 不进闭式）
    std::printf("lift_dyn    : %llu 次（计数精确；⛔ 不进闭式：尾调用 ⇒ 区间含目标体）\n",
                static_cast<unsigned long long>(win_.liftdyn_close - win_.liftdyn_open));
    // ★ 结论行（写死的单口径，§5）：最高**具名行** ≥10% 才是优化候选。
    {
        double top = 0.0;
        std::string top_name = "(none)";
        for (std::size_t id = 0; id < win_.per_shim_ns.size(); ++id) {
            if (win_.per_shim_calls[id] == 0) continue;
            const double v = static_cast<double>(win_.per_shim_ns[id]);
            if (v > top) {
                const ShimDef* def = shims_.byId(static_cast<std::uint32_t>(id));
                top = v;
                top_name = def != nullptr ? (def->module + "!" + def->name) : std::string("(unknown)");
            }
        }
        // `boundary` 也是具名行；`lift` 聚合**不进候选**（M-12 §5.2）
        const bool boundary_wins = t.boundary > top;
        const double best = boundary_wins ? t.boundary : top;
        const double share = t.wall > 0.0 ? best / t.wall : 0.0;
        const AotWindowProf::Rank rank = AotWindowProf::rankOf(share);
        const char* verdict = rank == AotWindowProf::Rank::Candidate
                                  ? "候选（≥10%）"
                                  : rank == AotWindowProf::Rank::Watch ? "观察名单（3–10%，需两次复现）"
                                                                       : "噪声（<3%）";
        std::printf("verdict     : 最高具名行 = %s %.1f%% ⇒ %s%s\n",
                    boundary_wins ? "boundary" : top_name.c_str(), share * 100.0, verdict,
                    rank == AotWindowProf::Rank::Candidate ? "" : "（无单点值得优化 ⇒ 记「均匀分布」）");
    }
    std::fflush(stdout);
}

void AotHost::windowMarkThunk(void* ctx, const char* label, bool begin) {
    auto* host = static_cast<AotHost*>(ctx);
    if (host == nullptr) return;
    if (begin) {
        host->beginWindow(label);
    } else {
        host->endWindow();
    }
}

}  // namespace aot
}  // namespace lift
