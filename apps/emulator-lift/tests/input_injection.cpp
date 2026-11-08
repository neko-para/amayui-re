// emulator-lift · 注入通道守卫：`--input-script` 不依赖 OS 条件（pure）
//
// 判据（红了意味着什么）：
//   ① 一份指令式时间线在**完全 headless**（不 `SDL_Init(VIDEO)`、没有窗口、没有焦点、没有任何人
//      挪过真实光标）的进程里跑完后，guest 通过它**自己的读点**看到注入：
//        · `GetAsyncKeyState(VK_LBUTTON / VK_RETURN)` 返回 0x8000；
//        · `GetCursorPos` 返回**注入的客户区坐标 + 窗口原点**，再经 `ScreenToClient` 回到注入坐标
//          （⇒ 复用的是同一套 client↔guest 映射，注入侧没有第二套）；
//        · `PeekMessageA` 取到 WM_LBUTTONDOWN / WM_KEYDOWN / WM_KEYUP / WM_LBUTTONUP，且
//          wParam / lParam 与注入一致；`DispatchMessageA` 真的把这些消息送进 guest 的 WndProc。
//   ② 帧触发真的按帧：`at frame 1` 在**没有 Present 过**时不触发；Present 一帧后触发。
//   ③ `click` 的按下与抬起**跨帧**：guest 必须采到"按下"与"抬起后"两种电平（同帧 down+up 会漏）。
//   ④ 优先级规则是显式的：脚本拥有某键时，OS 的抬起 / **焦点丢失清键**都不生效（注入不因失焦而丢）；
//      反过来，脚本接管光标时**真人松开按钮必须被接受**（否则 VK 电平卡在按下）。
//   ⑤ 脚本以"仍按着"的状态收口（动作跑完 / `quit`）⇒ 必须**补发** WM_KEYUP / WM_LBUTTONUP 并清电平。
//   ⑥ 同一帧里"上一 click 的抬起 + 本帧显式 mouse_down" 读作 down（延迟抬起先处理）。
//   ⑦ 解析失败必须响亮（动作前没有时间、mouse_down 没有坐标、未知按钮 / 指令 / VK 0、空脚本）。
//
// ★ 这个进程**从不调用 SDL 初始化**（测试里显式断言 `SDL_WasInit(VIDEO)==0`）：它证明的是
//   "注入只走 `runtime/input.h` + WindowManager 那一份状态"。
//   若把 `GetAsyncKeyState` 改回读 OS（或让注入改走 SDL 事件泵），这里会当场红。
#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "runtime/host_window.h"
#include "runtime/input.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kCode = 0x00401000u;  // guest 主循环体
constexpr std::uint32_t kProc = 0x00401800u;  // guest WndProc
constexpr std::uint32_t kConsumer = 0x00403000u;  // 伪造的"每 50 ms 采样一次"的 guest 输入消费者
// ★ 不采样的 guest 放**另一个**地址：`placeCode` 不失效块缓存（同一 EA 重放会跑旧块），
//   两个夹具共用地址会让"不采样"的用例偷偷跑成"采样"的（实测过）。
constexpr std::uint32_t kSpinner = 0x00404000u;
constexpr std::uint32_t kMsg = 0x00402000u;   // MSG（28 字节）
constexpr std::uint32_t kPoint = 0x00402100u; // POINT（8 字节）
constexpr std::uint32_t kRes = 0x00402200u;   // 观测槽
constexpr std::uint32_t kStubBase = 0x7FF80000u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;
// ★ 自动 hold 的守卫夹具（`click-spans-sample` / `click-bounded-without-sampling`）：
//   guest 时钟是虚拟时钟（每条指令 100 ns）⇒ `kSampleBurn` 次 `dec/jnz`（= 2×kSampleBurn 条指令）
//   ≈ 500k 条 = **50 ms guest 时间** = 实测房间的采样周期（`GetCursorPos` 880 次 / 43.96 s）。
constexpr std::uint32_t kSampleBurn = 250000u;
constexpr std::uint32_t kDownSeenPolls = kRes + 0u;  // guest 侧：看到"鼠标按下"的采样次数
constexpr std::uint32_t kPollCount = kRes + 4u;      // guest 侧：采样次数
constexpr std::uint32_t kKeyDownSeenPolls = kRes + 8u;  // guest 侧：看到 VK_RETURN 按下的采样次数

// 观测槽（guest 写的）
constexpr std::uint32_t kLastX = kRes + 0u;
constexpr std::uint32_t kLastY = kRes + 4u;
constexpr std::uint32_t kDownSeen = kRes + 8u;
constexpr std::uint32_t kKeySeen = kRes + 12u;
constexpr std::uint32_t kLbDownMsg = kRes + 16u;
constexpr std::uint32_t kLbUpMsg = kRes + 20u;
constexpr std::uint32_t kKeyDownMsg = kRes + 24u;
constexpr std::uint32_t kReleaseAfterDown = kRes + 28u;
constexpr std::uint32_t kRounds = kRes + 32u;
constexpr std::uint32_t kClientX = kRes + 36u;
constexpr std::uint32_t kClientY = kRes + 40u;
constexpr std::uint32_t kMsgWparam = kRes + 44u;
constexpr std::uint32_t kMsgLparam = kRes + 48u;
constexpr std::uint32_t kKeyWparam = kRes + 52u;
constexpr std::uint32_t kKeyUpMsg = kRes + 56u;
// WndProc 写的
constexpr std::uint32_t kDispatchId = kRes + 60u;
constexpr std::uint32_t kDispatchWparam = kRes + 64u;
constexpr std::uint32_t kDispatchLbDown = kRes + 68u;
constexpr std::uint32_t kDispatchLbWparam = kRes + 72u;
constexpr std::uint32_t kLbUpWparam = kRes + 76u;
constexpr std::uint32_t kLbUpLparam = kRes + 80u;
constexpr std::uint32_t kKeyUpWparam = kRes + 84u;
constexpr std::uint32_t kDispatchKeyDown = kRes + 88u;
constexpr std::uint32_t kDispatchKeyWparam = kRes + 92u;
constexpr std::uint32_t kResBytes = 96u;

// 窗口原点：非 0，才能证明 "client → screen" 的换算被复用（注入坐标是客户端坐标）。
constexpr int kWinX = 7;
constexpr int kWinY = 11;
constexpr int kClickX = 812;
constexpr int kClickY = 622;

// ---- 极简 x86 汇编器（只覆盖本夹具用到的编码）----
struct Asm {
    explicit Asm(std::uint32_t base) : base_(base) {}
    std::vector<std::uint8_t> b;
    std::vector<std::pair<std::uint32_t, int>> fixups;  // (rel32 字段偏移, label)
    int next_label = 0;

