// emulator-lift · 指针施加核心 —— 口径见 `pointer_core.h` 的头注。
//
// ⛔ 本文件**不是**第二份输入状态：状态只有 `WindowManager` 一份。这里只有"到期了没有"的推进 +
//   计数 + 待抬起队列；施加输入的**每一处**都走 `runtime/input.h` 既有的 `input::*`（与 SDL /
//   旧脚本同一条路径）。
// ★ 文本来源：`input_plan.cpp` 的 397-499 / 567-602 / 621-658 段（C1a 的**行为不变的机械抽取**）。
//   唯一的改写 = 三个读数换成 `PointerPoint` 的值参数、钟换成 `pt.now_ms`（见头注的"唯一偏离"）。
#include "runtime/pointer_core.h"

#include <cstddef>
#include <cstdint>

#include "runtime/cursor_map.h"  // referenceToClientPixel（1280×720 参考系 → 客户区，唯一公式）
#include "runtime/host_window.h"  // WindowManager / HostWindow
#include "runtime/input.h"        // input::mouseMove / mouseButton / releaseProgram（唯一施加路径）
#include "runtime/ui_input.h"     // S9：注入指针的叠层路由（D6 三个入口里的 down / up）

namespace lift {

void PointerCore::armMoveAt(WindowManager& wm, int ref_x, int ref_y, const PointerPoint& pt) {
    // ★ 与 click 的**第一段**同一段代码（同一份坐标映射、同一份 `input::mouseMove`）。
    int client_x = 0;
    int client_y = 0;
    int target_w = 0;
    int target_h = 0;
    if (const HostWindow* w = wm.find(wm.primaryWindow())) {
        target_w = w->client_w;  // 0 / 没有窗口 ⇒ referenceToClientPixel 退回 1280×720
        target_h = w->client_h;
    }
    referenceToClientPixel(target_w, target_h, ref_x, ref_y, &client_x, &client_y);
    input::mouseMove(wm, client_x, client_y, InputSource::Script, kPointerMessageTimeMs);
    // ★ S6 / Q3：`mouse_down` 的轻闸门从**设备最后一次移动**起算（控制层的纯 move 同样算：
    //   闸门判的是"guest 有没有读过这个位置"，不是"哪条 step 挪的"）。
    last_move_cursor_done_ = pt.cursor_reads_done;
    last_move_ms_ = pt.now_ms;
    session_closed_ = false;  // 新的注入工作 ⇒ 所有权要重新握在脚本手里（见 closeSessionIfDone）
}

void PointerCore::armClickAt(WindowManager& wm, int ref_x, int ref_y, MouseButton button,
                             const PointerPoint& pt, std::uint32_t owner) {
    // ★ 这是**唯一**的 click 施加核心的**第一段**（配方 step 与控制层立即队列都到这里；
    //   判据 1 的"同一 apply 通道"）。★ 它**只**把光标放到目标上 —— 按下在 `pressDue`（下一段）。
    // ★ 坐标**算一次**：move / 之后的 down / up 都读同一对值
    //   （⛔ 两处各算一次 ⇒ move 到 A、消息 lParam 是 B 的静默漂移）。
    int client_x = 0;
    int client_y = 0;
    int target_w = 0;
    int target_h = 0;
    if (const HostWindow* w = wm.find(wm.primaryWindow())) {
        target_w = w->client_w;  // 0 / 没有窗口 ⇒ referenceToClientPixel 退回 1280×720
        target_h = w->client_h;
    }
    referenceToClientPixel(target_w, target_h, ref_x, ref_y, &client_x, &client_y);
    // ★ S9 / D6：注入的 **down** 与 OS 事件问**同一个** `UiClickRouter`（`WindowManager` 装的那一个；
    //   `nullptr` = 没有叠层 ⇒ 不问）。⚠ 这是**任何 `input::*` 之前**：吞掉 ⇒ 连 `mouseMove` 都不发
    //   （光标不动、guest 一条 WM_MOUSEMOVE 都收不到）。
    bool swallowed = false;
    if (UiInputRouter* router = wm.uiInputRouter()) {
        swallowed = router->routeInjectedPointer(UiPointerPhase::Down, button, client_x, client_y);
    }
    session_closed_ = false;  // 新的注入工作 ⇒ 所有权要重新握在脚本手里（见 closeSessionIfDone）
    // ★ S7 / Q8-A：**没有**"同按钮先抬起"的强制释放了。这个按钮还按着 ⇒ 调用方
    //   （`atPoint` / `drainImmediate`）**不消费**这条 click，等上一次的 up 落地（见两处调用的判据）。
    if (swallowed) {
        // ★ S9：吞掉 ⇒ **不动设备**（不 move、不按下）。但仍然压进 `armed_` —— 三段时序不变，
        //   `pressDue` 的闸门③ / `releaseDue` 的闸门①照旧决定 up 的时机（与不吞时同一个形状）。
        ++ui_consumed_;
    } else {
        armMoveAt(wm, ref_x, ref_y, pt);  // 同一份映射 + 同一份 `input::mouseMove`（⛔ 不复制）
    }
    ArmedPress a;
    a.button = button;
    a.x = client_x;
    a.y = client_y;
    a.arm_reads = pt.read_index;
    a.arm_cursor_done = pt.cursor_reads_done;  // 闸门 ③ 的起点（"已完成"的位置读点数）
    a.arm_ms = pt.now_ms;                      // 兜底 ③ 的起点
    a.line = pt.line;                          // trace 的 `plan:<文件>:<行>`（控制层 = 0）
    a.owner = owner;                           // ★ C1c：`releaseOwner` 的归属（配方 / 控制层 = 0）
    a.swallowed = swallowed;
    armed_.push_back(a);
}

void PointerCore::pressDue(WindowManager& wm, const PointerPoint& pt) {
    // ★ 阶段 ②：**唯一**的"按下"落地处。闸门 ③（`pointer_core.h` 的 `kPressFallbackMs` 那一段）：
    //   · 已**完成**过至少一次 CursorPos 读点（`pt.cursor_reads_done` 前进）⇒ 引擎的位置锁存
    //     `+0x74A4` 已经被刷新到 move 之后的位置；
    //   · 或者墙钟兜底到期（这个屏整屏不读位置）⇒ 仍然按下（⛔ 不让 click 永远挂着）。
    //   ★ "至少跨过一个读点"是**结构性**保证：`armClickAt` 只在本读点的后半段被调用
    //     （`drainImmediate` / flush 循环），而本函数在 `atPoint` 的**前半段** ⇒ 最早也是下一个读点。
    if (armed_.empty()) return;
    // ★ S7 / Q2：一次读点最多按**一条**（写预算）；已在 `pending_` 的按钮跳过（等它抬起，
    //   ⛔ 不重复按下 —— 那会让两条 `armed_` 共用同一次 0→1 转移）。
    if (writes_this_point_ != 0) return;
    const std::uint64_t now = pt.now_ms;
    for (std::size_t i = 0; i < armed_.size();) {
        const ArmedPress& a = armed_[i];
        const bool cursor_fresh = pt.cursor_reads_done > a.arm_cursor_done;
        const bool wallclock = now >= a.arm_ms && now - a.arm_ms >= kPressFallbackMs;
        if ((!cursor_fresh && !wallclock) || buttonPending(a.button)) {
            ++i;
            continue;
        }
        PendingRelease r;
        r.button = a.button;
        r.x = a.x;
        r.y = a.y;
        r.line = a.line;
        r.swallowed = a.swallowed;
        r.press_samples = wm.inputSampleCount();              // 读数（⛔ 不是闸门）
        r.press_key_reads = pt.key_level_reads;               // ★ 闸门 ① 的起点
        r.press_ms = now;                                     // ★ 兜底 ② 的起点
        r.press_frames = pt.frames;                           // ★ 闸门 ②' 的起点
        r.press_reads = pt.read_index;
        r.owner = a.owner;  // ★ C1c：owner 从"臂上"原样带到"已按下"（releaseOwner 两侧都要认）
        // ★ S9：down 已被叠层吞掉 ⇒ **不调 `input::mouseButton(down)`**（guest 一条消息都没有），
        //   但仍生成 `PendingRelease{swallowed=true}`：up 的时机走同一组闸门（三段时序不变）。
        if (!r.swallowed) {
            input::mouseButton(wm, r.x, r.y, r.button, /*down=*/true, InputSource::Script,
                              kPointerMessageTimeMs);
        }
        pending_.push_back(r);
        ++releases_pending_;
        armed_.erase(armed_.begin() + static_cast<std::ptrdiff_t>(i));
        writes_this_point_ = 1;  // 本读点的写动作已用完
        break;                   // 一次最多按一条
    }
}

void PointerCore::releaseDue(WindowManager& wm, const PointerPoint& pt) {
    // ★ S7 / Q2：一次读点最多抬**一条**（写预算）。⚠ `releaseAll` / `closeSessionIfDone` 的
    //   会话收口是**例外**（§3.8 #19）：它们一次可以投影多条 up。
    for (std::size_t i = 0; i < pending_.size();) {
        if (writes_this_point_ != 0) return;  // 本读点的写动作已用完 ⇒ 其余等下一个读点
        const PendingRelease& r = pending_[i];
        // ★ 闸门 ①（**电平腿**）：按下之后至少跨过 `kKeyLevelReadsBeforeRelease` 个 **KeyLevel**
        //   读点 —— 也就是 guest 的 `GetAsyncKeyState` **真的读到过**那个按下电平。
        //   ⛔ 不用 `inputSampleCount()`（它把 `GetCursorPos` 也算样本 ⇒ 位置读点能满足它、
        //   按键一次都没被读到 —— 2026-11-19 的失效根因）。
        const bool key_seen = pt.key_level_reads >=
                              r.press_key_reads + kKeyLevelReadsBeforeRelease;
        // ② 墙钟兜底：这个屏根本不读按键 ⇒ 按下之后 kReleaseFallbackMs 到期仍然抬起（⛔ 不永久按住）。
        const std::uint64_t now = pt.now_ms;
        const bool wallclock = now >= r.press_ms && now - r.press_ms >= kReleaseFallbackMs;
        // ②' ★ **回调边界**（只有供给帧计数的驱动才判；见 `kCallbackFramesBeforeRelease`）：
        //   按下之后必须**跨过下一个完整 guest 回调**。★ 它与 ①/② **并联**而不是二选一：
        //   2026-11-21 实测失败臂 `reason=keylevel`、press→release 只跨 6 个读点（含 2 个 KeyLevel 读点）
        //   —— 那 6 个读点全在**同一个回调**里 ⇒ 闸门 ① 满足了，回调体的状态机却一次也没看到按下电平。
        //   ⇒ ①/② 任一成立之后仍要过这一条；只有没供给帧线的驱动（插件 executor / 纯夹具）才跳过。
        const bool callback_ok = !pt.callback_boundary ||
                                 (pt.frames >= r.press_frames + kCallbackFramesBeforeRelease);
        if (!callback_ok || (!key_seen && !wallclock)) {
            ++i;
            continue;
        }
        // ★ trace（opt-in）：抬起走的是哪条腿 = "guest 到底有没有机会看到按下"的唯一读数
        //   （D 的判据面：`reason=wallclock` + `key_reads==press_key_reads` ⇒ 兜底先于任何一次电平读）。
        if (trace_ != nullptr) {
            trace_(trace_ctx_, r, key_seen, wallclock, wm.inputSampleCount(), pt.frames, pt.point);
        }
        emitRelease(wm, r);
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
        --releases_pending_;
        writes_this_point_ = 1;  // ★ S7：抬起也是写动作 ⇒ 本读点到此为止
    }
}

bool PointerCore::buttonPending(MouseButton b) const {
    // 一个按钮在 `pending_` 里 = 它**已按下、还没抬起** ⇒ 连点串行化（Q8-A）与 `mouse_down` 的判据。
    for (const PendingRelease& r : pending_) {
        if (r.button == b) return true;
    }
    return false;
}

void PointerCore::emitRelease(WindowManager& wm, const PendingRelease& r) {
    // ★ **Script up 用设备当前位置**（S4 follow-up）：`input::mouseButton(..., down=false, Script)`
    //   不先移动光标，消息坐标由 `input::mouseButton` 从设备**写后回读**。`r.x/r.y` 只是按下时
    //   存下的读数（测试 / trace 对账用），⛔ 不再是抬起消息的坐标来源。
    // ★ S9 / D6：注入的 **up** 也是"三个入口"之一 ⇒ 不管 down 吞没吞，都把它交给 router。
    //   · down 吞 ⇒ up **必须**吞（不看返回值）：`ClickRouter` 的 capture / pending click 靠这条收口，
    //     而 guest ⛔ 不许收到没有 down 的 up；
    //   · down 放 ⇒ up **必须**放（同样不看返回值）：overlay 中途出现 / 消失都不改变这次点击的归属
    //     （"down 放 ⇒ up 放"是 D6 的明文；对 `ClickRouter` 来说这次 up 也是 no-op —— `up_owed_` 是假）。
    UiInputRouter* router = wm.uiInputRouter();
    if (router != nullptr) router->routeInjectedPointer(UiPointerPhase::Up, r.button, r.x, r.y);
    if (r.swallowed) {
        ++released_;
        return;
    }
    input::mouseButton(wm, r.x, r.y, r.button, /*down=*/false, InputSource::Script, kPointerMessageTimeMs);
    ++released_;
}

std::size_t PointerCore::releaseAllPending(WindowManager& wm) {
    std::size_t n = 0;
    for (const PendingRelease& r : pending_) {
        emitRelease(wm, r);
        ++n;
    }
    pending_.clear();
    releases_pending_ = 0;
    // ★ 还没按下的（`armed_`）直接丢掉：它们**没有按下过** ⇒ 没有要抬的，也不需要伪造一次点击。
    armed_.clear();
    // ★ 收口（AOT 的 `aot_main` 跑完之后 / CTest 的收尾都走这里）：先 `emitRelease` 处理
    //   `pending_`（显式 click 的按下），再 `input::releaseProgram` 处理显式 `mouse_down` /
    //   `key_down` 留下的 Program 电平（§4.4 的调用点表）。
    //   与解释器臂 `InputScript::abandon()`（`releaseOwnedInputs` → `input::releaseProgram`）
    //   同一处、同一顺序 —— AOT 臂以前**没有**这个调用点（真鼠标在第一次注入之后永久失效）。
    input::releaseProgram(wm, kPointerMessageTimeMs);
    session_closed_ = true;
    return n;
}

std::size_t PointerCore::releaseOwner(WindowManager& wm, std::uint32_t owner) {
    // ★ C1c（施工图 §4.4）：owner 级的释放。语义与 `releaseAllPending` **不同**：
    //   · 只动该 owner 的条目（别的 owner 的按住照旧）⇒ 这是 C5 多 owner 的前提，
    //     但 C1 单 owner 下就已经要判（"脚本错误 / 卸载 ⇒ 只释放该 owner 的 hold"）。
    //   · `emitRelease` 不受 `writes_this_point_` 限制（与 `releaseAll` 同一条既有例外）：
    //     释放是**收口**，不是"本读点还能不能再写一条"。
    std::size_t n = 0;
    for (std::size_t i = 0; i < pending_.size();) {
        if (pending_[i].owner != owner) {
            ++i;
            continue;
        }
        const PendingRelease& r = pending_[i];
        // trace：owner 级释放**没有**闸门（两条腿都不成立，是"被强制抬起"）。
        // `point` 给 `Count`：调用点不在读点上下文里（`releaseOwner` 是 Run 的生命周期事件）。
        // `frames` 给 0：这里没有本读点的帧读数（不是闸门，只是 trace 读数）。
        if (trace_ != nullptr) {
            trace_(trace_ctx_, r, /*key_seen=*/false, /*wallclock=*/false, wm.inputSampleCount(),
                   /*frames=*/0, InputPoint::Count);
        }
        emitRelease(wm, r);
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
        ++n;
    }
    releases_pending_ = pending_.size();  // 与 `pending_` 同源（⛔ 不靠增减配平，避免漏减）
    // ★ 还没按下的（`armed_`）直接丢：它们**没有按下过** ⇒ 没有要抬的，也不需要伪造一次点击。
    for (std::size_t i = 0; i < armed_.size();) {
        if (armed_[i].owner != owner) {
            ++i;
            continue;
        }
        armed_.erase(armed_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return n;
}

void PointerCore::clearPointerState() {
    // `InputPlan::clear()` 里"指针那一半"的机械改写（成员搬到基类 ⇒ 复位也搬过来）。
    // ⚠ 与旧 `clear()` 的字段集合**逐个对齐**：`ui_consumed_` 旧 `clear()` 也不复位 ⇒ 这里不碰。
    pending_.clear();
    armed_.clear();
    fired_ = 0;
    released_ = 0;
    cursor_reads_done_ = 0;
    writes_this_point_ = 0;
    last_move_cursor_done_ = 0;
    last_move_ms_ = 0;
    releases_pending_ = 0;
    session_closed_ = true;
}

}  // namespace lift
