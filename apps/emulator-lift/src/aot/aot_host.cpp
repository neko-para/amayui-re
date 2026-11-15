// emulator-lift · AOT 宿主（bring-up + 宿主服务装配）—— 实现
//
// ★ 本文件只做**装配与读数**；执行循环在 `aot_run.cpp`（同一个类，方法分开只为"装配"与"执行"两件事
//   各自可读）。常量一律照 `src/runtime/machine.cpp` 的 `Machine::init` / `wire` / `loadImage` /
//   `prepare`（那是解释器臂的**唯一**真源），⛔ 不在这里另写一套。
#include "aot/aot_host.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <thread>
#include <utility>
#include <vector>

#include "aot/aot_dispatch.h"
#include "runtime/sha256.h"  // lift::toLower（与解释器 `main.cpp:882` 同一份）
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
    // ★ 音频：口径 = 解释器臂的 `--audio off`（`AudioEngine::attach` + `AudioMode::Off`）。
    //   ⛔ 不建真设备 / 不跑喂料线程（AOT 本轮没有音频验收目标）。
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

// =====================================================================================
// native override（D25 第 1 项）：`.lst` / 两张表的装配
//
// ★ 为什么 AOT 也要 `.lst`：函数级 override 的 EA **按名字解析**（`lst_.findFunction(name)`），
//   段级项还要用 `.lst` 校验 entry/resume 的锚 —— 与解释器 `machine.cpp:3190-3309` 同一份判据。
//   ⛔ 不硬编码 EA（那会在换一份语料时静默指向别的函数）；AOT 的 exe pin（D1）只保证"字节没换"，
//   不保证"名字 ↔ EA 的映射没换"。
// ★ 装配的**唯一**差异（登记，⛔ 不藏）：需要嵌套 guest 执行器的项不装。判据不在名字里，在
//   `native_image_upload.cpp:242` 那一行（`hasNestedGuestExecutor()`）—— 见下面 `kNestedOnly`。
// =====================================================================================
namespace {

// ★ AOT **装不了**的项：它们会 `startNestedGuestCall()` 重入解释器，而 AOT 的
//   `hasNestedGuestExecutor()` 恒 false（`aot_host.h` 的类注释）⇒ 装了就是第一次调用即响亮停机。
//   ⛔ 不为了它去开嵌套执行器（那是另一件事，且有它自己的风险）。名字 → EA 仍由 `.lst` 解析。
struct NestedOnlyOverride {
    const char* name;
    const char* reason;
};
constexpr NestedOnlyOverride kNestedOnly[] = {
    {"sub_48B090",
     "native_image_upload 用 startNestedGuestCall() 重入解释器（AOT 没有嵌套执行器）"},
};

const NestedOnlyOverride* findNestedOnly(const std::string& name) {
    for (const NestedOnlyOverride& n : kNestedOnly) {
        if (name == n.name) return &n;
    }
    return nullptr;
}

}  // namespace

bool AotHost::loadLst(const std::string& path, std::string* err) {
    if (!image_loaded_) {
        if (err) *err = "还没有载入镜像（`.lst` 的 sha 校验要用镜像的 sha256）";
        return false;
    }
    std::string why;
    if (!lst_.load(path, &why)) {
        if (err) *err = "载入 .lst 失败（" + path + "）：" + why;
        return false;
    }
    // ★ 与解释器 `main.cpp:884-895` 逐条同口径：**执行源与语料必须同一份**，否则"名字 → EA"
    //   解析出来的每一个 EA 都可能指在别的函数上（而 override 是**按 EA 接管**的 ⇒ 静默跑错）。
    const std::string lst_sha = toLower(lst_.headerSha256());
    const std::string pe_sha = toLower(image_.sha256);
    if (lst_sha.empty()) {
        if (err) *err = "`.lst` 头部没有 `; Input SHA256 :`（" + path + "）⇒ 无法校验字节来源";
        return false;
    }
    if (lst_sha != pe_sha) {
        if (err) {
            *err = "拒绝装配 —— 执行源与语料不是同一份二进制\n  PE   sha256 = " + pe_sha + "  (" +
                   exe_path_ + ")\n  .lst sha256 = " + lst_sha + "  (" + path + ")";
        }
        return false;
    }
    lst_loaded_ = true;
    // 与解释器同口径（`cpu.lst`）：fault 报告的指令文本源 / 段级 override 的判据（`native_image_upload`
    // 那条我们不走，但别的实现也可能读它）。⛔ 执行路径不依赖它。
    cpu_.lst = &lst_;
    return true;
}

