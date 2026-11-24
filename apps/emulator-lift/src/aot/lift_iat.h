// emulator-lift · S7/IAT：`call *<IAT 槽>` 的**栈清理字节**（生成期专用）
//
// ★ 为什么要这张表（REQ-01M86NMT0YJ9V78XS2DZZV364Q 的 design §1 / §2）：
//   间接 `call` 之后 esp 能不能继续记账，取决于**callee 在返回地址之外多弹了多少字节**。
//   静态上那件事只有两种来源：① callee 的 `retn imm16`（跨单元已知时已在用，见
//   `lift_struct.cpp` 的 ①）；② **导入槽** —— 槽里是宿主 shim，它的清理字节由
//   `ShimDef::conv/argc` 决定（运行期 `invokePrepared` 的 `stdcall_bytes`）。
//   本文件做 ②：build 期把 PE 导入表 × `ShimRegistry` 交叉成 `槽 EA → IatRow`。
//
// ★ 单一真源：`cleanup = (conv == Conv::Stdcall) ? argc*4 : 0`
//   —— 与 `src/runtime/shims.cpp` 的 `invokePrepared` **同一条式子**（`ShimCall::stdcall_bytes`）。
//   ⇒ ⛔ 不按微软文档手写 `导入名 → conv/argc` 表（那会是第二份真源，漂了没人发现）；
//   ⛔ 也不解析 `add(...)` 的源码（跨行 / 宏 / 重载 / `registerImportGaps` 是生成物 ⇒ 必漂）。
//
// ★ 这是**生成期**的表：运行期**不用**它（运行期的事实源仍是 `ShimRegistry` 自己）。
//   `buildIatModel` 是纯函数：不读 `.text` / 不读 `.lst` / 不跑 `bind()` / 不发 stub EA。
//   ★ 为什么**不**跑 `bind()`：那需要 `GuestMemory` + 桩页，而且会让结果依赖"stub EA 的
//   发号顺序"（本机制**不需要** stub EA —— 判据只有槽 EA 与清理字节）。
//
// ★ `modelled == false` 的三种情形（都必须保守 ⇒ 调用点仍把 esp 标成未知）：
//   ① 该导入在注册表里查不到（`bind()` 现场补的"裸缺口"）或 `fn == nullptr`（执行即 trap
//      ⇒ 它不是一个"会返回的 callee"）；
//   ② 该 shim 会 `take_control`（自己安排下一条 esp / eip，`invokePrepared` **不**弹参）；
//   ③ 不是"纯绝对地址的 IAT 槽"（寄存器 / 基址 / 段前缀 / 表里没有的绝对地址）——
//      这一条由 `iatOf()` 返回 `nullptr` 表达，不建行。
#pragma once

#include <cstdint>
#include <map>

#include "runtime/insn.h"
#include "runtime/shims.h"

namespace lift {
class ShimRegistry;
struct PeImage;
namespace aot {

// 一个导入槽的模型行。`slot_ea` 是**镜像里那 4 字节**的 guest EA。
struct IatRow {
    std::uint32_t slot_ea = 0;  // image_base + PeImportInfo::iat_rva（image.h 的 :32 + :48）
    std::uint32_t cleanup = 0;  // (conv == Conv::Stdcall) ? argc*4 : 0（= shims.cpp 的 stdcall_bytes）
    Conv conv = Conv::Stdcall;  // 只进报告 / 诊断（判据只用 cleanup）
    std::int32_t argc = 0;      // 同上
    bool modelled = false;      // false ⇒ 运行期会 trap 或 take_control ⇒ 调用点必须仍取 kUnknown
};

using IatModel = std::map<std::uint32_t, IatRow>;

// 纯函数：PE 导入表 × 注册表。（名字归一与 `ShimRegistry::bind()` 逐字相同：
// 按序号导入的条目在注册表里叫 `#<n>`，模块名 / 函数名大小写不敏感。）
IatModel buildIatModel(const ShimRegistry& shims, const PeImage& img);

// 读数（`[shim-meta]` 行；⛔ 只进读数、不进判据）：`imports` = 导入条目数，
// `modelled` = 可建模的槽数，`unmodelled` = 其余（缺口 / take_control）。
struct IatModelStats {
    std::uint64_t imports = 0;
    std::uint64_t modelled = 0;
    std::uint64_t unmodelled = 0;
};

inline IatModelStats iatModelStats(const IatModel& m) {
    IatModelStats s;
    s.imports = m.size();
    for (const auto& kv : m) {
        if (kv.second.modelled) ++s.modelled;
        else ++s.unmodelled;
    }
    return s;
}

// `call` 的操作数 → 模型行。判据**全部**来自 `Operand` / `MemOperand`：
//   ⛔ 不看助记符、⛔ 不看 `.lst`、⛔ 不看 EA 的数值是不是"像导入"。
// 只接受"纯绝对地址"（`absolute` ⇒ base/index 都空，见 `runtime/decode.cpp` 的 :427），
// 且不带 fs/gs 段前缀 —— 其余（`call *%eax` / `call *0x10(%ecx)` / `fs:`）都是运行期才知道的目标。
inline const IatRow* iatOf(const Operand& op, const IatModel& m) {
    if (op.kind != OpKind::Mem) return nullptr;  // 寄存器间接
    const MemOperand& mo = op.mem;
    if (!mo.absolute || mo.index != Reg::None || mo.seg_fs || mo.seg_gs) return nullptr;
    const auto it = m.find(mo.disp);
    return it == m.end() ? nullptr : &it->second;
}

}  // namespace aot
}  // namespace lift