    std::uint32_t ea() const { return base_ + static_cast<std::uint32_t>(b.size()); }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void pushImm(std::uint32_t v) {
        u8(0x68);
        u32(v);
    }
    void callRel(std::uint32_t target) {
        const std::uint32_t at = ea();
        u8(0xE8);
        u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(target - (at + 5))));
    }
    void movEaxImm(std::uint32_t v) {
        u8(0xB8);
        u32(v);
    }
    void movEcxImm(std::uint32_t v) {
        u8(0xB9);
        u32(v);
    }
    void decEcx() { u8(0x49); }
    void jmp32(int l) {
        u8(0xE9);
        fixups.emplace_back(static_cast<std::uint32_t>(b.size()), l);
        u32(0);
    }
    void movEaxMem(std::uint32_t addr) {
        u8(0xA1);
        u32(addr);
    }
    void movMemEax(std::uint32_t addr) {
        u8(0xA3);
        u32(addr);
    }
    void movEcxMem(std::uint32_t addr) {
        u8(0x8B);
        u8(0x0D);
        u32(addr);
    }
    void movMemEcx(std::uint32_t addr) {
        u8(0x89);
        u8(0x0D);
        u32(addr);
    }
    void movMemImm(std::uint32_t addr, std::uint32_t v) {
        u8(0xC7);
        u8(0x05);
        u32(addr);
        u32(v);
    }
    void testEaxImm(std::uint32_t v) {
        u8(0xA9);
        u32(v);
    }
    void cmpEaxImm(std::uint32_t v) {
        u8(0x3D);
        u32(v);
    }
    void addEaxImm(std::uint32_t v) {
        u8(0x05);
        u32(v);
    }
    int newLabel() { return next_label++; }
    void label(int l) { labels_[l] = static_cast<std::uint32_t>(b.size()); }
    void jz(int l) { jcc(0x84, l); }
    void jnz(int l) { jcc(0x85, l); }
    void jcc(std::uint8_t op2, int l) {
        u8(0x0F);
        u8(op2);
        fixups.emplace_back(static_cast<std::uint32_t>(b.size()), l);
        u32(0);
    }
    void ret() { u8(0xC3); }
    void retStdcall(std::uint16_t bytes) {
        u8(0xC2);
        u8(static_cast<std::uint8_t>(bytes & 0xFF));
        u8(static_cast<std::uint8_t>((bytes >> 8) & 0xFF));
    }
    bool patch(std::string* err) {
        for (const auto& f : fixups) {
            const auto it = labels_.find(f.second);
            if (it == labels_.end()) {
                *err = "未定义的标号";
                return false;
            }
            const std::uint32_t target = base_ + it->second;
            const std::int32_t rel = static_cast<std::int32_t>(target - (base_ + f.first + 4));
            for (int i = 0; i < 4; ++i) {
                b[f.first + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((rel >> (8 * i)) & 0xFF);
            }
        }
        return true;
    }

private:
    std::uint32_t base_;
    std::map<int, std::uint32_t> labels_;
};

std::uint32_t stubOf(lift::Machine& m, const char* name) {
    const lift::ShimDef* d = m.shims().find("user32", name);
    if (!d || !d->stub_ea) {
        std::fprintf(stderr, "stub missing: %s\n", name);
        return 0;
    }
    return d->stub_ea;
}

// 一轮 guest 体：读光标（含 ScreenToClient 回程）/ 读两个键电平 / 收一条消息并 Dispatch 到 WndProc /
// 记"按下之后又看到抬起"。
bool buildGuestBody(std::vector<std::uint8_t>* out, std::uint32_t get_cursor, std::uint32_t screen_to_client,
                    std::uint32_t get_async, std::uint32_t peek, std::uint32_t dispatch, std::uint32_t hwnd,
                    std::string* err) {
    Asm a(kCode);
    // GetCursorPos(&pt)
    a.pushImm(kPoint);
    a.callRel(get_cursor);
    a.movEaxMem(kPoint);
    a.movMemEax(kLastX);
    a.movEaxMem(kPoint + 4u);
    a.movMemEax(kLastY);
    // ScreenToClient(hwnd, &pt) ⇒ 回到客户区坐标
    a.pushImm(kPoint);
    a.pushImm(hwnd);
    a.callRel(screen_to_client);
    a.movEaxMem(kPoint);
    a.movMemEax(kClientX);
    a.movEaxMem(kPoint + 4u);
    a.movMemEax(kClientY);

    // GetAsyncKeyState(VK_LBUTTON)
    a.pushImm(0x01u);
    a.callRel(get_async);
    a.testEaxImm(0x8000u);
    const int no_down = a.newLabel();
    a.jz(no_down);
    a.movMemImm(kDownSeen, 1u);
    a.label(no_down);

    // GetAsyncKeyState(VK_RETURN)
    a.pushImm(0x0Du);
    a.callRel(get_async);
    a.testEaxImm(0x8000u);
    const int no_key = a.newLabel();
    a.jz(no_key);
    a.movMemImm(kKeySeen, 1u);
    a.label(no_key);

    // PeekMessageA(&msg, 0, 0, 0, PM_REMOVE=1)
    a.pushImm(1u);
    a.pushImm(0u);
    a.pushImm(0u);
    a.pushImm(0u);
    a.pushImm(kMsg);
    a.callRel(peek);
    a.testEaxImm(0xFFFFFFFFu);
    const int no_msg = a.newLabel();
    a.jz(no_msg);
    a.movEaxMem(kMsg + 4u);  // MSG.message
    const int not_lbdown = a.newLabel();
    a.cmpEaxImm(0x0201u);  // WM_LBUTTONDOWN
    a.jnz(not_lbdown);
    a.movMemImm(kLbDownMsg, 1u);
    a.movEaxMem(kMsg + 8u);  // MSG.wParam
    a.movMemEax(kMsgWparam);
    a.movEaxMem(kMsg + 12u);  // MSG.lParam
    a.movMemEax(kMsgLparam);
    a.label(not_lbdown);
    const int not_lbup = a.newLabel();
    a.cmpEaxImm(0x0202u);  // WM_LBUTTONUP
    a.jnz(not_lbup);
    a.movMemImm(kLbUpMsg, 1u);
    a.movEaxMem(kMsg + 8u);
    a.movMemEax(kLbUpWparam);
    a.movEaxMem(kMsg + 12u);
    a.movMemEax(kLbUpLparam);
    a.label(not_lbup);
    const int not_keydown = a.newLabel();
    a.cmpEaxImm(0x0100u);  // WM_KEYDOWN
    a.jnz(not_keydown);
    a.movMemImm(kKeyDownMsg, 1u);
    a.movEaxMem(kMsg + 8u);
    a.movMemEax(kKeyWparam);
    a.label(not_keydown);
    const int not_keyup = a.newLabel();
    a.cmpEaxImm(0x0101u);  // WM_KEYUP
    a.jnz(not_keyup);
    a.movMemImm(kKeyUpMsg, 1u);
    a.movEaxMem(kMsg + 8u);
    a.movMemEax(kKeyUpWparam);
    a.label(not_keyup);
    // DispatchMessageA(&msg)：真的进 guest 的 WndProc（引擎的事件锁存就走这条）
    a.pushImm(kMsg);
    a.callRel(dispatch);
    a.label(no_msg);

    // 曾经按下 + 现在抬起 ⇒ 抬起发生在**按下之后**（click 跨帧的判据）。
    a.pushImm(0x01u);
    a.callRel(get_async);
    a.testEaxImm(0x8000u);
    const int still_down = a.newLabel();
    a.jnz(still_down);
    a.movEaxMem(kDownSeen);
    a.testEaxImm(0xFFFFFFFFu);
    a.jz(still_down);
    a.movMemImm(kReleaseAfterDown, 1u);
    a.label(still_down);

    // rounds++
    a.movEaxMem(kRounds);
    a.addEaxImm(1u);
    a.movMemEax(kRounds);
    a.ret();
    if (!a.patch(err)) return false;
    *out = std::move(a.b);
    return true;
}

