// emulator-lift · AGERC.DLL：**真实 PE 的导出表**是"它有哪些导出"的唯一真源（⛔ 不再维护 3 行白名单）
//
// ★ 口径（2026-11-04 用户改口径 + 2026-11-12 缺陷单 REQ-01M75SDKZGY87B8SK7GBP92PXD）：
//   · AGERC.DLL **作为真实 guest 模块装载** —— PE 节映射进 guest、真实 ImageBase 当 HINSTANCE、
//     资源 / 版本从该镜像解析（见 `Machine::loadAgercModule`）。
//   · 「有哪些导出」= **这份镜像的 export directory**（`agercExportCount/Name/Rva`，从
//     `cpu.shim.agerc_image` 的字节里在**宿主侧**解析）。⛔ 不再手写白名单 —— 那张 3 行的表漏了
//     `_SetNameLenMax@20`，于是 SAVE.BIN 的 `COMMAND=332` 拿不到地址（127）⇒ 引擎弹"致命的错误"框
//     ⇒ 被 `MessageBoxA` 的恒 IDOK 压成"中止" ⇒ `WM_CLOSE` ⇒ 干净退出。
//   · 本文件登记**4 个实现**（它们优先于真实代码）：
//       `_GetInstance@0`  → **真实模块基址**（= HINSTANCE；guest 拿它当令牌传给 LoadIconA）
//       `_ShowDialog@12`  → ★ 2026-11-12 起**桥接成系统对话框**（`SDL_ShowMessageBox`）：只处理 `id==3`（关窗二次确认
//                            「終了」），其余选择子返回 1 + 一次 warn。⛔ 不读 AGERC 的 RT_DIALOG 资源
//                            （汉化件那份文本是乱码），文案在 `runtime/message_box.cpp` 的
//                            `makeCloseConfirmSpec()`。`REQ-01M72JFZ87DJNT52DW4HB8SEV6` stage 1+2。
//       `_OperateMenu@16` → 0（SCOPE §1，仍是 stub）
//       `_SetNameLenMax@20` → **按真实实现**（见下面 `shimAgercSetNameLenMax` 的长注释）
//   · **其余真实导出**（21 − 4 = 17 条，见 `data/requirements/01M75SDKZGY87B8SK7GBP92PXD.md`）
//     由 `agercResolveExport` 交给**运行期 unimplemented thunk**：地址非 0（真机 loader 也这么说），
//     真被调用时由 dispatcher **响亮失败**并打印是哪一条 —— ⛔ 不把未重定位的真实代码地址交给 guest。
// ★ 为什么不能"给 RVA 就能跑"（本单的硬证据）：AGERC 的 ImageBase `0x10000000` 被 guest 的**堆**
//   占着（`GuestLayout::heap_base`），所以它总被换基址到 `0x30000000`；而 `image.cpp` 明说
//   「固定 imageBase ⇒ 不做重定位」。`_SetNameLenMax@20` 的真实体里有一条**绝对**引用
//   （`0x100367F9 mov dword ptr [0x100A9000], ecx`），它在 `.reloc` 里是 HIGHLOW（RVA `0x367FB`）
//   ⇒ 直接在 `0x30000000` 跑它会把值写进**堆**（`0x100A9000` ∈ 堆区间）而不是 `0x300A9000`。
// ★ 它们分到唯一 guest 地址（shim 桩页；见 ShimRegistry::bind 的 extras 段 / addRuntime 的后续槽），
//   所以 guest 的 `call eax`（GetProcAddress 的返回值）与 `call [IAT]` 走同一条 dispatcher 路径。
// ★ ⛔ 不是"静默 no-op"：每个 thunk 都打一行 `[shim] agerc!...` 日志（SCOPE §3 第 3 条）。
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/audio.h"
#include "runtime/image.h"
#include "runtime/machine.h"
#include "runtime/message_box.h"
#include "runtime/present.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {

