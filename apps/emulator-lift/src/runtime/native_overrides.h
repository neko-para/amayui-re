// emulator-lift · guest 函数级 native override（试点原型）
//
// ★ 口径（REQ-01M6NZSAAD0WCNBE3RKMFF84R5）：
//   - 只在 **dispatcher 的 block 入口**按 EA 拦截（不 patch guest 字节 ⇒ 天然避开 SMC / 代码页保护）；
//   - native 实现只能经 `GuestMemory` 读写 guest；⛔ 宿主指针不进 guest 状态；
//   - 返回后由本框架按 ABI 设置 `EAX` / `ESP` / `EIP`，未覆盖路径照旧走解释器；
//   - 每一项都**默认关闭**，由 `--native-overrides=<list>` 显式启用。
// ★ 这里只放"机制 + 少量 CRT 叶子"；候选由 profile 选出，不先猜（见 requirement 的测试计划）。
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/guest_call.h"

namespace lift {

struct X86;
class GuestMemory;

enum class GuestConv : std::uint8_t {
    Cdecl = 0,    // 调用方清栈
    Stdcall = 1,  // 被调用方清栈
    Thiscall = 2, // ECX = this；栈参数由被调用方清（MSVC x86）
};

const char* guestConvName(GuestConv c);

// native 实现：读参数（cpu.arg(i) / cpu.ecx()），经 mem 访存，写 *result（= 返回 EAX）。
// 返回 false ⇒ 本次**不做** override，由 dispatcher 回退解释器（例如 safe mode 下参数越界）。
using NativeFn = bool (*)(X86& cpu, GuestMemory& mem, std::uint32_t* result);

struct NativeOverride {
    std::uint32_t ea = 0;
    std::string name;
    GuestConv conv = GuestConv::Cdecl;
    std::uint32_t arg_bytes = 0;  // 栈参数总字节（不含返回地址；thiscall 不含 ECX）
    NativeFn fn = nullptr;
    std::uint64_t hits = 0;
    std::uint64_t fallbacks = 0;
};

class NativeOverrideTable {
public:
    static constexpr std::size_t kMax = 8;

    bool add(std::uint32_t ea, const std::string& name, GuestConv conv, std::uint32_t arg_bytes, NativeFn fn,
             std::string* err);
    void clear();
    bool empty() const { return count_ == 0; }
    std::size_t size() const { return count_; }
    NativeOverride* at(std::size_t i) { return i < count_ ? &entries_[i] : nullptr; }
    const NativeOverride* at(std::size_t i) const { return i < count_ ? &entries_[i] : nullptr; }

    NativeOverride* find(std::uint32_t ea);
    const NativeOverride* find(std::uint32_t ea) const;

