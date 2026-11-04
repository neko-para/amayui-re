#include "runtime/present.h"

namespace lift {

bool HeadlessPresenter::present(const SurfaceArena& arena, const GuestSurface& surface,
                                const std::string& dump_path, std::string* err) {
    last_hash_ = arena.hash(surface);
    ++frame_count_;
    last_dump_.clear();
    if (!dump_path.empty()) {
        if (!arena.dumpBmp(surface, dump_path, err)) return false;
        last_dump_ = dump_path;
    }
    return true;
}

}  // namespace lift