// =======================================================================================
// 真实导出表（宿主侧解析 PE export directory）
//
// ★ 声明为什么写在这里而不是 `register.h`：本缺陷单的输入边界只含
//   `kernel32_agerc.cpp` / `kernel32_process.cpp`（导出表那一段）/ `tests/loader.cpp`，
//   ⛔ 不含任何头文件。**签名真源就是本文件**；消费方（`kernel32_process.cpp` 的
//   GetProcAddress、`tests/loader.cpp` 的守卫）各自 `extern` 声明同一组函数。
// ⚠ 改签名 ⇒ 三处一起改（没有编译器替你连起来）。
// =======================================================================================
std::size_t agercExportCount(const PeImage& img);
std::string agercExportName(const PeImage& img, std::size_t index);
std::uint32_t agercExportRva(const PeImage& img, const char* name);
// GetProcAddress(AGERC, name) 的全部问答：返回 0 = **真实 PE 里没有这个导出**（真机同形 ⇒ 127）；
// 非 0 = 交给 guest 的地址（已登记实现的 thunk / 运行期 unimplemented thunk / 真实代码 base+RVA）。
std::uint32_t agercResolveExport(X86& cpu, const char* name);

namespace {

constexpr const char* kAgerc = "agerc";

// AGERC 真实导出表里的名字都带 stdcall 修饰（`_Name@N`，N = 参数字节数）。
// ⇒ `argc = N/4`：只用于 trace / 响亮失败时的诊断（thunk 本身不经手弹参）。
int argcFromExportName(const std::string& name) {
    const std::size_t at = name.rfind('@');
    if (at == std::string::npos || at + 1 >= name.size()) return 0;
    std::size_t i = at + 1;
    std::uint32_t bytes = 0;
    for (; i < name.size(); ++i) {
        const char c = name[i];
        if (c < '0' || c > '9') return 0;
        bytes = bytes * 10u + static_cast<std::uint32_t>(c - '0');
        if (bytes > 1024u) return 0;
    }
    return static_cast<int>(bytes / 4u);
}

bool equalsIgnoreCase(const std::string& a, const char* b) {
    const std::size_t n = b ? std::char_traits<char>::length(b) : 0;
    if (a.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + ('a' - 'A'));
        if (x != y) return false;
    }
    return true;
}

std::uint16_t rd16(const std::vector<std::uint8_t>& b, std::size_t off) {
    std::uint16_t v = 0;
    for (int i = 0; i < 2; ++i) v |= static_cast<std::uint16_t>(b[off + i]) << (8 * i);
    return v;
}

std::uint32_t rd32(const std::vector<std::uint8_t>& b, std::size_t off) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b[off + i]) << (8 * i);
    return v;
}

// RVA → 文件偏移（只认节表；头部按 RVA 当偏移；落在 .bss 段/文件外 ⇒ false）
bool rvaToOffset(const PeImage& img, std::uint32_t rva, std::uint32_t* out) {
    if (const PeSectionInfo* s = img.sectionOfRva(rva)) {
        const std::uint32_t delta = rva - s->va;
        if (delta >= s->raw_size) return false;  // 落在 .bss（文件里没有这些字节）
        const std::uint32_t off = s->raw_offset + delta;
        if (static_cast<std::size_t>(off) >= img.bytes.size()) return false;
        *out = off;
        return true;
    }
    if (rva < img.size_of_headers && static_cast<std::size_t>(rva) < img.bytes.size()) {
        *out = rva;
        return true;
    }
    return false;
}

// 读 RVA 处的 NUL 结尾 ASCII 串（导出名）。⛔ 读不到就 false，不返回"空串"让调用方猜。
bool readAsciiAt(const PeImage& img, std::uint32_t rva, std::string* out) {
    std::uint32_t off = 0;
    if (!rvaToOffset(img, rva, &off)) return false;
    out->clear();
    for (std::size_t i = off; i < img.bytes.size() && out->size() < 256; ++i) {
        const std::uint8_t c = img.bytes[i];
        if (c == 0) return !out->empty();
        if (c < 0x20 || c > 0x7E) return false;  // 导出名是 ASCII；别的字节说明 RVA 指错了
        out->push_back(static_cast<char>(c));
    }
    return false;  // 没有 NUL 终止 / 超长
}

struct ExportEntry {
    std::string name;
    std::uint32_t rva = 0;
};

