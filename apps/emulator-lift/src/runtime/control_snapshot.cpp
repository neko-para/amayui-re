// emulator-lift · 控制层快照的**主线程取数**（`sample` 的 script 块）
//
// ★ 位置读取**只能**走 `readScriptPositionSample`（`runtime/engine_marker.h` 的**唯一一份**公式）：
//   本文件不重写 `pos = header_len + (pc - base)`，也不从 guest 头里读 `sub_header_length`
//   （头在 guest 内存里没有落点 ⇒ `kScriptHeaderLen` 是编译期常量，见 `engine_marker.h` 的头注）。
// ★ `script_sha` 走**宿主文件**：guest 侧拿不到脚本体长度（`engine_marker.h:50` 只有上界），
//   所以"对 guest 后缓冲哈希"不可实现（`design-review.md` §8-1）。解析 / 读失败 ⇒ `sha_why` 枚举，
//   ⛔ 不猜、不静默回一个空 sha 当"没有内容"。
// ★ 本文件**不碰** GPU / presenter / SDL：pure CTest 用 `GuestMemory` + 假 Vfs 就能断言
//   "boot 前 ⇒ available:false + why ∈ 既有 8 个字面量"（判据 3d）与 `header_len` / `pos % 4`。
#include "runtime/control_snapshot.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "runtime/engine_marker.h"
#include "runtime/memory.h"
#include "runtime/sha256.h"
#include "runtime/vfs.h"

namespace lift {

void fillControlScriptSample(const GuestMemory& mem, MarkerSampler& sampler, const Vfs& vfs,
                             ControlScriptSample* out) {
    if (out == nullptr) return;
    *out = ControlScriptSample{};
    out->header_len = kScriptHeaderLen;  // 编译期常量；照实回（⛔ 不是"从头里读出来的"）

    // ---- ① 身份：`Engine` 单例 → `cur` → `frame[cur].+0x3C` → 名字（名字只在身份变化时解析）----
    MarkerSample identity = sampler.sampleIdentity(mem);
    out->script_id = identity.id;
    out->cur = identity.cur;
    if (identity.valid) {
        // `sampleIdentity()` 只读 `cur` / `id`；名字解析是慢路径（CP932 + 两次 std::string），
        // 只在**暂停点**采一次 ⇒ 每次 sample 都补名字是安全的（这里不是 VM 循环头）。
        sampler.resolveName(mem, &identity);
        out->name = identity.name;
    }

    // ---- ② 位置：唯一公式；读不到 ⇒ 枚举 why（⛔ 不猜一个 pos）----
    const ScriptPositionSample pos = readScriptPositionSample(mem, identity);
    out->available = pos.valid;
    out->cur = pos.cur;
    if (pos.valid) {
        out->pos = pos.pos;
        out->pos_dwords =
            (pos.pos >= kScriptHeaderLen) ? (pos.pos - kScriptHeaderLen) / 4u : 0u;
    } else {
        out->why = pos.why;  // `readScriptPositionSample` 的静态字面量（既有 8 个之一）
    }

    // ---- ③ script_sha：宿主文件路线（解析不到 / 读失败 ⇒ 枚举理由）----
    if (!identity.valid || out->name.empty()) {
        out->sha_why = "no_identity";
        return;
    }
    std::string host_path;
    std::string resolve_err;
    if (!vfs.resolve(out->name, &host_path, &resolve_err, true, true)) {
        // ★ 本安装的 TITLE.BIN 在 `DATA*.ALF` 归档里（安装根没有裸件）⇒ 这里就是 `unresolved`。
        out->sha_why = "unresolved";
        return;
    }
    std::ifstream in(host_path, std::ios::binary);
    if (!in) {
        out->sha_why = "unreadable";
        return;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) {
        out->sha_why = "unreadable";
        return;
    }
    out->sha = toHex(sha256(bytes.data(), bytes.size()));
    out->sha_why = "none";
}

}  // namespace lift