    // 执行一次命中：成功 ⇒ 设好 EAX / EIP / ESP 并返回 true；false ⇒ 原因写进 *why（调用方回退解释器）。
    // suspended=true ⇒ native 实现已经用 startNestedGuestCall() 铺了一个 guest 回调帧，
    //   EIP/ESP 已由 GuestCallStack 设置，调用方必须直接 continue（⛔ 不要回退解释器）。
    bool invoke(NativeOverride& ov, X86& cpu, GuestMemory& mem, const char** why, bool* suspended = nullptr);

private:
    std::array<NativeOverride, kMax> entries_{};
    std::size_t count_ = 0;
};

// 已知的 CRT 叶子（按名字解析 EA；EA 来自 `.lst` 的 `proc near` / `COLLAPSED FUNCTION` 行）。
// ★ 分层：`debug == false` 是**稳定**项，`--native-overrides all` 只含这一层；
//   `debug == true` 是保留用于复现/调试的原型，必须**显式点名**才能启用
//   （例如 `--native-overrides all,某个debug项`），⛔ 不会被 `all` 带进来。
struct KnownNative {
    const char* name;
    GuestConv conv;
    std::uint32_t arg_bytes;
    NativeFn fn;
    bool debug = false;
};
const KnownNative* findKnownNative(const std::string& name);
std::size_t knownNativeCount();
const KnownNative& knownNativeAt(std::size_t i);

// 具体实现（也直接给测试用）
bool nativeMemcpy(X86& cpu, GuestMemory& mem, std::uint32_t* result);
bool nativeMemset(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_401000` 的语义是**符号**（`.lst` 401066: `sar eax,1Fh; or eax,1` ⇒ -1/0/+1），
// 不是字节差。`_memcmp` 的 CRT 实现是 COLLAPSED（没有可观察字节）⇒ 暂不覆盖。
bool nativeSignMemcmp(X86& cpu, GuestMemory& mem, std::uint32_t* result);
bool nativeStrlen(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_436A80`：S4IC / LZSS 解压（profile 选出的 boot 第一热点）。
bool nativeLzss436A80(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_48B090`（NormalTexture::UploadImage）：thiscall（ECX = NormalTexture），栈参数 (img, alpha, key)。
// 准入失败（不支持的源格式 / 色键未实现 / 目标函数认不出）⇒ 返回 false，dispatcher 回退解释器。
bool nativeUpload48B090(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// 纯解码核（测试直接用；src/comp = 输入，dst/out = 输出，ring = 4096 B 字典）。
bool lzssDecode(const std::uint8_t* src, std::uint32_t comp_size, std::uint8_t* dst, std::uint32_t out_size,
                std::uint8_t ring[4096]);
// sub_48B090 的像素核（纯函数：只碰宿主缓冲，测试可以直接用）。
// ★ 语义逐字照 `.lst` 的 `sub_48B090`：
//   - 源是 **bottom-up** DIB（源 row = height-1-目标 row），目标是 **top-down**（行按 `dst_pitch` 前进）；
//   - `alpha`（可为空）**按目标行序线性消费**（guest 在行循环里只 `inc` 指针、不重置）
//     ⇒ index = row*width+col；32bpp 源**不读** alpha（用源像素自带的第 4 字节）；
//   - 每个像素的 32 位值就是内存里的字节序（R 在最低字节、A 在最高字节）；
//   - 8bpp 的调色板项 = 4 字节步长、只用前 3 字节（byte0→bit0 / byte1→bit8 / byte2→bit16），
//     没有 alpha 时补 0xFF。
// 4bpp / 1bpp / 16bpp **不支持**（本线没有证据级用例；`convertImageTo32` 显式拒绝）。
void convertPal8To32(const std::uint8_t* src, std::uint32_t stride, std::uint32_t w, std::uint32_t h,
                     const std::uint8_t* pal, std::uint8_t* dst, std::uint32_t pitch, const std::uint8_t* alpha);
void convert24To32(const std::uint8_t* src, std::uint32_t stride, std::uint32_t w, std::uint32_t h,
                   std::uint8_t* dst, std::uint32_t pitch, const std::uint8_t* alpha);
// 32bpp 源：**逐字节搬运**（guest 0x48B3xx 的 default/32 分支就是 `memcpy(dst, &Src, 4)`），
// alpha 参数不存在 —— 源像素的第 4 字节就是目标 alpha。
void convert32To32(const std::uint8_t* src, std::uint32_t stride, std::uint32_t w, std::uint32_t h,
                   std::uint8_t* dst, std::uint32_t pitch);

// 源像素描述（宿主指针；由调用方从 guest 内存里解析出来）。
struct ImageSource {
    std::uint32_t bpp = 0;                  // 8 / 24 / 32
    const std::uint8_t* pixels = nullptr;   // 源像素首字节（行按 stride、bottom-up）
    std::uint32_t stride = 0;               // 源行字节数（DIB 的 4 字节对齐）
    const std::uint8_t* palette = nullptr;  // 8bpp 才用：256 项 × 4 字节
    const std::uint8_t* alpha = nullptr;    // 8/24bpp 才用：h*w 字节，按目标行序；32bpp 忽略
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
// 校验 + 分发到上面三个核。返回 false + why（**不写** dst 之外的任何东西）。
bool convertImageTo32(const ImageSource& src, std::uint8_t* dst, std::uint32_t dst_pitch, std::string* why);

// 把源像素写进一个**已存在**的纹理：走 `d3d9::lockRect` / `unlockRect`（与 guest 0x48B234 /
// 0x48B06C 同一路径），所以 pitch / pBits / 锁定计数 / 已锁拒绝都是那一份语义。
// ★ 前置条件（guest 侧的观察，不是本函数编的）：目标必须是 32bpp、尺寸与 src 相符。
// ★ 失败时不写像素、并保证**不留下锁**（除非锁本来就不是我们加的）。
bool uploadPixelsToTexture(X86& cpu, std::uint32_t texture_object, const ImageSource& src, std::string* why);

// 从 native override 里铺一个 guest 回调帧（必须由 NativeOverrideTable::invoke 的 suspended 机制接住）。
// `callee_ecx` = 进入 callee 前要写进 ECX 的 `this`（thiscall）；0 = 不动。
bool startNestedGuestCall(X86& cpu, std::uint32_t fn, const std::vector<std::uint32_t>& args,
                          std::uint32_t cleanup_bytes, const char* api, GuestContinuation cont, std::string* err,
                          std::uint32_t callee_ecx = 0);

}  // namespace lift