// 解析 PE 的 export directory。返回 false + `err` 原因（⛔ 不许把"读不出"当成"没有导出"）。
bool parseExports(const PeImage& img, std::vector<ExportEntry>* out, std::string* err) {
    auto fail = [err](const char* why) {
        if (err) *err = why;
        return false;
    };
    const std::vector<std::uint8_t>& b = img.bytes;
    if (b.size() < 0x40) return fail("镜像字节不足（< 0x40）");
    const std::uint32_t pe = rd32(b, 0x3C);
    if (pe == 0 || static_cast<std::size_t>(pe) + 24 > b.size()) return fail("e_lfanew 越界");
    const std::uint32_t opt = pe + 24;
    if (static_cast<std::size_t>(opt) + 2 > b.size()) return fail("可选头越界");
    const std::uint16_t magic = rd16(b, opt);
    // DataDirectory 起点：PE32 = 可选头 +96，PE32+ = +112
    const std::uint32_t dd = opt + (magic == 0x20Bu ? 112u : 96u);
    if (static_cast<std::size_t>(dd) + 8 > b.size()) return fail("DataDirectory 越界");
    const std::uint32_t exp_rva = rd32(b, dd);
    const std::uint32_t exp_size = rd32(b, dd + 4);
    if (exp_rva == 0 || exp_size == 0) return fail("没有导出目录（DataDirectory[0] 为 0）");
    std::uint32_t dir_off = 0;
    if (!rvaToOffset(img, exp_rva, &dir_off)) return fail("导出目录的 RVA 落不到文件里");
    if (static_cast<std::size_t>(dir_off) + 40 > b.size()) return fail("导出目录被文件截断");

    const std::uint32_t n_funcs = rd32(b, dir_off + 20);
    const std::uint32_t n_names = rd32(b, dir_off + 24);
    const std::uint32_t funcs_rva = rd32(b, dir_off + 28);
    const std::uint32_t names_rva = rd32(b, dir_off + 32);
    const std::uint32_t ords_rva = rd32(b, dir_off + 36);
    if (n_names > 4096) return fail("NumberOfNames 异常（> 4096）");
    if (n_names == 0) return fail("NumberOfNames 为 0（没有按名导出）");
    std::uint32_t funcs_off = 0;
    std::uint32_t names_off = 0;
    std::uint32_t ords_off = 0;
    if (!rvaToOffset(img, funcs_rva, &funcs_off)) return fail("AddressOfFunctions 的 RVA 落不到文件里");
    if (!rvaToOffset(img, names_rva, &names_off)) return fail("AddressOfNames 的 RVA 落不到文件里");
    if (!rvaToOffset(img, ords_rva, &ords_off)) return fail("AddressOfNameOrdinals 的 RVA 落不到文件里");

    out->clear();
    out->reserve(n_names);
    for (std::uint32_t i = 0; i < n_names; ++i) {
        const std::size_t name_slot = static_cast<std::size_t>(names_off) + i * 4u;
        const std::size_t ord_slot = static_cast<std::size_t>(ords_off) + i * 2u;
        if (name_slot + 4 > b.size() || ord_slot + 2 > b.size()) return fail("导出名字表被文件截断");
        const std::uint16_t index = rd16(b, ord_slot);
        if (index >= n_funcs) return fail("AddressOfNameOrdinals 里的序号越界");
        const std::size_t func_slot = static_cast<std::size_t>(funcs_off) + static_cast<std::size_t>(index) * 4u;
        if (func_slot + 4 > b.size()) return fail("AddressOfFunctions 被文件截断");
        const std::uint32_t func_rva = rd32(b, func_slot);
        std::string name;
        if (!readAsciiAt(img, rd32(b, name_slot), &name)) return fail("导出名读不到");
        if (func_rva == 0) continue;  // 洞里（未使用的序号）
        if (func_rva >= exp_rva && func_rva < exp_rva + exp_size) {
            // 转发导出（RVA 指回导出目录 = 转发到别的 DLL 的函数）。
            shimLogWarn(kAgerc, "agercExport", "导出 \"%s\" 是**转发导出**（RVA 0x%X 落在导出目录内）"
                                                "⇒ 本 shim 不支持转发，按「没有实现」处理",
                        name.c_str(), func_rva);
            continue;
        }
        out->push_back(ExportEntry{name, func_rva});
    }
    if (out->empty()) return fail("导出目录里一条可用的按名导出都没有");
    return true;
}

