// emulator-lift · `GuestEditSink` 的实现 —— 口径见 `edit_sink.h` 的头注。
//
// ★ 一行施加：`writeGlobal`（`runtime/script_globals.h`）是 script global 写的**唯一**实现
//   （ENC 编码 / 池基址 / 计数校验 / 错误词表全在那边）。⛔ 本文件不许自己算地址、⛔ 不许缓存
//   池基址（`0x6d` 会整体对调）、⛔ 不写后校验、⛔ 不挑时机。
//
// ★ 这条 `writeGlobal` 调用**就是**"Edit 立即"的判据面：`tools/mutate-check.mjs` 的
//   `edit_sink-delay-write` 把它改成"写进队列（延迟）"⇒ `trigger-runner/global-read-modify-write-click`
//   的"同拍读回"当场红。⇒ 本行的**位置**是那个变异的 `from` 锚点（改这里要同时改清单）。
#include "runtime/edit_sink.h"

#include <cstdint>

#include "runtime/memory.h"          // GuestMemory
#include "runtime/script_globals.h"  // writeGlobal / GlobalError / globalErrorName

namespace lift::plugin {

Receipt GuestEditSink::globalSet(GlobalKind kind, std::uint32_t id, const GlobalValue& value) {
    GlobalError err = GlobalError::None;
    const bool ok = writeGlobal(mem_, id, kind, value, &err);

    Receipt receipt;
    receipt.accepted = ok;
    // ★ 成功 = nullptr；失败 = **既有词表**里的名字（⛔ 不新造 `"encode_failed"` 的第二份拼写）。
    receipt.code = ok ? nullptr : globalErrorName(err);
    // `seq/queued/id` 保持 0：Edit 不进队列（见 `edit_sink.h` 头注）。
    return receipt;
}

}  // namespace lift::plugin