// guest WndProc：记录消息 / wParam，并对 WM_LBUTTONDOWN 打标记；返回 0x1234（stdcall，ret 16）。
bool buildWndProc(std::vector<std::uint8_t>* out, std::string* err) {
    Asm a(kProc);
    // mov eax, [esp+8]（WndProc(hwnd, msg, wparam, lparam)：msg 在 esp+8）
    a.u8(0x8B);
    a.u8(0x44);
    a.u8(0x24);
    a.u8(0x08);
    a.movMemEax(kDispatchId);
    // mov ecx, [esp+12]
    a.u8(0x8B);
    a.u8(0x4C);
    a.u8(0x24);
    a.u8(0x0C);
    a.movMemEcx(kDispatchWparam);
    a.cmpEaxImm(0x0201u);  // WM_LBUTTONDOWN ?
    const int skip = a.newLabel();
    a.jnz(skip);
    a.movMemImm(kDispatchLbDown, 1u);
    a.u8(0x8B);  // mov ecx, [esp+12]
    a.u8(0x4C);
    a.u8(0x24);
    a.u8(0x0C);
    a.movMemEcx(kDispatchLbWparam);
    a.label(skip);
    const int skip_key = a.newLabel();
    a.cmpEaxImm(0x0100u);  // WM_KEYDOWN ?
    a.jnz(skip_key);
    a.movMemImm(kDispatchKeyDown, 1u);
    a.u8(0x8B);  // mov ecx, [esp+12]
    a.u8(0x4C);
    a.u8(0x24);
    a.u8(0x0C);
    a.movMemEcx(kDispatchKeyWparam);
    a.label(skip_key);
    a.movEaxImm(0x1234u);
    a.retStdcall(16u);
    if (!a.patch(err)) return false;
    *out = std::move(a.b);
    return true;
}

void resetObservation(lift::Machine& m) {
    for (std::uint32_t off = 0; off < kResBytes; off += 4u) m.mem().wr32(kRes + off, 0u);
}

// ★ **伪造的 guest 输入消费者**（`click-spans-sample` / `key-up-waits-sample` 用）：每 `burn*2` 条指令
//   （≈ 50 ms guest 时间）做一次**电平采样** —— `GetAsyncKeyState(VK_LBUTTON)` / `GetAsyncKeyState(VK_RETURN)`
//   （真的按下才 +1 计数）＋ `GetCursorPos`。⇒ 它把"注入层是否等到 guest 采样"变成 guest 内存里的
//   三个可数事实：`kDownSeenPolls` / `kKeyDownSeenPolls` / `kPollCount`。
bool buildConsumerBody(std::vector<std::uint8_t>* out, std::uint32_t get_cursor, std::uint32_t get_async,
                       std::uint32_t burn, std::string* err) {
    Asm a(kConsumer);
    const int top = a.newLabel();
    const int inner = a.newLabel();
    const int no_down = a.newLabel();
    a.label(top);
    a.movEcxImm(burn);
    a.label(inner);
    a.decEcx();
    a.jnz(inner);  // 烧掉 2*burn 条指令（≈ 一个采样周期）
    // GetAsyncKeyState(VK_LBUTTON) —— 电平采样点 ①
    a.pushImm(0x01u);
    a.callRel(get_async);
    a.testEaxImm(0x8000u);
    a.jz(no_down);
    a.movEaxMem(kDownSeenPolls);
    a.addEaxImm(1u);
    a.movMemEax(kDownSeenPolls);
    a.label(no_down);
    // GetAsyncKeyState(VK_RETURN) —— 电平采样点 ②（键盘同理）
    a.pushImm(0x0Du);
    a.callRel(get_async);
    a.testEaxImm(0x8000u);
    const int no_key = a.newLabel();
    a.jz(no_key);
    a.movEaxMem(kKeyDownSeenPolls);
    a.addEaxImm(1u);
    a.movMemEax(kKeyDownSeenPolls);
    a.label(no_key);
    // GetCursorPos(&pt) —— 采样点 ③（引擎每迭代的位置采样；实测 20 Hz）
    a.pushImm(kPoint);
    a.callRel(get_cursor);
    a.movEaxMem(kPollCount);
    a.addEaxImm(1u);
    a.movMemEax(kPollCount);
    a.jmp32(top);  // 永不返回 ⇒ 测试用 `max_insns` 分块推进
    if (!a.patch(err)) return false;
    *out = std::move(a.b);
    return true;
}

// ★ **完全不采样**的 guest（`click-bounded-without-sampling` / `click-honors-explicit-hold` 用）：
//   只烧指令，一次输入 API 也不调 ⇒ 采样计数**不动**。用来钉"到上限仍然抬起 + 告警"与
//   "显式 hold 不看采样"。
bool buildSpinnerBody(std::vector<std::uint8_t>* out, std::uint32_t burn, std::string* err) {
    Asm a(kSpinner);
    const int top = a.newLabel();
    const int inner = a.newLabel();
    a.label(top);
    a.movEcxImm(burn);
    a.label(inner);
    a.decEcx();
    a.jnz(inner);
    a.jmp32(top);
    if (!a.patch(err)) return false;
    *out = std::move(a.b);
    return true;
}

// 推进一块 guest 执行（`max_insns` 是**累计值** ⇒ 这里传"再跑多少条"）。
bool runChunk(lift::Machine& m, std::uint64_t insns) {
    lift::RunOptions opt;
    opt.max_insns = m.cpu().insns + insns;
    opt.time_limit_sec = 60.0;  // 兜底：⛔ 守卫不许挂死
    const lift::RunResult r = m.run(opt);
    if (!r.ok()) {
        std::fprintf(stderr, "chunk 停在 %s：%s\n", lift::stopReasonName(r.reason), r.detail.c_str());
        return false;
    }
    return true;
}

// 脚本用**绝对帧号**触发（每段用例起点的 frameCount 不同，⛔ 不要写死 frame 1）。
std::uint64_t nextFrame(lift::Machine& m) { return m.presenter().frameCount() + 1u; }

// 丢掉队列里的历史消息（每段用例从干净的输入队列开始）。
void drainMessages(lift::WindowManager& wm) {
    lift::WinMessage ignored;
    while (wm.peek(&ignored, true)) {
    }
}

bool runOneRound(lift::Machine& m) {
    lift::RunOptions opt;
    opt.until_ea = kSentinel;
    m.cpu().eip = kCode;
    // ★ 每轮都重置栈（上一轮的 `ret` 已经把哨兵弹掉了；不重放的话第二轮 ret 会读空栈）。
    m.cpu().esp() = m.layout().stack_top - 4u;
    m.mem().wr32(m.cpu().esp(), kSentinel);
    const lift::RunResult r = m.run(opt);
    if (r.reason != lift::StopReason::UntilEa && r.reason != lift::StopReason::Stopped) {
        std::fprintf(stderr, "round 停在 %s：%s\n", lift::stopReasonName(r.reason), r.detail.c_str());
        return false;
    }
    return true;
}

}  // namespace