// 解析 + （读不出时）打一行 warn。⛔ 读不出**不是**"没有导出"：调用方必须把两者分开报。
bool exportsOrWarn(const PeImage& img, std::vector<ExportEntry>* out, std::string* err) {
    if (parseExports(img, out, err)) return true;
    shimLogWarn(kAgerc, "agercExportTable",
                "读不出真实 AGERC 的导出表（%s）⇒ 导出问答退化为「只会说没有这个导出」",
                err ? err->c_str() : "?");
    return false;
}

// 运行期 unimplemented thunk 的理由串：真被调用时 dispatcher 会把它打出来。
constexpr const char* kUnimplementedExportReason =
    "AGERC 的真实导出：有地址（真机 loader 也这么说），但本模拟器没有实现它 —— 镜像被换基址"
    "（0x10000000→0x30000000，堆占了 ImageBase）而 `.reloc` 未应用，直接把真实代码地址交出去"
    "会静默写坏堆（见 src/shims/kernel32_agerc.cpp 文件头）";

// ---- 本文件登记的 4 个实现 ----
void shimAgercGetInstance(X86& cpu, ShimCall& call) {
    const std::uint32_t base = cpu.shim.agerc_base;
    if (base != 0) {
        shimLog(kAgerc, "_GetInstance@0",
                "返回**真实 AGERC 模块基址 0x%08X**（HINSTANCE；PE 已映射进 guest，资源从该镜像解析）",
                base);
        call.result = base;
        return;
    }
    // 没有镜像 ⇒ 真机上这个模块根本不存在 ⇒ 返回 0（⛔ 不再造一个假堆对象）。
    shimLog(kAgerc, "_GetInstance@0",
            "没有装载 AGERC 镜像（agerc_base == 0）⇒ 返回 0（⛔ 不造假实例；用 --agerc 指向拆壳件）");
    call.result = 0;
}

