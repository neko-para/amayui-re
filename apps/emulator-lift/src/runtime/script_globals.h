// emulator-lift · **AGE 引擎层：script global 的池布局 / 类型族 / ENC-DEC 读写**
//
// ⚠ 是否被其它 AGE 作品复用 = **待证**（需要第二部 AGE 的二进制或脚本来证）；本模块只承诺
//   "**可替换**"，不承诺"跨作品通用"。
// ★ 它是**与传输无关**的（不依赖 JSON / WebSocket / control 端点）—— 那只是**依赖方向**，
//   不是"跨引擎通用"的声明。放在 `src/runtime/` 是位置，不是"通用"的证明。
//
// ★ 这是什么（父级 2026-11-19 裁决）：script global 的「寻址 + ENC/DEC + 类型编码」只有这一份实现。
//   控制层（`global-get` / `global-set`）现在调它；QJS 侧将来也调**同一份引擎层实现**
//   （见 report 的 `## QJS 侧将来怎么接`）。⛔ 本模块里**没有** JSON / WebSocket / control 端点 /
//   `emu-ctl` / 暂停 / `serve()` 概念 —— 它只认 `GuestMemory` + `id` + `GlobalKind`。
//   ⇒ 将来要支持别的 AGE 作品 / 别的引擎，替换的正是**这一层**（上层不用动）。
//
// ★ 存储面 = **guest 内存**（AOT 臂没有宿主数组；见 `.tmp/emu-ctl-globals/analysis.md` §2.1）：
//     engine = rd32(kEngineSingletonGlobal)              // 0 ⇒ 未发布
//     base   = rd32(engine + 0x5D800 / 0x5D808)          // **每次现读**（⛔ 不缓存：`0x6d` 会整体对调）
//     addr   = base + 4 * id                             // int / float 都是 4 B
//     key    = rd32(engine + 0x5EC8C)
//     int/uint：槽里是 ENC(value, key)（**⛔ 不是明文**）；float：槽里就是 IEEE-754 裸 32 位。
//
// ★ id 的语义：**raw 池下标**（脚本 token `(global-int b1b6)` 里的 `b1b6` 就是 hex raw，不是名字）；
//   ⛔ 没有「名 → id」映射。池容量是**运行期状态**（`Engine+0x5D7E8/0x5D7EC` 的计数），
//   可寻址范围 = `0..count`（含 count；fill 循环按 `count + 余量 + 1` 分配，详见 analysis §2.2）。
//
// ★ 哑原语纪律：⛔ 不猜类型（kind 由调用方给）、⛔ 不挑时机、⛔ 不重试、⛔ 不写后校验、
//   ⛔ 不为任何具体 global / 选项开特例。
//
// ★ 分层：`src/runtime/**` 不依赖 quickjs / `src/ui/**`；`src/ui/**` 可以依赖它。
//   ⚠ 这条只是**依赖方向**（runtime 不反向依赖 UI）；⛔ 不构成"跨引擎通用"的声明。
#pragma once

#include <cstdint>

namespace lift {

class GuestMemory;

// =====================================================================================
// 存储几何（EA / spec 锚；常量集中放一处 ⇒ 测试夹具与实现用同一份偏移）
// =====================================================================================
// `packages/age-format/src/engine/layout.mts:29-38` 的 `GLOBAL_SLOTS`；取址 EA `0x42AF2F..35`
// （`layout.mts:260` 的 `mov edx,[esi+5D800h]` / `lea eax,[edx+ecx*4]`）。
inline constexpr std::uint32_t kScriptGlobalIntBaseOffset = 0x5D800;    // 全局整型池当前基址
inline constexpr std::uint32_t kScriptGlobalFloatBaseOffset = 0x5D808;  // 全局浮点池当前基址
inline constexpr std::uint32_t kScriptGlobalIntCountOffset = 0x5D7E8;   // int 计数槽
inline constexpr std::uint32_t kScriptGlobalFloatCountOffset = 0x5D7EC; // float 计数槽
inline constexpr std::uint32_t kScriptGlobalIntKeyOffset = 0x5EC8C;     // int 编码 key（唯一写点 EA 0x417359）
inline constexpr std::uint32_t kScriptGlobalSlotBytes = 4;              // int / float 元素宽度

// =====================================================================================
// 类型 / 错误 / 值
// =====================================================================================
// ★ 引擎的槽里**没有运行期类型标签**（标签在脚本操作数里，不在槽里：`40-pools.md:12, 52`）
//   ⇒ kind 只能是**调用方断言**；本模块能校验的是「请求值是否符合这个断言」，不是「槽是不是这个类型」。
//   `String` 保留枚举值但本实现**未实现**（记录布局不在 spec；见 analysis §5.5）⇒ 返回 TypeMismatch。
enum class GlobalKind : std::uint8_t { Int = 0, Uint = 1, Float = 2, String = 3 };

// ⛔ 协议错误 reason 只有这三项（`None` = 没错误）。调用方负责把它们翻成自己的传输错误码；
//   本模块不认识 HTTP / WebSocket / JSON。
enum class GlobalError : std::uint8_t { None = 0, IdOutOfRange, TypeMismatch, EncodeFailed };

const char* globalKindName(GlobalKind kind);   // "int" / "uint" / "float" / "string"
const char* globalErrorName(GlobalError err);  // "none" / "id_out_of_range" / "type_mismatch" / "encode_failed"
// 给人看的一句话（**静态字面量**；调用方决定是否把它放进 message）。⛔ 客户端不许按它分支。
const char* globalErrorDetail(GlobalError err);

// 一次读 / 写携带的值。**只含值**（没有 guest 指针）⇒ 可跨线程拷贝。
//   · int / uint：`u32` = 32 位位模式（写：调用方填；读：模块填 DEC(raw)）
//   · float     ：`f64` = 语义值（写：调用方填；读：模块填 IEEE 位模式 → double）
//   · `raw`     ：槽里的**原始 32 位**（读：模块填；写：忽略）—— 诊断 / 逐位核对的唯一证据
struct GlobalValue {
    GlobalKind kind = GlobalKind::Int;
    std::uint32_t index = 0;  // 读：模块回显的 id（写：忽略）
    std::uint32_t u32 = 0;
    double f64 = 0.0;
    std::uint32_t raw = 0;
};

// =====================================================================================
// 读写（每次调用都重新读 Engine / 池基址 / 计数 / key；⛔ 无缓存）
// =====================================================================================
// 成功 ⇒ true 且 `*err = None`；失败 ⇒ false 且 `*err` ∈ {IdOutOfRange, TypeMismatch, EncodeFailed}。
//   · IdOutOfRange = engine 未发布 / 池基址为 0 / `id > 运行期计数` / 地址回绕或未提交；
//   · TypeMismatch = kind = String（第二期未实现）或写入值的 kind 与实参不符；
//   · EncodeFailed = int/uint 的 key 读不到（无法编码 / 解码）。
bool readGlobal(const GuestMemory& mem, std::uint32_t id, GlobalKind kind, GlobalValue* out, GlobalError* err);
bool writeGlobal(GuestMemory& mem, std::uint32_t id, GlobalKind kind, const GlobalValue& value, GlobalError* err);

// float 的**裸 32 位**写（NaN / -0.0 这类逐位往返；JSON 层走 `writeGlobal` 的 `f64` 那条）。
bool writeGlobalFloatBits(GuestMemory& mem, std::uint32_t id, std::uint32_t bits, GlobalError* err);

}  // namespace lift