int main() {
    using namespace lift_test;
    // ★ 先断言"这个进程没有初始化过 SDL 视频/音频"——注入了却靠 SDL 事件泵 = 假绿。
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_AUDIO), 0u);

    lift::Machine m;
    std::string err;
    g_current_case = "setup";
    LIFT_CHECK(m.initBare(&err));
    LIFT_CHECK(m.prepareStack(&err));
    lift::registerUser32WindowShims(m.shims());  // GetAsyncKeyState / PeekMessageA / ScreenToClient / DispatchMessageA
    lift::registerUser32InputShims(m.shims());   // GetCursorPos
    for (std::uint32_t id = 0; id < m.shims().all().size(); ++id) {
        lift::ShimDef* d = const_cast<lift::ShimDef*>(m.shims().byId(id));
        if (!d || !d->fn) continue;
        d->stub_ea = kStubBase + id * 4u;
        m.cache().installShim(d->stub_ea, d->id);
    }
    const std::uint32_t get_cursor = stubOf(m, "GetCursorPos");
    const std::uint32_t screen_to_client = stubOf(m, "ScreenToClient");
    const std::uint32_t get_async = stubOf(m, "GetAsyncKeyState");
    const std::uint32_t peek = stubOf(m, "PeekMessageA");
    const std::uint32_t dispatch = stubOf(m, "DispatchMessageA");
    LIFT_CHECK_NE_U32(get_cursor, 0u);
    LIFT_CHECK_NE_U32(screen_to_client, 0u);
    LIFT_CHECK_NE_U32(get_async, 0u);
    LIFT_CHECK_NE_U32(peek, 0u);
    LIFT_CHECK_NE_U32(dispatch, 0u);
    if (get_cursor == 0 || screen_to_client == 0 || get_async == 0 || peek == 0 || dispatch == 0) {
        return lift_test::finish("input-injection");
    }

    // ★ 非零窗口原点：注入给的是**客户区**坐标，`GetCursorPos` 必须返回屏幕坐标（+ 原点），
    //   而 `ScreenToClient` 必须把它减回注入坐标。
    lift::WindowManager* wm = m.cpu().windows;
    LIFT_CHECK(wm != nullptr);
    if (wm == nullptr) return lift_test::finish("input-injection");
    const std::uint32_t class_atom = wm->registerClass("InputInjection", kProc, 0x10000000u);
    LIFT_CHECK_NE_U32(class_atom, 0u);
    const std::uint32_t win = wm->createWindow("InputInjection", 0x10000000u, 0, kWinX, kWinY, 1280, 720, 0);
    LIFT_CHECK_NE_U32(win, 0u);
    LIFT_CHECK(wm->find(win) != nullptr);
    LIFT_CHECK_EQ_U32(wm->find(win) ? wm->find(win)->proc : 0u, kProc);  // WndProc 真的挂上了

    std::vector<std::uint8_t> body;
    std::vector<std::uint8_t> proc;
    std::string aerr;
    LIFT_CHECK(buildGuestBody(&body, get_cursor, screen_to_client, get_async, peek, dispatch, win, &aerr));
    LIFT_CHECK(buildWndProc(&proc, &aerr));
    if (body.empty() || proc.empty()) return lift_test::finish("input-injection");
    LIFT_CHECK(m.placeCode(kCode, body.data(), body.size(), &err));
    LIFT_CHECK(m.placeCode(kProc, proc.data(), proc.size(), &err));
    std::vector<std::uint8_t> zeros(kResBytes, 0u);
    LIFT_CHECK(m.placeData(kRes, zeros.data(), kResBytes, &err));
    LIFT_CHECK(m.placeData(kMsg, zeros.data(), 28u, &err));
    LIFT_CHECK(m.placeData(kPoint, zeros.data(), 8u, &err));

    lift::SurfaceFormat fmt;
    fmt.bpp = 32;
    const std::uint32_t surf = m.surfaces().create(4, 3, fmt);
    LIFT_CHECK_NE_U32(surf, 0u);
    lift::GuestSurface* gs = m.surfaces().find(surf);
    LIFT_CHECK(gs != nullptr);
    if (gs == nullptr) return lift_test::finish("input-injection");

    // 起跑前：没有键按下、光标仍在 (0,0) —— 本进程里没有任何人动过真实光标。
    LIFT_CHECK(!wm->keyDown(0x01u));
    LIFT_CHECK(!wm->keyDown(0x0Du));
    {
        int cx = -1;
        int cy = -1;
        wm->getCursorPos(&cx, &cy);
        LIFT_CHECK_EQ_I64(cx, 0);
        LIFT_CHECK_EQ_I64(cy, 0);
    }

    // =================================================================================
    // ① `at guest_ms`：headless 时间线（不 Present、不建 SDL 视频）⇒ guest 看到按键 / 光标 / 消息
    // =================================================================================
    g_current_case = "script-guest-ms";
    {
        const std::string script =
            "# 注入通道自检（pure CTest）\n"
            "at guest_ms 3\n"
            "mouse_move 812 622\n"
            "wait 1 ms\n"
            "mouse_down\n"
            "wait 2 ms\n"
            "key_down 0x0D\n"
            "wait 1 ms\n"
            "key_up 0x0D\n"
            "wait 2 ms\n"
            "mouse_up\n"
            "wait 1 ms\n"
            "quit\n";
        LIFT_CHECK(m.inputScript().loadText(script, "inline:guest-ms", &err));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.inputScript().actionCount()), 11u);
        drainMessages(*wm);
        resetObservation(m);

        bool checked_priority = false;
        int rounds = 0;
        for (; rounds < 40000; ++rounds) {
            if (m.inputScript().quitRequested()) break;
            if (!runOneRound(m)) {
                LIFT_CHECK(false);
                break;
            }
            if (!checked_priority && wm->keyDown(0x01u)) {
                checked_priority = true;
                g_current_case = "script-priority";
                // ④ 脚本拥有 VK_LBUTTON：OS 的抬起 + 焦点丢失都不能把它清掉。
                LIFT_CHECK(!wm->setKeyDown(lift::InputSource::Os, 0x01u, false));
                LIFT_CHECK(wm->keyDown(0x01u));
                wm->clearOsInput();
                LIFT_CHECK(wm->keyDown(0x01u));
                int cx = 0;
                int cy = 0;
                wm->getCursorPos(&cx, &cy);
                LIFT_CHECK(!wm->setCursorClientPos(win, 1, 1, lift::InputSource::Os));
                int ax = 0;
                int ay = 0;
                wm->getCursorPos(&ax, &ay);
                LIFT_CHECK_EQ_I64(ax, cx);
                LIFT_CHECK_EQ_I64(ay, cy);
                g_current_case = "script-guest-ms";
            }
        }
        LIFT_CHECK(m.inputScript().quitRequested());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(m.inputScript().appliedCount()),
                          static_cast<std::int64_t>(m.inputScript().actionCount()));
        LIFT_CHECK(checked_priority);  // 按下确实被观察到过（否则下面那些断言没意义）
        // 结束时键 / 按钮都已抬起（脚本自己 mouse_up / key_up）；所有权交还 OS。
        LIFT_CHECK(!wm->keyDown(0x01u));
        LIFT_CHECK(!wm->keyDown(0x0Du));
        LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);
        LIFT_CHECK(!wm->cursorOwnedByScript());

        // guest 侧观测
        LIFT_CHECK_EQ_U32(m.mem().rd32(kLastX), static_cast<std::uint32_t>(kClickX + kWinX));
        LIFT_CHECK_EQ_U32(m.mem().rd32(kLastY), static_cast<std::uint32_t>(kClickY + kWinY));
        LIFT_CHECK_EQ_U32(m.mem().rd32(kClientX), static_cast<std::uint32_t>(kClickX));
        LIFT_CHECK_EQ_U32(m.mem().rd32(kClientY), static_cast<std::uint32_t>(kClickY));
        LIFT_CHECK_EQ_U32(m.mem().rd32(kDownSeen), 1u);   // GetAsyncKeyState(1) == 0x8000
        LIFT_CHECK_EQ_U32(m.mem().rd32(kKeySeen), 1u);    // GetAsyncKeyState(0x0D) == 0x8000
        LIFT_CHECK_EQ_U32(m.mem().rd32(kLbDownMsg), 1u);  // PeekMessageA 取到 WM_LBUTTONDOWN
        LIFT_CHECK_EQ_U32(m.mem().rd32(kLbUpMsg), 1u);    // … 与 WM_LBUTTONUP
        LIFT_CHECK_EQ_U32(m.mem().rd32(kKeyDownMsg), 1u); // … 与 WM_KEYDOWN
        LIFT_CHECK_EQ_U32(m.mem().rd32(kKeyUpMsg), 1u);   // … 与 WM_KEYUP
        LIFT_CHECK_EQ_U32(m.mem().rd32(kMsgWparam) & 0xFFFFu, 0x0001u);  // MK_LBUTTON
        LIFT_CHECK_EQ_U32(m.mem().rd32(kMsgLparam),
                          (static_cast<std::uint32_t>(kClickY) << 16) | static_cast<std::uint32_t>(kClickX));
        LIFT_CHECK_EQ_U32(m.mem().rd32(kKeyWparam), 0x0Du);
        // 抬起消息的 payload：按钮位已清、坐标仍是同一处；键码仍是同一个 VK。
        LIFT_CHECK_EQ_U32(m.mem().rd32(kLbUpWparam), 0u);
        LIFT_CHECK_EQ_U32(m.mem().rd32(kLbUpLparam),
                          (static_cast<std::uint32_t>(kClickY) << 16) | static_cast<std::uint32_t>(kClickX));
        LIFT_CHECK_EQ_U32(m.mem().rd32(kKeyUpWparam), 0x0Du);
        LIFT_CHECK_EQ_U32(m.mem().rd32(kReleaseAfterDown), 1u);
        LIFT_CHECK_EQ_U32(m.mem().rd32(kDispatchLbDown), 1u);                 // 真的进了 WndProc
        LIFT_CHECK_EQ_U32(m.mem().rd32(kDispatchLbWparam) & 0x0001u, 0x0001u);
        LIFT_CHECK_EQ_U32(m.mem().rd32(kDispatchKeyDown), 1u);                // 键盘消息也进了 WndProc
        LIFT_CHECK_EQ_U32(m.mem().rd32(kDispatchKeyWparam), 0x0Du);
        LIFT_CHECK(m.mem().rd32(kRounds) > 1u);
        // ★ 跑到这里进程仍未初始化 SDL（注入不靠 SDL 事件泵）。
        LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
        LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_AUDIO), 0u);
        std::printf("script-guest-ms: %d 轮，%zu 条动作全部应用\n", rounds, m.inputScript().actionCount());
    }

    // =================================================================================
    // ② `at frame` + `click`：帧触发；按下 / 抬起必须跨帧（guest 两态都采到）
    // =================================================================================
    g_current_case = "script-frame";
    {
        const std::string script =
            "at frame 1\n"
            "click 100 200 hold 1\n"
            "wait 3 frames\n"
            "quit\n";
        LIFT_CHECK(m.inputScript().loadText(script, "inline:frame", &err));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.inputScript().actionCount()), 3u);
        drainMessages(*wm);
        resetObservation(m);
        wm->setCursorPos(0, 0);  // 观测起点（guest 自己的 API；不改所有权）

        // 还没有 Present 过任何一帧 ⇒ `at frame 1` 不触发。
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.presenter().frameCount()), 0u);
        LIFT_CHECK(runOneRound(m));
        LIFT_CHECK_EQ_U32(m.mem().rd32(kRounds), 1u);
        LIFT_CHECK(!m.inputScript().quitRequested());
        LIFT_CHECK(!wm->keyDown(0x01u));  // 没有提前按下
        {
            int cx = 0;
            int cy = 0;
            wm->getCursorPos(&cx, &cy);
            LIFT_CHECK_EQ_I64(cx, 0);
            LIFT_CHECK_EQ_I64(cy, 0);
        }

        // Present 第 1 帧 ⇒ 下一次块边界应用 move + down（up 排到第 2 帧）。
        LIFT_CHECK(m.presenter().present(m.surfaces(), *gs, "", &err));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.presenter().frameCount()), 1u);
        LIFT_CHECK(runOneRound(m));
        LIFT_CHECK(wm->keyDown(0x01u));
        {
            int cx = 0;
            int cy = 0;
            wm->getCursorPos(&cx, &cy);
            LIFT_CHECK_EQ_I64(cx, 100 + kWinX);
            LIFT_CHECK_EQ_I64(cy, 200 + kWinY);
        }

        // Present 第 2 帧 ⇒ click 的抬起到期。
        LIFT_CHECK(m.presenter().present(m.surfaces(), *gs, "", &err));
        LIFT_CHECK(runOneRound(m));
        LIFT_CHECK(!wm->keyDown(0x01u));

        // Present 第 3 / 4 帧 ⇒ `wait 3 frames` 的 quit 到期（1 + 3 = 4）。
        LIFT_CHECK(m.presenter().present(m.surfaces(), *gs, "", &err));
        LIFT_CHECK(runOneRound(m));
        LIFT_CHECK(!m.inputScript().quitRequested());
        LIFT_CHECK(m.presenter().present(m.surfaces(), *gs, "", &err));
        LIFT_CHECK(runOneRound(m));
        LIFT_CHECK(m.inputScript().quitRequested());

        LIFT_CHECK_EQ_U32(m.mem().rd32(kLastX), static_cast<std::uint32_t>(100 + kWinX));
        LIFT_CHECK_EQ_U32(m.mem().rd32(kLastY), static_cast<std::uint32_t>(200 + kWinY));
        LIFT_CHECK_EQ_U32(m.mem().rd32(kDownSeen), 1u);
        LIFT_CHECK_EQ_U32(m.mem().rd32(kLbDownMsg), 1u);
        LIFT_CHECK_EQ_U32(m.mem().rd32(kLbUpMsg), 1u);
        LIFT_CHECK_EQ_U32(m.mem().rd32(kReleaseAfterDown), 1u);  // 跨帧：按下之后才看到抬起
        LIFT_CHECK_EQ_U32(m.mem().rd32(kDispatchLbDown), 1u);
        LIFT_CHECK(!wm->cursorOwnedByScript());                  // 脚本结束后所有权已交还
        LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);
    }

    // =================================================================================
    // ③ 脚本以"仍按着"收口（quit 时 click 的 release 未到期 + key 没抬）⇒ 必须补发 UP
    // =================================================================================
    g_current_case = "script-finish-releases";
    {
        wm->clearKeys();
        wm->setCursorPos(0, 0);
        drainMessages(*wm);
        resetObservation(m);
        const std::string script = "at frame " + std::to_string(nextFrame(m)) +
                                   "\nmouse_move 50 60\nmouse_down\nkey_down 0x0D\nquit\n";
        LIFT_CHECK(m.inputScript().loadText(script, "inline:finish", &err));
        LIFT_CHECK(m.presenter().present(m.surfaces(), *gs, "", &err));
        LIFT_CHECK(runOneRound(m));
        LIFT_CHECK(m.inputScript().quitRequested());
        // quit 收口：脚本按下的左键 / Enter 必须已被抬起（状态 + 消息）。
        LIFT_CHECK(!wm->keyDown(0x01u));
        LIFT_CHECK(!wm->keyDown(0x0Du));
        LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);
        // ★ `quit` 之后 `Machine::run` 会立刻停（脚本还在 quitRequested 状态）⇒ 要让 guest 把补发的
        //   WM_LBUTTONUP / WM_KEYUP 取走，先把脚本卸掉再跑几轮。
        m.inputScript().clear();
        for (int i = 0; i < 12 && (m.mem().rd32(kLbUpMsg) == 0u || m.mem().rd32(kKeyUpMsg) == 0u); ++i) {
            LIFT_CHECK(runOneRound(m));
        }
        LIFT_CHECK_EQ_U32(m.mem().rd32(kLbUpMsg), 1u);
        LIFT_CHECK_EQ_U32(m.mem().rd32(kKeyUpMsg), 1u);
    }

    // =================================================================================
    // ④ 脚本接管光标时，**真人松开按钮**必须被接受（否则 VK_LBUTTON 电平卡在按下）
    // =================================================================================
    g_current_case = "os-release-while-script-owns-cursor";
    {
        wm->clearKeys();
        wm->setCursorPos(0, 0);
        resetObservation(m);
        // 真人按下左键（OS 来源）
        LIFT_CHECK(lift::input::mouseButton(*wm, 10, 10, lift::MouseButton::Left, true,
                                            lift::InputSource::Os, 0u));
        LIFT_CHECK(wm->keyDown(0x01u));
        LIFT_CHECK_EQ_U32(wm->mouseButtons(), lift::input::kMkLButton);
        const std::string script = "at frame " + std::to_string(nextFrame(m)) +
                                   "\nmouse_move 300 400\nwait 5 frames\nquit\n";
        LIFT_CHECK(m.inputScript().loadText(script, "inline:osup", &err));
        LIFT_CHECK(m.presenter().present(m.surfaces(), *gs, "", &err));
        LIFT_CHECK(runOneRound(m));
        LIFT_CHECK(wm->cursorOwnedByScript());
        // OS 的按下在光标被脚本拥有时被拒（消息与 GetCursorPos 不能两套话）……
        LIFT_CHECK(!lift::input::mouseButton(*wm, 10, 10, lift::MouseButton::Left, true,
                                             lift::InputSource::Os, 0u));
        // ……但 OS 的**抬起**必须接受。
        LIFT_CHECK(lift::input::mouseButton(*wm, 10, 10, lift::MouseButton::Left, false,
                                            lift::InputSource::Os, 0u));
        LIFT_CHECK(!wm->keyDown(0x01u));
        LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);
    }

    // =================================================================================
    // ⑤ 同一帧里"上一 click 的抬起 + 本帧显式 mouse_down" ⇒ 读作 down（延迟抬起先处理）
    // =================================================================================
    g_current_case = "release-before-same-frame-down";
    {
        wm->clearKeys();
        wm->setCursorPos(0, 0);
        drainMessages(*wm);
        resetObservation(m);
        const std::uint64_t f = nextFrame(m);
        const std::string script = "at frame " + std::to_string(f) +
                                   "\nclick 100 200 hold 1\nat frame " + std::to_string(f + 1) +
                                   "\nmouse_down\nwait 1 frames\nquit\n";
        LIFT_CHECK(m.inputScript().loadText(script, "inline:order", &err));
        LIFT_CHECK(m.presenter().present(m.surfaces(), *gs, "", &err));  // frame f
        for (int i = 0; i < 4; ++i) LIFT_CHECK(runOneRound(m));
        LIFT_CHECK(wm->keyDown(0x01u));  // click 按下
        LIFT_CHECK(m.presenter().present(m.surfaces(), *gs, "", &err));  // frame f+1
        for (int i = 0; i < 8; ++i) LIFT_CHECK(runOneRound(m));
        // 同一帧里 click 的 up 先落地，显式 mouse_down 后落地 ⇒ 最终仍是按下。
        LIFT_CHECK(wm->keyDown(0x01u));
        LIFT_CHECK_EQ_U32(m.mem().rd32(kLbDownMsg), 1u);
        LIFT_CHECK_EQ_U32(m.mem().rd32(kLbUpMsg), 1u);
        LIFT_CHECK(m.presenter().present(m.surfaces(), *gs, "", &err));  // frame f+2
        LIFT_CHECK(runOneRound(m));
        LIFT_CHECK(m.inputScript().quitRequested());
        LIFT_CHECK(!wm->keyDown(0x01u));
    }

    // =================================================================================
    // ⑥ 换脚本（`loadInputScript`）时旧脚本仍按着 ⇒ 必须先抬起（不静默留下卡住的键）
    // =================================================================================
    g_current_case = "script-replaced-while-holding";
    {
        wm->clearKeys();
        wm->setCursorPos(0, 0);
        drainMessages(*wm);
        resetObservation(m);
        const std::uint64_t f = nextFrame(m);
        const std::string hold = "at frame " + std::to_string(f) +
                                 "\nmouse_move 70 80\nmouse_down\nkey_down 0x0D\nwait 5 frames\n";
        LIFT_CHECK(m.inputScript().loadText(hold, "inline:hold", &err));
        LIFT_CHECK(m.presenter().present(m.surfaces(), *gs, "", &err));
        LIFT_CHECK(runOneRound(m));
        LIFT_CHECK(wm->keyDown(0x01u));
        LIFT_CHECK(wm->keyDown(0x0Du));
        // 换脚本：旧脚本仍按着的输入必须被抬起并交还所有权。
        m.inputScript().abandon(*wm, 0u);
        LIFT_CHECK(!wm->keyDown(0x01u));
        LIFT_CHECK(!wm->keyDown(0x0Du));
        LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);
        LIFT_CHECK(!wm->cursorOwnedByScript());
        LIFT_CHECK(!m.inputScript().active());
    }

    // =================================================================================
    // ⑦ `input-injection/click-spans-sample`：一次 `click` 的 down 与 up 之间**至少跨过一个
    //    guest 采样点**（且 up 的时刻 ≥ 那个采样点 + 一个采样周期）。
    // ★ 夹具：一个**每 50 ms guest 时间采样一次**的伪造消费者（= 实测房间的 20 Hz）。
    //   红了意味着：`click` 又退回"同一 tick 内 down+up"⇒ 按下被漏掉（用户实测的那个 bug）。
    // =================================================================================
    g_current_case = "input-injection/click-spans-sample";
    {
        wm->clearKeys();
        wm->setCursorPos(0, 0);
        drainMessages(*wm);
        resetObservation(m);
        std::vector<std::uint8_t> consumer;
        LIFT_CHECK(buildConsumerBody(&consumer, get_cursor, get_async, kSampleBurn, &aerr));
        if (consumer.empty()) {
            LIFT_CHECK(false);
        } else {
            LIFT_CHECK(m.placeCode(kConsumer, consumer.data(), consumer.size(), &err));
            // ★ 虚拟时钟是**累计**的（前面几段用例已经跑掉一大截）⇒ 触发时刻按"现在 + 偏移"写，
            //   ⛔ 不要写死 `at guest_ms 100`（那在过去 ⇒ 第一个 tick 就触发）。
            const std::uint64_t base_ms = m.clock().nowMs();
            const std::string script =
                "# 伪造 50 ms 周期的 guest 输入消费者：一次 click 必须跨过它的采样点\n"
                "at guest_ms " + std::to_string(base_ms + 120ull) + "\n"
                "click 300 400\n"
                "at guest_ms " + std::to_string(base_ms + 900ull) + "\n"
                "quit\n";
            LIFT_CHECK(m.inputScript().loadText(script, "inline:click-spans-sample", &err));
            const std::uint64_t samples_before = wm->inputSampleCount();
            m.cpu().eip = kConsumer;
            // 每块 500k 条 = 50 ms guest 时间；脚本到 900 ms 就 quit ⇒ 20 块足够，40 块是兜底。
            bool stopped = false;
            for (int chunk = 0; chunk < 40; ++chunk) {
                if (!runChunk(m, 500000ull)) {
                    LIFT_CHECK(false);
                    break;
                }
                if (m.inputScript().quitRequested()) {
                    stopped = true;
                    break;
                }
            }
            LIFT_CHECK(stopped);
            LIFT_CHECK(m.inputScript().quitRequested());
            const lift::InputScript::ReleaseStats& rs = m.inputScript().releaseStats();
            const lift::InputScript::ReleaseInfo& lr = m.inputScript().lastRelease();
            LIFT_CHECK(lr.valid);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rs.releases), 1);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rs.auto_releases), 1);   // 走的是自动 hold
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rs.explicit_releases), 0);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rs.bounded_no_sample), 0);
            LIFT_CHECK(lr.auto_hold);
            // ① down 与 up 之间至少跨过一个采样点（计数 +1）
            LIFT_CHECK(lr.release_samples > lr.press_samples);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lr.release_samples - lr.press_samples),
                              static_cast<std::int64_t>(rs.waited_samples_max));
            // ② up 的时刻 ≥ 那个采样点 + 一个采样周期（夹具周期 = 50 ms ⇒ 40 ms 留测量余量）
            LIFT_CHECK(lr.observe_ms >= lr.press_ms);
            LIFT_CHECK(lr.release_ms >= lr.observe_ms);
            LIFT_CHECK(lr.release_ms - lr.observe_ms >= 40);
            LIFT_CHECK(lr.release_ms > lr.press_ms);
            // ③ guest 侧**真的看见了按下**（这才是"没漏"的判据，不是"我们等了"）
            LIFT_CHECK(m.mem().rd32(kDownSeenPolls) >= 1u);
            LIFT_CHECK(m.mem().rd32(kPollCount) >= 3u);
            LIFT_CHECK(wm->inputSampleCount() > samples_before);
            LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);  // 已抬起
            LIFT_CHECK(!wm->keyDown(0x01u));
            std::printf("click-spans-sample: polls=%u down-seen=%u press_samples=%llu release_samples=%llu "
                        "hold_ms=%llu post_sample_ms=%llu\n",
                        m.mem().rd32(kPollCount), m.mem().rd32(kDownSeenPolls),
                        (unsigned long long)lr.press_samples, (unsigned long long)lr.release_samples,
                        (unsigned long long)(lr.release_ms - lr.press_ms),
                        (unsigned long long)(lr.release_ms - lr.observe_ms));
        }
    }

    // =================================================================================
    // ⑧ `input-injection/key-up-waits-sample`：**键盘同理** —— 同一 tick 里的 `key_down` + `key_up`
    //   不许把 `key_up` 立刻落地：它必须等到 guest 采样过"按下"之后。
    // ★ 判据与 `click-spans-sample` 同一条（电平轮询 = `GetAsyncKeyState`）。
    // =================================================================================
    g_current_case = "input-injection/key-up-waits-sample";
    {
        wm->clearKeys();
        wm->setCursorPos(0, 0);
        drainMessages(*wm);
        resetObservation(m);
        std::vector<std::uint8_t> consumer;
        LIFT_CHECK(buildConsumerBody(&consumer, get_cursor, get_async, kSampleBurn, &aerr));
        if (consumer.empty()) {
            LIFT_CHECK(false);
        } else {
            LIFT_CHECK(m.placeCode(kConsumer, consumer.data(), consumer.size(), &err));
            const std::uint64_t base_ms = m.clock().nowMs();
            const std::string script =
                "at guest_ms " + std::to_string(base_ms + 120ull) + "\n"
                "key_down 0x0D\n"
                "key_up 0x0D\n"  // 与 down 同一个触发条件 ⇒ 同一 tick
                "at guest_ms " + std::to_string(base_ms + 900ull) + "\n"
                "quit\n";
            LIFT_CHECK(m.inputScript().loadText(script, "inline:key-up-waits-sample", &err));
            m.cpu().eip = kConsumer;
            bool stopped = false;
            for (int chunk = 0; chunk < 40; ++chunk) {
                if (!runChunk(m, 500000ull)) {
                    LIFT_CHECK(false);
                    break;
                }
                if (m.inputScript().quitRequested()) {
                    stopped = true;
                    break;
                }
            }
            LIFT_CHECK(stopped);
            const lift::InputScript::ReleaseStats& rs = m.inputScript().releaseStats();
            const lift::InputScript::ReleaseInfo& lr = m.inputScript().lastRelease();
            LIFT_CHECK(lr.valid);
            LIFT_CHECK(lr.is_key);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lr.vk), 0x0Du);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rs.deferred_ups), 1);  // key_up 被推迟过
            LIFT_CHECK(lr.auto_hold);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rs.bounded_no_sample), 0);
            // 抬起之前 guest 至少采样过一次，且又等了一个周期
            LIFT_CHECK(lr.release_samples > lr.press_samples);
            LIFT_CHECK(lr.release_ms - lr.observe_ms >= 40);
            // guest 侧真的看见了"键按着"
            LIFT_CHECK(m.mem().rd32(kKeyDownSeenPolls) >= 1u);
            LIFT_CHECK(!wm->keyDown(0x0Du));
            std::printf("key-up-waits-sample: key-down-seen=%u deferred=%llu press_samples=%llu "
                        "release_samples=%llu\n",
                        m.mem().rd32(kKeyDownSeenPolls), (unsigned long long)rs.deferred_ups,
                        (unsigned long long)lr.press_samples, (unsigned long long)lr.release_samples);
        }
    }

    // =================================================================================
    // ⑨ `input-injection/click-honors-explicit-hold`：显式 `hold N` **不**采用自动值。
    // ★ 夹具：guest **完全不采样**（只烧指令）⇒ 自动 hold 会一直等到上限；
    //   而显式 `hold 2` 必须在第 +2 帧抬起（帧号 / 采样计数两个断言同时钉住）。
    // =================================================================================
    g_current_case = "input-injection/click-honors-explicit-hold";
    {
        wm->clearKeys();
        wm->setCursorPos(0, 0);
        drainMessages(*wm);
        resetObservation(m);
        std::vector<std::uint8_t> spinner;
        LIFT_CHECK(buildSpinnerBody(&spinner, kSampleBurn, &aerr));
        if (spinner.empty()) {
            LIFT_CHECK(false);
        } else {
            LIFT_CHECK(m.placeCode(kSpinner, spinner.data(), spinner.size(), &err));
            const std::uint64_t f = nextFrame(m);
            const std::string script = "at frame " + std::to_string(f) +
                                       "\nclick 100 200 hold 2\nwait 3 frames\nquit\n";
            LIFT_CHECK(m.inputScript().loadText(script, "inline:click-honors-explicit-hold", &err));
            m.cpu().eip = kSpinner;
            LIFT_CHECK(m.presenter().present(m.surfaces(), *gs, "", &err));
            LIFT_CHECK(runChunk(m, 200000ull));
            LIFT_CHECK(wm->keyDown(0x01u));
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(m.inputScript().releaseStats().releases), 0);
            // 帧 f+1：仍按着（显式 hold = 2 帧，不是"同一帧 down+up"）
            LIFT_CHECK(m.presenter().present(m.surfaces(), *gs, "", &err));
            LIFT_CHECK(runChunk(m, 200000ull));
            LIFT_CHECK(wm->keyDown(0x01u));
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(m.inputScript().releaseStats().releases), 0);
            // 帧 f+2：抬起
            LIFT_CHECK(m.presenter().present(m.surfaces(), *gs, "", &err));
            LIFT_CHECK(runChunk(m, 200000ull));
            LIFT_CHECK(!wm->keyDown(0x01u));
            const lift::InputScript::ReleaseStats& rs = m.inputScript().releaseStats();
            const lift::InputScript::ReleaseInfo& lr = m.inputScript().lastRelease();
            LIFT_CHECK(lr.valid);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rs.explicit_releases), 1);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rs.auto_releases), 0);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rs.bounded_no_sample), 0);  // 不是"等到上限"
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rs.deferred_ups), 0);
            LIFT_CHECK(!lr.auto_hold);                                             // 显式路径
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lr.press_frame), static_cast<std::int64_t>(f));
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lr.release_frame), static_cast<std::int64_t>(f + 2));
            // ★ 关键：guest 一次也没采样，up 仍然按帧落地 ⇒ 自动值**没有**顶掉用户意图。
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lr.press_samples),
                              static_cast<std::int64_t>(lr.release_samples));
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lr.observe_ms), 0);
            // 继续到 `wait 3 frames` 的 quit（f+3）
            LIFT_CHECK(m.presenter().present(m.surfaces(), *gs, "", &err));
            LIFT_CHECK(runChunk(m, 200000ull));
            LIFT_CHECK(m.inputScript().quitRequested());
            std::printf("click-honors-explicit-hold: press_frame=%llu release_frame=%llu samples=%llu\n",
                        (unsigned long long)lr.press_frame, (unsigned long long)lr.release_frame,
                        (unsigned long long)lr.press_samples);
        }
    }

    // =================================================================================
    // ⑨ `input-injection/click-bounded-without-sampling`：guest 完全不采样 ⇒ 到上限后**仍然抬起**
    //    + 告警 + 脚本继续（⛔ 不挂死）。
    // =================================================================================
    g_current_case = "input-injection/click-bounded-without-sampling";
    {
        wm->clearKeys();
        wm->setCursorPos(0, 0);
        drainMessages(*wm);
        resetObservation(m);
        std::vector<std::uint8_t> spinner;
        LIFT_CHECK(buildSpinnerBody(&spinner, kSampleBurn, &aerr));
        if (spinner.empty()) {
            LIFT_CHECK(false);
        } else {
            LIFT_CHECK(m.placeCode(kSpinner, spinner.data(), spinner.size(), &err));
            // 同 ⑦：虚拟时钟累计 ⇒ 触发时刻 = 现在 + 偏移。
            const std::uint64_t base_ms = m.clock().nowMs();
            const std::string script =
                "at guest_ms " + std::to_string(base_ms + 10ull) + "\n"
                "click 100 200\n"
                "at guest_ms " + std::to_string(base_ms + 2000ull) + "\n"
                "quit\n";
            LIFT_CHECK(m.inputScript().loadText(script, "inline:click-bounded-without-sampling", &err));
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(m.inputScript().maxAutoHoldMs()), 1000);  // 缺省上限
            m.cpu().eip = kSpinner;
            // 每块 2M 条 = 200 ms guest 时间；脚本到 2000 ms ⇒ 12 块足够。
            bool stopped = false;
            for (int chunk = 0; chunk < 20; ++chunk) {
                if (!runChunk(m, 2000000ull)) {
                    LIFT_CHECK(false);
                    break;
                }
                if (m.inputScript().quitRequested()) {
                    stopped = true;
                    break;
                }
            }
            LIFT_CHECK(stopped);  // ⛔ 不挂死
            const lift::InputScript::ReleaseStats& rs = m.inputScript().releaseStats();
            const lift::InputScript::ReleaseInfo& lr = m.inputScript().lastRelease();
            LIFT_CHECK(lr.valid);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rs.bounded_no_sample), 1);  // 触发上限（并告警）
            LIFT_CHECK(lr.bounded);
            LIFT_CHECK(lr.auto_hold);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lr.press_samples),
                              static_cast<std::int64_t>(lr.release_samples));  // guest 一次也没采样
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lr.observe_ms), 0);
            const std::int64_t hold = static_cast<std::int64_t>(lr.release_ms - lr.press_ms);
            LIFT_CHECK(hold >= 1000);  // 到上限才抬
            LIFT_CHECK(hold <= 1200);  // 但不是无界等待
            LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);  // 抬起真的落地
            // 脚本**继续**到 quit（动作集全部应用）⇒ 不是"挂在那条 click 上"
            LIFT_CHECK(m.inputScript().quitRequested());
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(m.inputScript().appliedCount()),
                              static_cast<std::int64_t>(m.inputScript().actionCount()));
            std::printf("click-bounded-without-sampling: press_ms=%llu release_ms=%llu observe_ms=%llu "
                        "press_samples=%llu release_samples=%llu hold_ms=%lld bounded=%d\n",
                        (unsigned long long)lr.press_ms, (unsigned long long)lr.release_ms,
                        (unsigned long long)lr.observe_ms, (unsigned long long)lr.press_samples,
                        (unsigned long long)lr.release_samples, (long long)hold, lr.bounded ? 1 : 0);
        }
    }

    // =================================================================================
    // ⑩ 解析失败必须响亮
    // =================================================================================
    g_current_case = "parse-errors";
    {
        std::string e;
        LIFT_CHECK(!m.inputScript().loadText("mouse_move 1 2\n", "bad:no-trigger", &e));
        LIFT_CHECK(!e.empty());
        LIFT_CHECK(!m.inputScript().loadText("at frame 1\nmouse_down\n", "bad:no-pos", &e));
        LIFT_CHECK(!m.inputScript().loadText("at frame 1\nmouse_move 1 2\nmouse_down 0x41\n", "bad:button", &e));
        LIFT_CHECK(!m.inputScript().loadText("at frame 1\nkey_down 0\n", "bad:vk", &e));
        LIFT_CHECK(!m.inputScript().loadText("at frame 1\nfrobnicate 3\n", "bad:op", &e));
        LIFT_CHECK(!m.inputScript().loadText("at frame 1\nkey_down 0x0D\nwait 2\n", "bad:wait", &e));
        LIFT_CHECK(!m.inputScript().loadText("# 只有注释\n", "bad:empty", &e));
        LIFT_CHECK(!m.inputScript().loadFile("Z:/definitely/not/here.txt", &e));
        // 合法脚本能解析（含 `#` 注释 / `;` 分隔 / hold）
        LIFT_CHECK(m.inputScript().loadText("at frame 1; click 3 4 hold 2 # c\nquit\n", "ok", &e));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.inputScript().actionCount()), 2u);
        LIFT_CHECK(m.inputScript().loadText("at guest_ms 5\nafter 2 ms\nmouse_move 1 1\nquit\n", "ok2", &e));
    }

    // ★ 整个用例跑完仍没有 SDL 视频 / 音频初始化（"注入靠 SDL 事件泵"的回归会在这一步红）。
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_AUDIO), 0u);

    return lift_test::finish("input-injection");
}