// ★ `_ShowDialog@12`：AGERC 的"选择子 → 对话框"入口（3 个参数 = 12 字节，stdcall）。
//   实测调用参数 `(id=3, hWndOwner, 0)`（`.lst` 的 `0x405976/0x405978/0x405979 push 0/ecx/3` +
//   运行期 trace；锚见 `REQ-01M72JFZ87DJNT52DW4HB8SEV6` §1/§2）。
//   返回语义（真机三臂）：1 = プログラムを終了する（→ 0x405B21，继续关窗）、
//   2 = タイトルに戻る（→ 0x405A82，返回 0 不关窗）、0 = キャンセル（→ 0x405A95，撤减光）。
//   ⛔ 文案不从 AGERC 资源取；按钮/默认键/Esc/X 见 `makeCloseConfirmSpec()`。
void shimAgercShowDialog(X86& cpu, ShimCall& call) {
    const std::uint32_t id = call.u32(0);
    const std::uint32_t owner = call.u32(1);
    const std::uint32_t reserved = call.u32(2);
    Machine* machine = cpu.machine;
    PresentSink* sink = nullptr;
    if (machine != nullptr && machine->presenter().hasSink()) {
        sink = machine->presenter().sink();
    }
    if (id != 3) {
        // ⛔ 只桥接关窗确认；AGERC 的其它对话框（设置等）仍是 stub（今天的行为 = 返回 1）。
        shimLogWarn(kAgerc, "_ShowDialog@12",
                    "id=%u（hwndOwner=0x%08X arg2=%u）不是关窗确认选择子 3 ⇒ 不弹框、返回 1（IDOK）"
                    "（SCOPE §3 的 stub 口径；⛔ 不读 AGERC 资源）",
                    id, owner, reserved);
        call.result = 1;
        return;
    }
    const CloseConfirmPolicy policy =
        machine != nullptr ? machine->closeConfirmPolicy() : CloseConfirmPolicy::Ask;
    if (const std::optional<std::uint32_t> forced = closeConfirmPolicyResult(policy)) {
        shimLogWarn(kAgerc, "_ShowDialog@12",
                    "id=3「終了」按 --close-confirm=%s ⇒ 不弹框、直接返回 %u（策略短路）",
                    closeConfirmPolicyName(policy), *forced);
        // ★ 2/0 = 不关窗 ⇒ 必须撤掉那次关窗请求（清 close_pending / 3 帧 / 500 ms deadline），
        //   否则模态期间已过期的 deadline 会在下一帧把 run 收掉（用户看到的"点 X 直接退出"）。
        if (*forced != 1 && sink != nullptr) sink->cancelCloseRequest();
        call.result = *forced;
        return;
    }
    const ModalSpec spec = makeCloseConfirmSpec();
    std::uint32_t result = 1;  // headless / feeder / 无 sink / 系统框失败 ⇒ 今天的行为（= 退出程序）
    if (cpu.audio != nullptr && cpu.audio->onFeederThread()) {
        shimLogWarn(kAgerc, "_ShowDialog@12",
                    "在音频 feeder 线程上被调用 ⇒ headless 语义：返回 1（⛔ 不碰窗口）");
    } else if (sink == nullptr) {
        shimLogWarn(kAgerc, "_ShowDialog@12",
                    "headless / 没有窗口 sink ⇒ 返回 1（IDOK = 退出程序；⛔ 不弹系统框、不阻塞）");
    } else {
        std::uint32_t shown = 1;
        if (sink->showSystemMessageBox(spec, &shown)) {
            result = shown;
            shimLogWarn(kAgerc, "_ShowDialog@12",
                        "id=3「終了」系统对话框 ⇒ 用户选择 %u（1=プログラムを終了する / "
                        "2=タイトルに戻る / 0=キャンセル）",
                        result);
        } else {
            shimLogWarn(kAgerc, "_ShowDialog@12",
                        "系统对话框起不来（SDL_ShowMessageBox 失败）⇒ 返回 1（今天的行为）");
        }
        // ★ 2/0 = 窗口留着：撤掉那次关窗请求（详见上面策略短路分支的注释）。
        //   1 = 終了：**保留** close_pending，guest 自己走关窗；宽限只作兜底（⛔ 不提前 requestStop）。
        if (result != 1) sink->cancelCloseRequest();
    }
    call.result = result;
}

void shimAgercOperateMenu(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 参数：hwnd / uMsg / wParam / lParam（4 个 = 16 字节，stdcall）
    shimLog(kAgerc, "_OperateMenu@16", "AGERC 菜单操作 no-op ⇒ 返回 0（SCOPE §1）");
    call.result = 0;
}

// `_SetNameLenMax@20` —— **按真实实现**（不是"返回 0 的空壳"）。
//
// 真实体（AGERC.DLL：export 20 的 RVA `0x32EF1` = `jmp 0x100367D0`；体在 `0x100367D0`，
// 5 个 dword 参数 / `ret 0x14` / 返回 `int`）：
//   0x100367EE  mov  eax, [ebp+0xC]        ; arg1（第 2 个参数，类型 `int*`）
//   0x100367F7  mov  ecx, [eax]            ; *arg1
//   0x100367F9  mov  dword ptr [0x100A9000], ecx   ; ★ AGERC 的 .data 全局 g_nameLenMax（初值 0x12）
//   0x100367FF  xor  eax, eax              ; ★ 返回值恒 0
//   0x10036807  retn 14h                   ; @20 = 5 个 dword（__stdcall）
// ⇒ 语义：**把 `*arg1` 存进 AGERC 的"名字最大长度"全局，返回 0**。其余 3 个参数没有被读。
//   那个全局只被 AGERC 自己的换行循环读（`0x100378D1` / `0x100379C4`：`g_nameLenMax * 0x12` 与
//   逐字宽度比较），而那条路径是 `_ShowDialog@12`（设置对话框仍未做；只有关窗确认 id==3 被桥接）
//   ⇒ 引擎观测不到这个副作用。
//
// ★ 为什么不把 RVA 交给 guest 跑真实代码（见文件头）：镜像总被换基址且 `.reloc` 未应用，
//   这条函数里唯一的绝对引用（`0x100367F9`，`.reloc` 的 HIGHLOW 在 RVA `0x367FB`）会指到 **堆**
//   （`0x100A9000`）。所以这里**按语义实现**：读 `*arg1`、记进宿主侧模型、返回 0。
//   ⚠ 与真机的唯一差别：不写 guest 里 AGERC 的那个全局（写它就是往一个当前没人读的地址猜写）。
static std::uint32_t g_agercNameLenMax = 0x12;  // 真实 .data 的初值（`dword_100A9000 dd 12h`）

