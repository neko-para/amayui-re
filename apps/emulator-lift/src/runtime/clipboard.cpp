// emulator-lift · 系统剪贴板：**唯一** include `<SDL3/SDL_clipboard.h>` 的 TU（见 `clipboard.h`）。
#include "runtime/clipboard.h"

#include <SDL3/SDL_clipboard.h>

namespace lift {

bool setSystemClipboard(std::string_view text, std::string* err) {
    // SDL 要 NUL 结尾 ⇒ 必须有一次拷贝（报告文本 ≤ 1.5 KB，代价可忽略）。
    const std::string owned(text);
    if (SDL_SetClipboardText(owned.c_str())) return true;
    if (err != nullptr) *err = std::string("SDL_SetClipboardText: ") + SDL_GetError();
    return false;
}

}  // namespace lift
