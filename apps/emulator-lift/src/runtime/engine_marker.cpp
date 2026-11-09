#include "runtime/engine_marker.h"

#include <cstdio>
#include <cstdint>
#include <string>

#include "runtime/cp932.h"
#include "runtime/memory.h"

namespace lift {
namespace {

// 有界读：越界 / 未提交 ⇒ false（⛔ 绝不落到 GuestMemory 裸读 —— safe 模式缺省关，
// 漏一个 committed 就是宿主 SIGSEGV；纯档夹具没有 fault hook 可兜）。
bool readU32(const GuestMemory& mem, std::uint64_t ea, std::uint32_t* out) {
    if (ea > 0xFFFFFFFFull - 3ull) return false;
    const std::uint32_t a = static_cast<std::uint32_t>(ea);
    if (!mem.committed(a, 4u)) return false;
    *out = mem.rd32(a);
    return true;
}

struct NameResult {
    std::string name;
    bool degraded = false;
    const char* why = nullptr;
};

NameResult failName(const char* why) {
    NameResult r;
    r.degraded = true;
    r.why = why;
    return r;
}

// id → 名字：严格照 `sub_454FA0` 的三条臂（§3.3/§3.4/§3.7），但**每一跳先判可读**。
// ★ 与引擎的**有意差异**（父级裁决 2 + 取证 §3.7）：
//   · `id < 0`：引擎高字节非 0 ⇒ 走区号臂，`sar 24` 得 −1 ⇒ 读卷表**之前** 4 字节（表外读）。
//     宿主不复刻未定义行为，直接降级成 `id:0x…`；
//   · `+0x414`/`+0x11C` 为 0：引擎不判空（会返回 `80*id` 这种小整数当指针），宿主直接降级；
//   · `+0x40C` 构造期**没清零**（唯一写者是 `0x454D9B`）⇒ 读到 0 / 超上界都按"未装载"降级。
NameResult resolveIdName(const GuestMemory& mem, std::uint32_t db_ea, std::uint32_t raw_id) {
    if (static_cast<std::int32_t>(raw_id) < 0) {
        return failName("id<0（引擎走区号臂会读表外；宿主直接降级）");
    }
    const std::uint32_t volume = (raw_id >> 24) & 0xFFu;
    std::uint64_t entry = 0;
    if (volume != 0) {
        // 区号臂：卷表 `[index+0x3028 + 4*volume]` → 子索引对象。
        std::uint32_t sub = 0;
        if (!readU32(mem, static_cast<std::uint64_t>(db_ea) + kFileVolumeTableOffset + 4ull * volume, &sub) ||
            sub == 0) {
            return failName("卷表项不可读或为 0");
        }
        std::uint32_t count = 0;
        if (!readU32(mem, static_cast<std::uint64_t>(sub) + kFileSubIndexCountOffset, &count) || count == 0 ||
            count > kMaxFileEntries) {
            return failName("子索引条目数不可读 / 为 0 / 越界（卷未装载）");
        }
        const std::uint32_t idx = raw_id & 0xFFFFFFu;
        if (idx >= count) return failName("id 的低 24 位越界（>= 子索引条目数）");
        std::uint32_t name_base = 0;
        if (!readU32(mem, static_cast<std::uint64_t>(sub) + kFileSubIndexNameBaseOffset, &name_base) ||
            name_base == 0) {
            return failName("子索引名字数组基址为 0");
        }
        entry = static_cast<std::uint64_t>(name_base) + static_cast<std::uint64_t>(kFileNameStride) * idx;
    } else {
        // 根臂（区号 = 0）：`id < count`、名字 = `[+0x414] + 80*id`。
        std::uint32_t count = 0;
        if (!readU32(mem, static_cast<std::uint64_t>(db_ea) + kFileRootCountOffset, &count) || count == 0 ||
            count > kMaxFileEntries) {
            return failName("根条目数不可读 / 为 0 / 越界（TOC 未装载）");
        }
        if (raw_id >= count) return failName("id 越界（>= 根条目数）");
        std::uint32_t name_base = 0;
        if (!readU32(mem, static_cast<std::uint64_t>(db_ea) + kFileRootNameBaseOffset, &name_base) ||
            name_base == 0) {
            return failName("根名字数组基址为 0");
        }
        entry = static_cast<std::uint64_t>(name_base) + static_cast<std::uint64_t>(kFileNameStride) * raw_id;
    }
    // ★ 64 位算术：`80*id` 在 id=0xFFFFFF 时已经 0x4FFFFFF0，32 位会回绕到低地址（静默读别的页）。
    if (entry + kFileNameMaxBytes > 0x100000000ull) return failName("名字条目跨出 4 GiB 地址空间");
    std::string bytes;
    bytes.reserve(kFileNameMaxBytes);
    for (std::uint32_t i = 0; i < kFileNameMaxBytes; ++i) {
        const std::uint32_t a = static_cast<std::uint32_t>(entry + i);
        if (!mem.committed(a, 1u)) return failName("名字条目未提交");
        const std::uint8_t b = mem.rd8(a);
        if (b == 0) break;  // NUL 或满 80 字节截断：两种都接受（条目是否保证 NUL 未取证）
        bytes.push_back(static_cast<char>(b));
    }
    if (bytes.empty()) return failName("名字为空（条目未初始化？）");
    std::size_t bad = 0;
    const std::u16string wide = cp932ToWide(bytes, &bad);
    if (bad != 0) return failName("名字不是合法 CP932");
    std::size_t bad_utf8 = 0;
    NameResult out;
    out.name = utf16ToUtf8(wide, &bad_utf8, U'?');
    if (bad_utf8 != 0 || out.name.empty()) return failName("名字无法转成 UTF-8 / 为空");
    return out;
}

}  // namespace

MarkerSample MarkerSampler::sampleIdentity(const GuestMemory& mem) {
    MarkerSample s;
    ++attempts_;
    // ① 单例发布了吗（`0x4BAA3A` 之前 / 退出后是 0）——boot 期正常，不告警。
    std::uint32_t base = 0;
    if (!readU32(mem, kEngineSingletonGlobal, &base) || base == 0) {
        ++unpublished_;
        s.why = "engine-unpublished";
        return s;
    }
    ++published_;
    // ② cur（`0x5D880`；上界 39 由引擎自己维持，见 0x41C6C1 / 0x41C85A）。
    std::uint32_t cur = 0;
    if (!readU32(mem, static_cast<std::uint64_t>(base) + kEngineCurOffset, &cur)) {
        ++invalid_;
        s.why = "cur-uncommitted";
        if (warnOnce(WarnCur)) {
            std::fprintf(stderr, "[marker] ⚠ Engine=0x%08X 的 cur 槽未提交 ⇒ 本次不采\n", base);
        }
        return s;
    }
    if (cur >= kFrameCount) {
        ++invalid_;
        s.why = "cur-out-of-range";
        if (warnOnce(WarnCur)) {
            std::fprintf(stderr, "[marker] ⚠ cur=%u 越界（预期 0..%u）⇒ 本次不采；采样点/基址可能已漂\n", cur,
                         kFrameCount - 1u);
        }
        return s;
    }
    // ③ 记录：`base+0x5D894+0x78*cur`；`+0x00` = 代码区基址，非 0 且指向已提交页才算"这格装好了"
    //   （§4.C 的形状不变量：空槽 ⇒ 无当前帧）。
    const std::uint64_t rec =
        static_cast<std::uint64_t>(base) + kEngineFrameBase + static_cast<std::uint64_t>(kFrameStride) * cur;
    std::uint32_t code_base = 0;
    if (!readU32(mem, rec, &code_base) || code_base == 0 || !mem.committed(code_base, 1u)) {
        ++invalid_;
        s.why = "frame-record-unavailable";
        if (warnOnce(WarnRecord)) {
            std::fprintf(stderr,
                         "[marker] ⚠ Engine=0x%08X cur=%u 的记录不可用（+0x00=0x%08X 为 0 / 未提交）⇒ 本次不采；"
                         "帧还没装好或基址已漂\n",
                         base, cur, code_base);
        }
        return s;
    }
    // ④ 身份格（`+0x3C`；`0x40EDEB` 是写者，`0x3` 的第 4 实参 = 统一文件 id）。
    std::uint32_t raw_id = 0;
    if (!readU32(mem, rec + kFrameIdentityOffset, &raw_id)) {
        ++invalid_;
        s.why = "identity-uncommitted";
        if (warnOnce(WarnRecord)) {
            std::fprintf(stderr, "[marker] ⚠ Engine=0x%08X cur=%u 的身份格（记录+0x3C）未提交 ⇒ 本次不采\n", base, cur);
        }
        return s;
    }
    s.valid = true;
    s.cur = cur;
    s.id = raw_id;
    if (raw_id == 0 && cur != 0 && warnOnce(WarnZeroId)) {
        // id=0 是根脚本的**合法** id；但非根帧读到 0 说明身份还没写（帧装载器才有写者 0x40EDEB）。
        std::fprintf(stderr,
                     "[marker] ⚠ cur=%u 的身份 id=0（根脚本 id 合法）⇒ 按 TOC 第 0 条解析；若这不是根帧，"
                     "帧记录可能还没写身份\n",
                     cur);
    }
    return s;
}

std::uint64_t MarkerSampler::nameTableSignature(const GuestMemory& mem, std::uint32_t raw_id) const {
    std::uint32_t base = 0;
    if (!readU32(mem, kEngineSingletonGlobal, &base) || base == 0) return 0;
    const std::uint64_t db_ea = static_cast<std::uint64_t>(base) + kFileIndexOffset;
    if (db_ea > 0xFFFFFFFFull) return 0;
    std::uint32_t count = 0;
    std::uint32_t name_base = 0;
    const bool root_ok = readU32(mem, db_ea + kFileRootCountOffset, &count) &&
                         readU32(mem, db_ea + kFileRootNameBaseOffset, &name_base);
    // 0 也参与指纹：`0` = 未装载，与"已装载但某字段恰好 0"区分开（后者在解析里也会降级）。
    std::uint64_t sig = root_ok ? ((static_cast<std::uint64_t>(count) << 32) | name_base) : 0x9E3779B97F4A7C15ull;
    if (static_cast<std::int32_t>(raw_id) >= 0) {
        const std::uint32_t volume = (raw_id >> 24) & 0xFFu;
        if (volume != 0) {
            std::uint32_t sub = 0;
            if (!readU32(mem, db_ea + kFileVolumeTableOffset + 4ull * volume, &sub) || sub == 0) {
                return sig ^ 0x5555555555555555ull;
            }
            std::uint32_t sub_count = 0;
            std::uint32_t sub_name_base = 0;
            if (!readU32(mem, static_cast<std::uint64_t>(sub) + kFileSubIndexCountOffset, &sub_count) ||
                !readU32(mem, static_cast<std::uint64_t>(sub) + kFileSubIndexNameBaseOffset, &sub_name_base)) {
                return sig ^ sub;
            }
            sig ^= (static_cast<std::uint64_t>(sub_count) << 32) | sub_name_base;
            sig ^= sub;
        }
    }
    return sig;
}

void MarkerSampler::resolveName(const GuestMemory& mem, MarkerSample* s) {
    if (s == nullptr || !s->valid) return;
    s->name_table_sig = nameTableSignature(mem, s->id);
    std::uint32_t base = 0;
    NameResult nr;
    if (!readU32(mem, kEngineSingletonGlobal, &base) || base == 0) {
        nr = failName("Engine 单例在两次读之间变成 0");
    } else {
        const std::uint64_t db_ea = static_cast<std::uint64_t>(base) + kFileIndexOffset;
        nr = db_ea > 0xFFFFFFFFull ? failName("FileDB 偏移跨出 4 GiB")
                                   : resolveIdName(mem, static_cast<std::uint32_t>(db_ea), s->id);
    }
    if (nr.degraded) {
        s->name_degraded = true;
        s->why = nr.why;
        char idbuf[32];
        std::snprintf(idbuf, sizeof(idbuf), "id:0x%08X", s->id);
        s->name = idbuf;
        if (warnOnce(WarnNameTable)) {
            std::fprintf(stderr,
                         "[marker] ⚠ 名字表不可读（%s）⇒ 降级按 id 触发：id:0x%08X；脚本写名字时仍可用 "
                         "`id:0x%08X` 形比对（⛔ 不静默失明）\n",
                         nr.why != nullptr ? nr.why : "?", s->id, s->id);
        }
    } else {
        s->name = nr.name;
    }
}

MarkerSample MarkerSampler::sample(const GuestMemory& mem) {
    MarkerSample s = sampleIdentity(mem);
    resolveName(mem, &s);
    return s;
}

void MarkerSampler::resetCounters() {
    attempts_ = 0;
    unpublished_ = 0;
    published_ = 0;
    invalid_ = 0;
}

}  // namespace lift