void shimAgercSetNameLenMax(X86& cpu, ShimCall& call) {
    const std::uint32_t ptr = call.u32(1);  // arg1 = int*（stdcall：arg0 在 [esp+4]，arg1 在 [esp+8]）
    std::uint32_t value = 0;
    if (ptr != 0) {
        if (!cpu.mem) shimAbort(kAgerc, "_SetNameLenMax@20", "cpu.mem 为空（Machine 没装配 guest 内存）");
        // ★ 读之前先问"这 4 字节提交了吗"：指针如果是垃圾，真机在这里就是 AV（AGERC 的 bug）；
        //   本 shim 的**可观测契约**只有"地址非 0 + 返回 0"，⛔ 不值得为一条 stderr 日志把整轮跑停。
        //   ⇒ 读不到就**响亮地**说"没读到"，返回值照旧 0（⛔ 不是静默跳过）。
        if (cpu.mem->committed(ptr, 4)) {
            value = cpu.mem->rd32(ptr);
        } else {
            shimLogWarn(kAgerc, "_SetNameLenMax@20",
                        "arg1=0x%08X 那 4 字节没有提交（真机这里会 AV）⇒ 不读它，按未观测处理"
                        "（返回值仍是真实实现的 0）",
                        ptr);
        }
    }
    g_agercNameLenMax = value;
    shimLog(kAgerc, "_SetNameLenMax@20",
            "arg0=%u arg1(int*)=0x%08X arg2=%u arg3=%u arg4=%u ⇒ *arg1 = %u ⇒ AGERC 的 g_nameLenMax"
            "（真实实现写 .data VA 0x100A9000，初值 0x12）⇒ 返回 0（真实实现的返回值也是 0）",
            call.u32(0), ptr, call.u32(2), call.u32(3), call.u32(4), value);
    call.result = 0;
}

}  // namespace

// ---- 真实导出表：对外问答（声明见文件上方）----

std::size_t agercExportCount(const PeImage& img) {
    std::vector<ExportEntry> exports;
    std::string err;
    if (!exportsOrWarn(img, &exports, &err)) return 0;
    return exports.size();
}

std::string agercExportName(const PeImage& img, std::size_t index) {
    std::vector<ExportEntry> exports;
    std::string err;
    if (!exportsOrWarn(img, &exports, &err)) return {};
    if (index >= exports.size()) return {};
    return exports[index].name;
}

std::uint32_t agercExportRva(const PeImage& img, const char* name) {
    std::vector<ExportEntry> exports;
    std::string err;
    if (!exportsOrWarn(img, &exports, &err)) return 0;
    for (const ExportEntry& e : exports)
        if (equalsIgnoreCase(e.name, name)) return e.rva;
    return 0;
}

