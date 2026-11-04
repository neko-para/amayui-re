// emulator-lift · shim 框架（每个导入一个**唯一 guest 地址**）
//
// ★ 机制（S5）：导入表里每个导入分到桩页里的一个唯一地址（4 字节步进），IAT 槽写这个地址。
//   guest 的 `call dword ptr [IAT]` 于是落到 dispatcher → 块缓存里那个 EA 是 **shim 块** →
//   调用宿主函数，然后按调用约定弹参 + 回到返回地址。
//   ⇒ `call [IAT]` 与 `call 0x401234` 在 dispatcher 眼里是同一件事（README §2.4 的 table[EA]）。
// ★ 调用约定：stdcall 由被调方弹参（`ret N`），cdecl / varargs 由调用方弹（`ret`）——
//   ⛔ 不许"统一按 stdcall 弹"：那会让 cdecl 的栈每调一次涨 4 字节，几百次后炸在别处。
// ★ 未实现的导入**也分到唯一地址**，执行到就打印 module!name + EA + guest 返回栈并停止，
//   同时记进"基座缺口"清单（A5）。⛔ 不许静默 no-op（SCOPE §3）。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace lift {

struct X86;
struct PeImage;
class GuestMemory;

enum class Conv : std::uint8_t { Stdcall = 0, Cdecl, Varargs };

const char* convName(Conv c);

// 一次 shim 调用的现场。参数在 guest 栈上：`[esp]` = 返回地址，`[esp+4+4i]` = 第 i 个参数。
struct ShimCall {
    X86* cpu = nullptr;
    Conv conv = Conv::Stdcall;
    int argc = 0;                 // 声明的参数个数（仅用于弹参与诊断）
    std::uint32_t result = 0;     // 返回值（EAX）
    std::uint32_t ret_ea = 0;     // 返回地址（dispatcher 在调用前弹出）
    std::uint32_t stdcall_bytes = 0;  // 0 ⇒ argc*4
    // ★ `take_control = true` ⇒ **shim 自己接管下一条 esp / eip**，invoke 不再弹参、不再写 eip
    //   （返回值仍照常写 EAX）。只有"在 shim 里调用 guest 回调"这一种情形需要它：
    //   `EnumSystemLocalesA` 把控制权交给 guest 回调，guest 返回后落到内部续延桩
    //   （见 kernel32_string.cpp）。⛔ 普通实现不要设它 —— 设了就绕过了调用约定。
    bool take_control = false;

    // 读参数（i 从 0 起）
    std::uint32_t u32(int i) const;
    std::int32_t i32(int i) const { return static_cast<std::int32_t>(u32(i)); }
    std::uint32_t ptr(int i) const { return u32(i); }
    std::uint16_t u16(int i) const;
    // 读 guest C 字符串（NULL ⇒ 空串；越界即 fault，不静默截断）
    std::string str(int i) const;
    // 读 guest UTF-16 字符串
    std::u16string wstr(int i) const;
    // 读 guest 缓冲区（n 字节）
    std::vector<std::uint8_t> bytes(int i, std::size_t n) const;
};

using ShimFn = void (*)(X86& cpu, ShimCall& call);

struct ShimDef {
    std::string module;
    std::string name;
    Conv conv = Conv::Stdcall;
    int argc = 0;
    ShimFn fn = nullptr;            // nullptr ⇒ 未实现（执行即响亮失败）
    const char* reason = nullptr;   // 未实现 / stub 的理由（SCOPE 偏差必须点名）
    std::uint32_t stub_ea = 0;      // 唯一 guest 地址
    std::uint32_t id = 0;           // 从 0 起的稳定序号（trace 用）
    bool imported = false;          // 这个导入在本镜像的导入表里出现过
    bool declared = false;          // true = host 侧**显式**登记过（add / addUnimplemented）
                                    // false = 导入表里有、但我们连一行登记都没有（bind 现场补的）
    bool called = false;
    std::uint64_t call_count = 0;
};

class ShimRegistry {
public:
    void setTrace(bool on, std::FILE* out) { trace_ = on; trace_out_ = out ? out : stdout; }
    bool trace() const { return trace_; }

    // ---- host 侧声明 ----
    void add(const char* module, const char* name, Conv conv, int argc, ShimFn fn);
    void addUnimplemented(const char* module, const char* name, const char* reason);
    // ★ 有实现、但**按 SCOPE 的偏差整体 stub**（例：AGERC 的三个导出 thunk）。
    //   `fn` 非空（会返回合理值 + 打一行日志），`reason` 非空（覆盖清单里 kind=stub）。
    //   `stub_ea` 在 bind() 里分配（与导入同一个桩页；它不是导入，所以 IAT 里不会出现）。
    void addStub(const char* module, const char* name, Conv conv, int argc, ShimFn fn, const char* reason);

    // ---- 绑定：给镜像里每个导入分唯一地址，并把地址写回 IAT 槽 ----
    // `shim_page` / `shim_slots`：桩页起点与槽数（GuestLayout 提供）
    bool bind(GuestMemory& mem, const PeImage& img, std::uint32_t shim_page, std::uint32_t shim_slots,
              std::string* err);

    // ---- 查询 ----
    const ShimDef* find(const char* module, const char* name) const;
    ShimDef* findMutable(const char* module, const char* name);
    const ShimDef* byStubEa(std::uint32_t ea) const;
    const ShimDef* byId(std::uint32_t id) const;
    const std::vector<ShimDef>& all() const { return defs_; }
    std::size_t size() const { return defs_.size(); }

    // ---- 缺口报告（A5）----
    std::vector<const ShimDef*> importedButUnimplemented() const;
    std::vector<const ShimDef*> calledUnimplemented() const;
    std::size_t importedCount() const;

    // ---- 执行（dispatcher 调用）----
    // 走进一个 shim 块：弹返回地址 → 调用实现（或响亮失败）→ 按约定弹参
    void invoke(X86& cpu, ShimDef& def);

private:
    std::vector<ShimDef> defs_;
    bool trace_ = false;
    std::FILE* trace_out_ = stdout;
};

}  // namespace lift
