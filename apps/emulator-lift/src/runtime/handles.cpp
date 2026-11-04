// emulator-lift · guest 句柄表实现
// ★ 值 0x100 起、4 字节步进 ⇒ trace 可复现；⛔ 不用指针值当句柄（那会随 ASLR 变）。
#include "runtime/handles.h"

#include <cstdio>
#include <cstdlib>

namespace lift {

const char* handleKindName(HandleKind kind) {
    switch (kind) {
        case HandleKind::None: return "none";
        case HandleKind::Event: return "event";
        case HandleKind::Mutex: return "mutex";
        case HandleKind::Semaphore: return "semaphore";
        case HandleKind::CriticalSection: return "critsec";
        case HandleKind::File: return "file";
        case HandleKind::Find: return "find";
        case HandleKind::Thread: return "thread";
        case HandleKind::Timer: return "timer";
        case HandleKind::Module: return "module";
        case HandleKind::Other: return "other";
        case HandleKind::GdiDc: return "gdi-dc";
        case HandleKind::GdiFont: return "gdi-font";
        case HandleKind::GdiBitmap: return "gdi-bitmap";
        case HandleKind::GdiFontMem: return "gdi-fontmem";
        case HandleKind::GdiRegion: return "gdi-region";
    }
    return "?";
}

std::uint32_t HandleTable::add(HandleKind kind, const char* name, void* payload, std::uint32_t aux) {
    Slot s;
    s.value = kFirst + kStep * static_cast<std::uint32_t>(entries_.size());
    s.entry.kind = kind;
    s.entry.name = name ? name : "";
    s.entry.payload = payload;
    s.entry.aux = aux;
    s.entry.used = true;
    entries_.push_back(std::move(s));
    return entries_.back().value;
}

HandleEntry* HandleTable::get(std::uint32_t h) {
    // 句柄密集且数量小 ⇒ 线性查找足够；按值找而不是按下标算（关闭后不复用 ⇒ 不会歧义）
    for (Slot& s : entries_) {
        if (s.value == h && s.entry.used) return &s.entry;
    }
    return nullptr;
}

const HandleEntry* HandleTable::get(std::uint32_t h) const {
    for (const Slot& s : entries_) {
        if (s.value == h && s.entry.used) return &s.entry;
    }
    return nullptr;
}

bool releasePayload(HandleTable& table, std::uint32_t h) {
    HandleEntry* entry = table.get(h);
    if (!entry) return false;
    if (!entry->payload) return true;
    // ★ 判据是"还有没有**别的活句柄**指着同一个 payload"（不是引用计数：两个句柄各自不知道对方）。
    bool shared = false;
    const std::uint32_t end =
        HandleTable::kFirst + HandleTable::kStep * static_cast<std::uint32_t>(table.size());
    for (std::uint32_t other = HandleTable::kFirst; other < end; other += HandleTable::kStep) {
        if (other == h) continue;
        if (const HandleEntry* o = table.get(other)) {
            if (o->payload == entry->payload) {
                shared = true;
                break;
            }
        }
    }
    if (!shared && entry->destroy) entry->destroy(entry->payload);
    entry->payload = nullptr;
    entry->destroy = nullptr;
    return true;
}

std::uint32_t HandleTable::findNamed(HandleKind kind, const std::string& name) const {
    if (name.empty()) return 0;
    for (const Slot& s : entries_) {
        if (!s.entry.used || s.entry.kind != kind) continue;
        if (s.entry.name == name) return s.value;
    }
    return 0;
}

bool HandleTable::close(std::uint32_t h) {
    for (Slot& s : entries_) {
        if (s.value == h && s.entry.used) {
            s.entry.used = false;
            return true;
        }
    }
    return false;
}

void HandleTable::invalidHandle(std::uint32_t h, const char* api) const {
    std::fprintf(stderr, "\n=== invalid guest handle ===\n  api    : %s\n  handle : 0x%08X\n", api ? api : "?",
                 h);
    std::fprintf(stderr, "  已分配的句柄：");
    int n = 0;
    for (const Slot& s : entries_) {
        if (!s.entry.used) continue;
        std::fprintf(stderr, " 0x%08X(%s%s%s)", s.value, handleKindName(s.entry.kind),
                     s.entry.name.empty() ? "" : ":", s.entry.name.c_str());
        if (++n >= 16) {
            std::fprintf(stderr, " …");
            break;
        }
    }
    if (n == 0) std::fprintf(stderr, "(空表)");
    std::fprintf(stderr, "\n");
    std::fflush(nullptr);
    std::abort();  // ⛔ 句柄校验失败 = 响亮失败（S8 变异清单的一条）
}

}  // namespace lift