std::uint32_t agercResolveExport(X86& cpu, const char* name) {
    const PeImage* img = cpu.shim.agerc_image;
    if (img == nullptr || img->bytes.empty()) return 0;  // 没有镜像 ⇒ 连"有哪个导出"都答不上
    std::vector<ExportEntry> exports;
    std::string err;
    if (!exportsOrWarn(*img, &exports, &err)) return 0;

    const std::string want = name ? name : "";
    const ExportEntry* hit = nullptr;
    for (const ExportEntry& e : exports) {
        if (equalsIgnoreCase(e.name, want.c_str())) {
            hit = &e;
            break;
        }
    }
    if (hit == nullptr) return 0;  // 真实 PE 里没有 ⇒ GetProcAddress 走 127（真机同形）

    const std::uint32_t base = cpu.shim.agerc_base;
    // ★ 镜像就在 PE 头的 ImageBase ⇒ 绝对地址**不需要**重定位 ⇒ 直接把真实代码交给 guest
    //   （这才是真机 loader 的答案）。换过基址时下面那条路走 thunk。
    if (base != 0 && base == img->image_base) {
        const std::uint32_t ea = base + hit->rva;
        shimLog(kAgerc, "GetProcAddress",
                "AGERC!%s ⇒ **真实代码 0x%08X**（RVA 0x%08X；模块就在 ImageBase ⇒ 无需重定位）",
                hit->name.c_str(), ea, hit->rva);
        return ea;
    }

    // ★ 换过基址 + `.reloc` 未应用 ⇒ ⛔ 不交真实代码地址（会静默写坏堆，见文件头）。
    //   交一个**运行期 unimplemented thunk**：地址非 0（导出确实存在），真被调用时响亮失败。
    const int argc = argcFromExportName(hit->name);
    if (cpu.shims == nullptr) {
        shimLogWarn(kAgerc, "GetProcAddress",
                    "AGERC!%s 是真实导出，但没有 shim 注册表（cpu.shims == nullptr）⇒ 只能返回 0（127）",
                    hit->name.c_str());
        return 0;
    }
    ShimDef* rt = cpu.shims->addRuntime(kAgerc, hit->name.c_str(), Conv::Stdcall, argc, nullptr);
    if (rt == nullptr) {
        shimLogWarn(kAgerc, "GetProcAddress",
                    "AGERC!%s 是真实导出，但拿不到运行期 unimplemented thunk（桩页槽位耗尽？）"
                    "⇒ 返回 0 + 127",
                    hit->name.c_str());
        return 0;
    }
    rt->reason = kUnimplementedExportReason;  // 真被调用时 dispatcher 会打出来
    if (cpu.cache != nullptr) cpu.cache->installShim(rt->stub_ea, rt->id);
    shimLog(kAgerc, "GetProcAddress",
            "AGERC!%s ⇒ **运行期 unimplemented thunk 0x%08X**（真实导出，RVA 0x%08X；镜像被换基址"
            " 0x%08X→0x%08X 且 `.reloc` 未应用 ⇒ 不把未重定位的代码地址交给 guest；调用它会响亮失败）",
            hit->name.c_str(), rt->stub_ea, hit->rva, img->image_base, base);
    return rt->stub_ea;
}

// ---- 注册（4 个实现）----
void registerAgercShims(ShimRegistry& s) {
    // ★ stdcall 的弹参字节数由 argc 算（0/3/4/5 ⇒ 0/12/16/20），与导出名里的 @N 一致。
    s.addStub("agerc", "_GetInstance@0", Conv::Stdcall, 0, &shimAgercGetInstance,
              "AGERC 设置对话框仍属 SCOPE §1 的未做范围；_GetInstance 返回**真实模块基址**（HINSTANCE）");
    // ★ `_ShowDialog@12` 已不是 stub：id==3 桥接成系统「終了」对话框（REQ-01M72JFZ87DJNT52DW4HB8SEV6），
    //   其余选择子返回 1 + warn（见 shimAgercShowDialog）。
    s.add("agerc", "_ShowDialog@12", Conv::Stdcall, 3, &shimAgercShowDialog);
    s.addStub("agerc", "_OperateMenu@16", Conv::Stdcall, 4, &shimAgercOperateMenu,
              "SCOPE §1：AGERC 菜单操作（_OperateMenu@16）仍是 stub ⇒ 返回 0");
    // ★ REQ-01M75SDKZGY87B8SK7GBP92PXD：SAVE.BIN 的 COMMAND=332 取的就是这一条。真实实现的
    //   语义 = 存 `*arg1` 进 AGERC 的 g_nameLenMax、返回 0（见 shimAgercSetNameLenMax 的长注释）。
    s.addStub("agerc", "_SetNameLenMax@20", Conv::Stdcall, 5, &shimAgercSetNameLenMax,
              "按真实实现（AGERC export 20 / RVA 0x32EF1 → 体 0x100367D0）的语义：存 *arg1 进 "
              "g_nameLenMax、返回 0；镜像换基址且 .reloc 未应用 ⇒ 不跑真实代码（见 kernel32_agerc.cpp）");
}

}  // namespace lift
