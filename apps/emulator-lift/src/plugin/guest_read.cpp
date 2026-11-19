// emulator-lift · `GuestEngineRead` 的实现（口径见头注）
#include "plugin/guest_read.h"

#include <cstdint>

#include "runtime/script_globals.h"

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

}  // namespace lift::plugin