bool AotHost::configureNativeOverrides(const std::string& list, std::string* err) {
    // ★ 结构照 `Machine::configureNativeOverrides`（`machine.cpp:3190-3309`）：**判据一行不改**，
    //   只把"存到 Machine 的成员"换成"存到本宿主的成员"。
    if (!lst_loaded_) {
        if (err) *err = "native override 需要 --lst（函数级按名字解析 EA、段级按 `.lst` 锚校验；⛔ 不硬编码 EA）";
        return false;
    }
    overrides_spec_ = list;
    auto knownNames = []() {
        std::string s;
        for (std::size_t i = 0; i < knownNativeCount(); ++i) {
            if (!s.empty()) s += ", ";
            s += knownNativeAt(i).name;
            if (knownNativeAt(i).debug) s += "(debug)";
        }
        for (std::size_t i = 0; i < knownBlockNativeCount(); ++i) {
            if (!s.empty()) s += ", ";
            s += knownBlockNativeAt(i).name;
            s += "(段级)";
            if (knownBlockNativeAt(i).debug) s += "(debug)";
        }
        return s;
    };
    auto trim = [](std::string s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        return s;
    };
    std::vector<std::string> tokens;
    {
        std::size_t start = 0;
        while (start <= list.size()) {
            const std::size_t comma = list.find(',', start);
            tokens.push_back(
                trim(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    auto knownAny = [](const std::string& n) {
        return findKnownNative(n) != nullptr || findKnownBlockNative(n) != nullptr;
    };
    std::vector<std::string> disabled;
    for (const std::string& token : tokens) {
        if (token.empty() || token.front() != '-') continue;
        const std::string name = trim(token.substr(1));
        if (name.empty()) {
            if (err) *err = "`-` 后面需要一个 native override 名字";
            return false;
        }
        if (!knownAny(name)) {
            if (err) *err = "未知的 native override 排除项：" + name + "（已知：" + knownNames() + "）";
            return false;
        }
        disabled.push_back(name);
    }
    auto isDisabled = [&](const std::string& name) {
        return std::find(disabled.begin(), disabled.end(), name) != disabled.end();
    };
    // 段级项：先做锚校验（entry 必须是 `.lst` 里的那条指令、且属于声明的函数），再入表。
    // ★ 校验失败**一定打一行**（与解释器同口径）—— 否则"优化没生效"在日志里看不出来。
    auto addBlock = [&](const KnownBlockNative& k, bool required) -> bool {
        if (isDisabled(k.name)) return true;
        std::string why;
        if (!verifyKnownBlockAnchor(lst_, k, &why)) {
            std::fprintf(stderr, "[aot][native] %s\n", why.c_str());
            if (required) {
                if (err) *err = why;
                return false;
            }
            return true;
        }
        if (block_overrides_.find(k.entry_ea) != nullptr) return true;
        return block_overrides_.add(k.entry_ea, k.resume_ea, k.name, k.fn, err, k.feeder_ok);
    };
    auto addName = [&](const std::string& name, bool required) -> bool {
        if (isDisabled(name)) return true;
        if (const KnownBlockNative* kb = findKnownBlockNative(name)) return addBlock(*kb, required);
        const KnownNative* k = findKnownNative(name);
        if (!k) {
            if (required && err) *err = "未知的 native override：" + name + "（已知：" + knownNames() + "）";
            return !required;
        }
        const LstFunction* f = lst_.findFunction(name);
        if (!f) {
            if (required && err) *err = "在 .lst 里找不到函数入口：" + name;
            return !required;
        }
        // ★ AOT 的装配差异（唯一的）：需要嵌套 guest 执行器 ⇒ **不装**，只登记 + 每次都点名打一行。
        if (const NestedOnlyOverride* nested = findNestedOnly(name)) {
            if (std::find(nested_only_ea_.begin(), nested_only_ea_.end(), f->ea) == nested_only_ea_.end()) {
                nested_only_ea_.push_back(f->ea);
                nested_only_name_.push_back(name);
                std::fprintf(stderr,
                             "[aot][native] %s @ %08X 需要嵌套 guest 执行器（%s）⇒ 本轮**跳过**"
                             "（保持走 lifted 代码；⛔ 不为它开嵌套执行器）\n",
                             name.c_str(), f->ea, nested->reason);
            }
            return true;
        }
        // 别名（两个名字指向同一个 EA）只加一次：`all` 不该因为别名而硬失败。
        if (overrides_.find(f->ea) != nullptr) return true;
        return overrides_.add(f->ea, name, k->conv, k->arg_bytes, k->fn, err);
    };

    for (const std::string& token : tokens) {
        if (token.empty() || token.front() == '-') continue;
        if (token == "all") {
            // `all` = 只开**已验证**项；debug 项必须显式点名；排除项优先。函数级 + 段级都在 `all` 里。
            for (std::size_t i = 0; i < knownNativeCount(); ++i) {
                if (knownNativeAt(i).debug) continue;
                if (!addName(knownNativeAt(i).name, false)) return false;
            }
            for (std::size_t i = 0; i < knownBlockNativeCount(); ++i) {
                if (knownBlockNativeAt(i).debug) continue;
                if (!addName(knownBlockNativeAt(i).name, false)) return false;
            }
        } else if (!addName(token, true)) {
            return false;
        }
    }
    if (overrides_.empty() && block_overrides_.empty() && nested_only_ea_.empty()) {
        if (err) *err = "没有解析到任何 native override（检查名字 / .lst）";
        return false;
    }
    rebuildOverrideFilter();
    return true;
}

void AotHost::rebuildOverrideFilter() {
    override_filter_.assign(kOverrideFilterBits / 64u, 0ull);
    auto addBit = [this](std::uint32_t ea) {
        const std::size_t bit = (static_cast<std::size_t>(ea) >> 2) & (kOverrideFilterBits - 1u);
        override_filter_[bit >> 6] |= 1ull << (bit & 63u);
    };
    for (std::size_t i = 0; i < overrides_.size(); ++i) addBit(overrides_.at(i)->ea);
    for (std::size_t i = 0; i < block_overrides_.size(); ++i) addBit(block_overrides_.at(i)->entry_ea);
    for (const std::uint32_t ea : nested_only_ea_) addBit(ea);
}

bool AotHost::overrideCandidate(std::uint32_t ea) const {
    if (override_filter_.empty()) return false;
    const std::size_t bit = (static_cast<std::size_t>(ea) >> 2) & (kOverrideFilterBits - 1u);
    return ((override_filter_[bit >> 6] >> (bit & 63u)) & 1ull) != 0;
}

bool AotHost::overrideCallFrame(std::uint32_t callee) const {
    // ① 栈顶必须可读（`[esp]` = 返回地址）
    const std::uint32_t esp = cpu_.esp();
    if (!mem_.committed(esp, 4u)) return false;
    const std::uint32_t ret = mem_.rd32(esp);
    // ② `call rel32` 是 5 字节 ⇒ 站点 = ret - 5；站点必须可读且是 `E8 rel32`
    if (ret < 5u) return false;
    const std::uint32_t site = ret - 5u;
    if (!mem_.committed(site, 5u)) return false;
    const std::uint8_t* p = mem_.host(site);
    if (p[0] != 0xE8u) return false;
    std::int32_t rel = 0;
    std::memcpy(&rel, p + 1, sizeof(rel));
    // ③ 那个 call 的目标必须正好是本次到达的 EA（32 位回绕，与 `genBranchTarget` 同口径）
    return static_cast<std::uint32_t>(site + 5u + static_cast<std::uint32_t>(rel)) == callee;
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

void AotHost::printReadout(std::FILE* out) const {
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
                 cpu_.audio != nullptr ? audioModeName(audio_.mode()) : "no");
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

    // ---- native override 的**装配面**（与解释器启动回声 `main.cpp:916-918` 同一组字段）----
    //   ★ 启动时还没有 hits ⇒ 这里只报"装了几项 + 谁没装"；命中数在运行摘要里。
    {
        std::fprintf(out, "native-ovr: %zu 项 + %zu 段级（--native-overrides=%s）",
                     overrides_.size(), block_overrides_.size(),
                     overrides_spec_.empty() ? "未开" : overrides_spec_.c_str());
        if (!nested_only_name_.empty()) {
            std::fprintf(out, " | ⛔ 未装（缺嵌套执行器）：");
            for (std::size_t i = 0; i < nested_only_name_.size(); ++i) {
                std::fprintf(out, "%s%s", i == 0 ? "" : " / ", nested_only_name_[i].c_str());
            }
        }
        std::fprintf(out, "\n");
        for (std::size_t i = 0; i < overrides_.size(); ++i) {
            const NativeOverride* e = overrides_.at(i);
            std::fprintf(out, "  %-12s @ %08X %s(%uB)\n", e->name.c_str(), e->ea, guestConvName(e->conv),
                         e->arg_bytes);
        }
        for (std::size_t i = 0; i < block_overrides_.size(); ++i) {
            const NativeBlockOverride* e = block_overrides_.at(i);
            std::fprintf(out, "  %-12s @ %08X..%08X 段级（函数中段；跳到出口）\n", e->name.c_str(),
                         e->entry_ea, e->resume_ea);
        }
    }

    // ---- guest 区域表（"谁占了哪一段"的机械读数；复用既有的 dumpRegions）----
    std::fprintf(out, "regions   : %zu 段（committed=%llu 字节）\n", mem_.regions().size(),
                 static_cast<unsigned long long>(mem_.committedBytes()));
    dumpRegions(mem_.regions(), out);}

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
    // ---- native override 的账（D25 第 1 项）----
    // ★ 字段口径 = 解释器 `machine.cpp:2451-2472` 的 `native-ovr: entries/blocks/hits/fallbacks` +
    //   本轮任务书要求的固定一行 `override : hits=… insns=… skipped-nested=…`。
    //   ★ `hits` / `fallbacks` 从**表项**里求和（与解释器同一来源，⛔ 不另立第二份计数器），
    //     `insns` = 本轮的 `AotRunResult::ov_insns`（override 折算掉的 guest 指令数）。
    //   ★ 缺省档（没开 `--native-overrides`）**也打这一行**：`hits=0` 与"根本没装表"是两件事。
    {
        std::uint64_t fn_hits = 0, fn_fb = 0, blk_hits = 0, blk_fb = 0;
        for (std::size_t i = 0; i < overrides_.size(); ++i) {
            fn_hits += overrides_.at(i)->hits;
            fn_fb += overrides_.at(i)->fallbacks;
        }
        for (std::size_t i = 0; i < block_overrides_.size(); ++i) {
            blk_hits += block_overrides_.at(i)->hits;
            blk_fb += block_overrides_.at(i)->fallbacks;
        }
        std::fprintf(out,
                     "override  : hits=%llu insns=%llu skipped-nested=%llu（函数级 %zu 项 hits=%llu "
                     "回退=%llu · 段级 %zu 项 hits=%llu 回退=%llu · 非 call 帧跳过=%llu）\n",
                     static_cast<unsigned long long>(fn_hits + blk_hits),
                     static_cast<unsigned long long>(r.ov_insns),
                     static_cast<unsigned long long>(r.ov_skipped_nested), overrides_.size(),
                     static_cast<unsigned long long>(fn_hits), static_cast<unsigned long long>(fn_fb),
                     block_overrides_.size(), static_cast<unsigned long long>(blk_hits),
                     static_cast<unsigned long long>(blk_fb),
                     static_cast<unsigned long long>(r.ov_not_call));
        std::fprintf(out, "override  : 开关 = --native-overrides=%s\n",
                     overrides_spec_.empty() ? "(未给 ⇒ 两张表都空)" : overrides_spec_.c_str());
        if (!nested_only_name_.empty()) {
            std::fprintf(out, "override  : ⛔ 未装（缺嵌套 guest 执行器）：");
            for (std::size_t i = 0; i < nested_only_name_.size(); ++i) {
                std::fprintf(out, "%s%s", i == 0 ? "" : " / ", nested_only_name_[i].c_str());
            }
            std::fprintf(out, "（到达次数 = 上面的 skipped-nested）\n");
        }
        // ---- top-N：按 hits 降序（同 hits 按名字稳定排序；⛔ 不依赖表内顺序）----
        struct Row {
            std::uint64_t hits;
            std::uint64_t fallbacks;
            std::uint32_t ea;
            std::uint32_t ea2;  // 段级：resume EA；函数级 = 0
            std::string name;
            bool block;
        };
        std::vector<Row> rows;
        for (std::size_t i = 0; i < overrides_.size(); ++i) {
            const NativeOverride* e = overrides_.at(i);
            rows.push_back({e->hits, e->fallbacks, e->ea, 0u, e->name, false});
        }
        for (std::size_t i = 0; i < block_overrides_.size(); ++i) {
            const NativeBlockOverride* e = block_overrides_.at(i);
            rows.push_back({e->hits, e->fallbacks, e->entry_ea, e->resume_ea, e->name, true});
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.hits != b.hits) return a.hits > b.hits;
            return a.name < b.name;
        });
        // ★ top-N = **所有 hits>0 的项**（同 hits 按名字稳定排序；⛔ 不依赖表内顺序）；
        //   `hits==0` 的项只在一行里点名 —— 它们不是异常（这一趟没走到），但"装了什么却没走到"
        //   是判"override 有没有接上"的第一手读数（P0 的 29.3% 那一项就是"1 次调用、上亿条"）。
        constexpr std::size_t kTopN = 16;
        std::size_t nonzero = 0;
        for (const Row& w : rows) {
            if (w.hits != 0) ++nonzero;
        }
        const std::size_t shown = std::min(kTopN, nonzero);
        for (std::size_t i = 0; i < shown; ++i) {
            const Row& w = rows[i];
            std::fprintf(out, "override  :   %zu. %-12s @ %08X%s hits=%llu 回退=%llu\n", i + 1,
                         w.name.c_str(), w.ea, w.block ? "（段级）" : "",
                         static_cast<unsigned long long>(w.hits),
                         static_cast<unsigned long long>(w.fallbacks));
        }
        if (nonzero > shown) {
            std::fprintf(out, "override  :   另有 %zu 项 hits>0（未展开）\n", nonzero - shown);
        }
        if (rows.size() > shown) {
            std::fprintf(out, "override  :   hits=0 的 %zu 项：", rows.size() - shown);
            for (std::size_t i = shown; i < rows.size(); ++i) {
                std::fprintf(out, "%s%s", i == shown ? "" : " / ", rows[i].name.c_str());
            }
            std::fprintf(out, "\n");
        }
    }
    // ---- 呈现读数（可测性口径；与解释器 `present-tail` / `d3d9.cpp:845-863` 同一组字段名）----
    //   ★ `lastHash` 是**最后一次 Present 的 guest 后缓冲哈希**（`arena.hash(surface)`，`present.cpp:68`）；
    //     逐帧读数由 `run()` 在每次 Present 之后打印（`[frame] n=… hash=…`）。
    //   ★ `sinkFailures` 与解释器同口径（提交失败**不改** guest 的 Present 结果）。
    std::fprintf(out,
                 "present   : sink=%s frames=%llu lastHash=0x%016llX sinkFailures=%llu%s\n",
                 presenter_.sink() != nullptr ? presenter_.sink()->describe().c_str() : "headless(no-sink)",
                 static_cast<unsigned long long>(r.frames),
                 static_cast<unsigned long long>(presenter_.lastHash()),
                 static_cast<unsigned long long>(presenter_.sinkFailures()),
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
}

}  // namespace aot
}  // namespace lift
