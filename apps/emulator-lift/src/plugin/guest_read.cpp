// emulator-lift · `GuestEngineRead` 的实现（口径见头注）
#include "plugin/guest_read.h"

#include <cstdint>

#include "runtime/memory.h"       // `Tick.local` 的按需读（committed / rd32）
#include "runtime/script_globals.h"
#include "runtime/value_codec.h"  // 语义值 = DEC(raw, key)

namespace lift::plugin {

bool GuestEngineRead::globalGet(GlobalKind kind, std::uint32_t id, GlobalValue* out, GlobalError* err) {
    // ★ 唯一的读实现：`readGlobal`（`runtime/script_globals.cpp`）。
    //   ⛔ 不在这里补第二套"engine 未发布 / 池越界"的判据 —— 那是同一个结论的第二个出口。
    if (out == nullptr) {
        if (err != nullptr) *err = GlobalError::TypeMismatch;
        return false;
    }
    return lift::readGlobal(mem_, id, kind, out, err);
}

bool GuestEngineRead::readLocal(const InstrLocalView& v, InstrLocalKind kind, std::uint32_t index,
                                std::uint32_t* out) const {
    if (out == nullptr) return false;
    const auto read4 = [this](std::uint32_t ea, std::uint32_t* o) {
        if (!mem_.committed(ea, 4u)) return false;
        *o = mem_.rd32(ea);
        return true;
    };
    switch (kind) {
        case InstrLocalKind::Int: {
            if (index >= v.int_count) return false;
            std::uint32_t raw = 0;
            // ★ 没有 ENC key ⇒ 不能声称读到的 raw 是语义值（⛔ 不拿 raw 冒充）。
            if (!read4(v.int_base + 4u * index, &raw) || !v.has_key) return false;
            *out = decodeInt(raw, v.codec_key);
            return true;
        }
        case InstrLocalKind::Ptr: {
            if (index >= v.ptr_count) return false;
            return read4(v.ptr_base + 4u * index, out);  // ★ 指针值 = 槽里原样（⛔ 不解码）
        }
        case InstrLocalKind::PtrDword: {
            if (index >= v.ptr_count) return false;
            std::uint32_t p = 0;
            if (!read4(v.ptr_base + 4u * index, &p) || p == 0) return false;
            std::uint32_t raw = 0;
            if (!read4(p, &raw) || !v.has_key) return false;
            *out = decodeInt(raw, v.codec_key);
            return true;
        }
    }
    return false;
}

}  // namespace lift::plugin
