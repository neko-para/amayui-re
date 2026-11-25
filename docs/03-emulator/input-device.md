# 输入设备：shim 侧的一台虚拟鼠标

> 本文写**已实施**的输入设备合同（输入重设计 S1–S9 落地之后），不是提案。
> **配方 / 投递层**（11 个投递点 = 9 读点 + `SetCursorPos` 写点 + `ReadFile` 事件源；门、完成度）
> 在 `input-plan.md`；**交互模型**在
> `interaction-model.md`；**overlay 的命中 / `--ui-input`** 在 `overlay.md`；**Win32 返回值**
> 在 `contracts-win32.md` 的 L1-MSG-05。本文只写「设备是什么、谁能写、写了之后投影什么」。
> 设计稿与复审只作这一轮的出处（`.tmp/`，不入库）；长期真源 = **代码 + CTest**。

## 0. 一句话与为什么

**guest 可见的输入状态只有一台设备**：`InputDevice` 存「guest 屏幕坐标 + 256 键电平 +
逐电平来源 + 单一租约」。SDL、配方（`--input-plan`）、控制层（`emu-ctl`）、机器内
`InteractionGraph` 都只是**驱动**，一律经 `input::*` → `WindowManager` → 设备；shim 的读点
只读设备（+ 消息队列这个投影日志），不读 SDL。

用户口径（原文）：

> 「我理解 shim 层需要一个'虚拟鼠标'，而 input-plan 和 SDL 的输入都只是控制鼠标移动，
> 而不是两遍分别组合。需要确保 shim 看到的状态是唯一的，不依赖 SDL。」

这条口径解决的**不是**「写路径分叉」（写路径早已统一到 `input::*`），而是：

1. **状态不再散落**：旧实现把输入拆成 6 组字段 + 3 套 owner
   （`key_owner_` / `button_owner_` / `cursor_script_owned_`）+ 死状态 `capture_`；
   现在只有 `lease_` + `level_owner_[256]`。
2. **谁来驱动有同一道闸门**：OS / Program 的每次写入都走同一张转移表；拒绝与「接受但不投影」
   是设备的返回值，不再散在 `input.cpp` 的 4 条 OS 特例里。
3. **消息是设备的投影**：`WM_*` 的 `wParam` / `lParam` 一律在**写后从设备回读**；
   队列是历史日志，不是第二份状态。
4. **SDL 不是前提**：guest 输入状态与读点不要求 SDL 窗口 / 事件泵存在；`--present headless`
   仍建隐藏 SDL 窗口 + GPU（缺的只是 sink 与事件泵）。准确表述是「**输入不依赖 SDL**」，
   不是「headless 没有 SDL」。

## 1. 设备模型

### 1.1 类型（`runtime/input_device.h`）

```cpp
enum class InputSource : std::uint8_t { Os = 0, Script = 1 };  // 公开施加面
enum class Driver      : std::uint8_t { Os = 0, Program = 1, Guest = 2 };
enum class LevelOwner  : std::uint8_t { None = 0, Os = 1, Program = 2 };
enum class LevelChange : std::uint8_t { None = 0, Down = 1, Up = 2 };

struct WriteResult {
    bool accepted = false;                 // false = 被租约 / 来源规则拒绝（不写、不投影）
    LevelChange change = LevelChange::None; // 只在 accepted 时有意义
};

struct LevelRef {                          // 收口时的一条待投影电平
    std::uint32_t vk = 0;
    LevelOwner owner = LevelOwner::None;
    bool mouse = false;                    // true = WM_*UP；false = WM_KEYUP
};
```

- `InputSource` 是 `WindowManager` 对外的两个来源：`Os` → `Driver::Os`，
  `Script` → `Driver::Program`（配方 / 控制层 / `InputScript` / 图**从不并发**，同属一个
  Program 驱动者）。
- `Driver::Guest` **只**由 guest 自己的 `SetCursorPos` 使用：永远接受、不改租约 / 电平、
  不发消息。
- `LevelOwner` 回答的是「**当前这份电平是谁抬起的**」，不是「这个键归谁」；它只用于
  down/up 配对与收口。`LevelOwner::None` ⇔ 电平为 0。

### 1.2 字段（只有这四组）

```cpp
class InputDevice {
public:
    // ---- 读（唯一电平状态；guest 侧所有读点最终读这几行）----
    void cursorScreen(int* x, int* y) const;      // GetCursorPos 的空间
    bool keyDown(uint32_t vk) const;              // GetAsyncKeyState / TranslateMessage
    bool buttonDown(MouseButton b) const;         // key_level_ 的只读派生
    uint32_t buttons() const;                     // MK_* 位图（无第二份）
    Driver lease() const;                         // Os 或 Program；Guest 永不出现
    bool programDriving() const;
    LevelOwner levelOwner(uint32_t vk) const;
    std::vector<LevelRef> programHeldLevels() const;  // 收口前只读快照（日志 / 测试）

private:
    friend class WindowManager;
    // ---- 写（private；只有 friend 能调）----
    bool moveToScreen(int screen_x, int screen_y, Driver d);
    WriteResult setButton(MouseButton b, bool down, Driver d);
    WriteResult setKey(uint32_t vk, bool down, Driver d);
    std::vector<LevelRef> release(Driver d);      // 清 O==Program；设备不 post
    std::vector<LevelRef> dropOsLevels();         // 清 O==Os；设备不 post
    void clearKeys();                             // 整机复位（测试 / 旧口径）

    std::int32_t screen_x_ = 0, screen_y_ = 0;    // ★ 唯一位置（guest 屏幕坐标）
    std::uint8_t key_level_[256] = {};            // ★ 唯一电平表（按钮 = 其中三个 VK）
    std::uint8_t level_owner_[256] = {};          // ★ 1 时记谁抬起的；0 时 None
    Driver lease_ = Driver::Os;                   // ★ 当前租约；只有 Os / Program
};
```

**设备里没有的东西（关键，不是遗漏）**：

- **没有时钟**：设备不判「到点」⇒ 结构上不可能变成「块边界 pump」。到期由调度层
  （`InputPlan` 的闸门 / `wait`）用宿主墙钟判。
- **没有消息队列**：消息由 `input::*` 组装并 `post`；设备只回答「现在是什么状态」。
- **没有窗口**：屏幕 ↔ 客户区的换算在 `WindowManager`（窗口表）与适配器（letterbox）。
- **没有滚轮电平**：滚轮**只是事件**（Win32 也没有滚轮电平；`GetKeyState` 报不出来），
  设备不存 wheel 累计。
- **没有 `mouse_buttons_`**：按钮位图是 `key_level_` 三个 VK 的只读投影。

### 1.3 所有权与线程

- `InputDevice` 是 `WindowManager` 的**私有成员** `device_`；不返回公开引用，也不提供
  `device()`。生命周期 = `WindowManager`。
- 写 API 全部 **private**，`friend class WindowManager`；`WindowManager` 保留
  `setKeyDown` / `setMouseButton` / `setCursorPos` / `setCursorClientPos` 的**同名**公开
  setter，是唯一写入口。`input::*` 只调这些 setter。
- 收口路径：`WindowManager::releaseProgramLevels()` / `dropOsLevelsState()` 是 **private**，
  只 friend `input::releaseProgram` / `input::dropOsLevels`；它们只返回 `vector<LevelRef>`，
  消息由这两个 free function **逐条 post**。
- ⛔ **不再有** `releaseScriptOwnership()` / `clearOsInput()` 这两个「只改状态、不投影」的
  公开入口（S4 删除）；也⛔ 不再有死状态 `WindowManager::setCapture/capture`（S1 删除）。
- `InputDevice` 自身**不做线程检查**；`WindowManager` 的转发方法保留
  `checkMainThread("window.key" / "window.mouse" / "window.cursor")` 的站点名。
  `setUiInputRouter` / `uiInputRouter` 的站点名是 `window.ui-input`（S9）。
- 默认构造、无分配 ⇒ `Machine` / `AotHost` / 纯夹具的构造顺序不变；析构即释放。

## 2. 转移表

记 `L(vk)=key_level_[vk]`、`O(vk)=level_owner_[vk]`、`lease=lease_`。
「投影」列是**调用方**（`input::*`）看到返回的 `change` 之后做的事；**设备自己不 post**。

| # | 事件 | 前置 | 设备动作 / 返回 | 投影 |
|---|---|---|---|---|
| 1 | `Guest` 的 `SetCursorPos(x,y)` | 无 | `screen=(x,y)`；不改 lease / 电平；accepted | **无**（`SetCursorPos` 不发 `WM_MOUSEMOVE`） |
| 2 | `Os.move(x,y)` | `lease==Os` | `screen=(x,y)`；accepted | `WM_MOUSEMOVE`（wParam=`buttons()`，lParam=设备 client） |
| 3 | `Os.move(x,y)` | `lease==Program` | 不写；rejected | 无 |
| 4 | `Program.move(x,y)` | 任意 | `lease=Program`；`screen=(x,y)`；accepted | `WM_MOUSEMOVE` |
| 5 | `Os.down(vk)` | `lease==Program` | 不写；rejected | 无（OS 的上升被拒） |
| 6 | `Os.down(vk)` | `lease==Os` 且 `L==0` | `L=1, O=Os`；accepted / Down | `WM_*DOWN` / `WM_KEYDOWN` |
| 7 | `Os.down(vk)` | `lease==Os` 且 `L==1` | 不写；accepted / None | 无（按钮不 repeat；键盘 repeat 见 #23） |
| 8 | `Os.up(vk)` | `L==0` | 不写；accepted / None | **无**（孤儿 up 消失） |
| 9 | `Os.up(vk)` | `L==1` 且 `O==Program` | 不写；accepted / None | 无（真人的松开不取消 Program 的 hold） |
| 10 | `Os.up(vk)` | `L==1` 且 `O==Os` | `L=0, O=None`；accepted / Up | `WM_*UP` / `WM_KEYUP` |
| 11 | `Program.down(vk)` | `lease==Os` | `lease=Program`；再按 #12/#13 | 同 #12/#13 |
| 12 | `Program.down(vk)` | `L==0` | `L=1, O=Program`；accepted / Down | `WM_*DOWN` / `WM_KEYDOWN` |
| 13 | `Program.down(vk)` | `L==1` 且 `O==Os` | `O=Program`（**接管，不改电平**）；accepted / None | **无**（不重复 down） |
| 14 | `Program.down(vk)` | `L==1` 且 `O==Program` | 不写；accepted / None | 无 |
| 15 | `Program.up(vk)` | `lease!=Program` | 不写；rejected | 无（没有租约的 up 不生效） |
| 16 | `Program.up(vk)` | `L==0` | 不写；accepted / None | 无 |
| 17 | `Program.up(vk)` | `L==1` 且 `O==Program` | `L=0, O=None`；accepted / Up | `WM_*UP` / `WM_KEYUP` |
| 18 | `Program.up(vk)` | `L==1` 且 `O==Os` | 不写；rejected | 无（Program 不取消真人按住的键） |
| 19 | `Program.release()` | `lease==Program` | 所有 `O==Program` 的 `L=0`；`lease=Os`；返回 `vector<LevelRef>` | 调用方**逐条** post up（会话收口） |
| 20 | `focusLost` / `pause` 的 `dropOsLevels()` | 无 | 所有 `O==Os` 的 `L=0`；返回 `vector<LevelRef>` | 调用方逐条 post up（失焦 / 恢复） |
| 21 | `Os.wheel(delta)` | 任意 | 不改电平 / lease；光标写成功用新位置，失败用设备当前位置；accepted | `WM_MOUSEWHEEL`（wParam=`delta<<16 \| buttons()`，lParam=设备 client） |
| 22 | `Guest.setButton/setKey` | — | **不提供**（Guest 只能写光标） | — |
| 23 | `Os.key repeat down` | `L==1` 且 `repeat==true` | 不改变电平；accepted / None | `WM_KEYDOWN`（**显式例外**；lParam bit30=1） |

**「消息只在电平转移时投影」的准确表述**：

- `setButton` / `setKey` 返回 `WriteResult{accepted, change}`；`input::*` **只在
  `change==Down/Up` 时 post**。
- `accepted==true && change==None`（#7/#8/#9/#13/#14/#16）⇒ **接受但无消息**。
  「OS up 被接受但不发消息」因此有定义：它没有造成 1→0 转移（或它不敢动 Program 的 hold）。
- **两个显式例外**（写进合同，不藏在实现里）：
  1. **键盘 auto-repeat**（#23）：`down && repeat` 即使电平不变也 post 一条 `WM_KEYDOWN`；
     `present_sdl.cpp` 传 `event.key.repeat`，`InputScript` 的 `key_down` 传 `false`。
  2. **批量收口**（#19/#20）：一次 `release(Program)` / `dropOsLevels()` 可以投影多条 up
     （会话结束 / 焦点丢失 / 暂停恢复），不是「一条 step」。
- **滚轮不是电平**（#21）：它是事件；不参与转移规则，也不会因为光标写失败被丢。
  ⚠ #21 **不是** `InputDevice` 的写方法：设备没有 `wheel_`；`input::mouseWheel` 只做光标的
  best-effort 写 + `WM_MOUSEWHEEL` 投影。⛔ 不要给设备加滚轮累计。

## 3. 租约与来源

1. **一个租约**：`lease_` 只能是 `Os` 或 `Program`；`Guest` 是写者，永远不取得租约。
   `Program` = 配方 / 控制层 / `InteractionGraph`→`InputScript`（从不并发；它们之间的先后是
   调度层的顺序，不是所有权）。
2. **Program 取得租约的时机**：**第一次有效上升或移动**（`Program.move` / `Program.down`）。
   ⛔ 不是「装载配方时」（空计划必须零副作用），⛔ 不是 `Program.up`。
   ★ S4 follow-up：`input::mouseButton(..., Script, down=false)` **不先移动光标** —— 否则
   `moveToScreen(Driver::Program)` 会无条件取得租约，导致 #15（没有租约的 Program up 应拒绝）
   对鼠标按钮**不可达**，#18 也会在拒绝之前把租约 / 光标改掉。up 用**设备当前位置**投影。
3. **OS 的上升**在 `lease==Program` 期间一律拒绝（#3 / #5）。**OS 的下降**：
   - `L==1 && O==Os` ⇒ 接受、1→0、post up（#10）；
   - `L==1 && O==Program` ⇒ 接受但**不写、不发**（#9；真人的松开不取消 Program 的 hold）；
   - `L==0` ⇒ 接受但**不发**（#8；孤儿 up 消失）。
4. **Program 的上升**：`L==1 && O==Os` 时**接管**（`O=Program`，电平不变，不重复 down，#13）；
   `L==0` 时正常 down（#12）；`lease==Os` 时先取得租约（#11）。
   ⇒ 一次「真人按住 → 注入 click」在 guest 眼里是「down（真人）… up（注入收口）」，
   **不会多一条 down**（`input-injection/program-down-takeover`）。
5. **Program 的下降**：只对 `O==Program` 生效（#17）；`O==Os` 拒绝（#18）；`L==0` 无操作（#16）。
6. **`release(Program)` 的语义**：设备侧清所有 `O==Program` 的 `L`、`lease=Os`、返回
   `vector<LevelRef>`；**设备不 post**。调用方 `input::releaseProgram(wm, time_ms)` 拿到 events，
   **逐条** post 对应的 `WM_*UP` / `WM_KEYUP`（wParam / lParam 从设备回读）。
   - `InputPlan::closeSessionIfDone`：在 `atPoint` **开头**（早退之前）调用 ⇒ 消息在本读点入队，
     guest 本读点的 `PeekMessage` 就能取到；电平已经是 0。
   - `InputPlan::releaseAll`：一次性收口（run 结束 / CTest teardown），没有后续读点；消息仍入队，
     保持「消息 = 投影」。
   - `InputScript::releaseOwnedInputs`：在 `tick` 的收口块 / `abandon` 调用；消息入队，下一个
     guest 读点可见。
7. **焦点丢失 / 暂停恢复**：`dropOsLevels()` 清 `O==Os` 的电平，返回 events；
   `input::dropOsLevels` **逐条 post up**（Q1b）。⇒ 「down/up 永远配对」在 OS 释放 + 失焦 +
   暂停三处都成立；Program 的 hold 不被失焦 / 暂停取消。
   - 失焦：`present_sdl.cpp` 的 `SDL_EVENT_WINDOW_FOCUS_LOST`。
   - 暂停恢复：`aot_host.cpp` 的 `controlOnResume` 在设置陈旧截点**之前**调用；否则暂停期间
     真人松开的按钮会永久按下（`pumpOneEvent` 丢弃早于恢复时刻的 OS 事件）。
8. **交还时不从 OS 重新同步电平（显式 residual）**：`release(Program)` 只清 `O==Program` 的
   电平；如果真人此刻正物理按着**同一个**键（而它是 Program 抬起的），设备上它是「抬起」，
   直到下一条 OS 事件。`O==Os` 的键不受影响。**为什么不做 resync**：`SDL_GetMouseState` /
   `SDL_GetKeyboardState` 是 **SDL 依赖**，与「输入不依赖 SDL」冲突；要做就做成适配器的
   **可选**钩子，headless 没有钩子 ⇒ 不 resync。⚠ 这条必须留着，否则下一个人会把它当 bug
   修成 SDL 依赖。
9. **`--cursor-warp` 的回灌抑制保留在适配器**：SDL 只是驱动者，「这次宿主光标移动是我自己
   造成的」是**适配器的知识**（需要 clamp 之前的窗口点）；设备拿不到也不该拿到。判据仍是
   L1-MSG-05 那一组 `cursor-warp/*` + `warp-echo/*`。
10. **`SetCursorPos` 是 `Driver::Guest`**：永远接受、不改 lease / 电平、不发消息（#1）。
    `shimSetCursorPos` 先 `deliverWrite(SetCursorPos)`（配方可在写点推进），再
    `wm.setCursorPos(x,y)`（内部 `device_.moveToScreen(x,y,Driver::Guest)`）。它不会被租约误伤。

## 4. 驱动者与坐标链

### 4.1 四个驱动者

| 驱动者 | 进入点 | 来源 | 备注 |
|---|---|---|---|
| SDL（OS） | `present_sdl.cpp:Impl::handleEvent` | `InputSource::Os` | 键 / move / 按钮 / 滚轮 4 个事件写；失焦是会话写 |
| 配方（AOT） | `InputPlan::atPoint`（guest 读点） | `InputSource::Script` | `click` 三段 + `mouse_move/down/up` |
| 控制层 | `InputPlan` 的立即队列（暂停态入队） | 同上（`Script`） | 走**同一段** `armClickAt` / `armMoveAt`；计入 `fired` |
| 机器内 `InteractionGraph` | `InputScript::appendText`（块边界） | 同上（`Script`） | **AOT 不 tick 它**；仍是第 4 个 Program 生产者样本 |

**写入点两张账**：`input::*` 的事件落点 **17** 个 + **会话写 5** 个。计数口径：

```bash
rg -n -U 'input::(mouseMove|mouseButton|mouseWheel|key)\(' apps/emulator-lift/src
```

（排除注释与 `input.h/.cpp` 自身的转发 / 定义。）事件落点 = SDL 4 + `input_plan.cpp` 4 +
`InputScript` 9（`apply` / `emitRelease`）；会话写 = 失焦（`present_sdl.cpp`）· 恢复（`aot_host.cpp`）·
配方收口（`closeSessionIfDone` / `releaseAll`）· `InputScript` 收口（`releaseOwnedInputs`，
`tick` / `abandon` 共用）。⚠ 设计稿记的「18」是 S4 之前的读数；`releaseOwnedInputs` 原有两个
`input::*` 已被 `input::releaseProgram` 取代 ⇒ 按当前代码是 17。

### 4.2 坐标链（每个空间只做自己那一跳）

| 投影 | 谁做 | 唯一实现 |
|---|---|---|
| guest screen ↔ guest client | `WindowManager`（窗口原点） | `host_window.cpp` 的 `setCursorClientPos` / `cursorClientPos` |
| 1280×720 参考 → guest client | 调度层（配方 / 控制层） | `cursor_map.cpp` 的 `referenceToClientPixel` |
| guest client → **窗口点**（letterbox 正算） | 适配器 | `cursor_map.cpp` 的 `imageToWindowPixel`（warp / overlay 用） |
| **窗口点** → guest client（反算 + clamp） | 适配器 | `cursor_map.cpp` 的 `mapWindowPixelToImage`（`mapMouse` 用） |
| 窗口点 → **窗口像素**（DPR） | 适配器 | `present_sdl.cpp` 的 `pointerWindowPixels` / `cursor_map.cpp` 的 `overlayPointerWindowPixels` |

- 设备只存 **guest 屏幕坐标**（`GetCursorPos` / `SetCursorPos` 的空间）；`WM_*` 的 `lParam`
  是**客户区**坐标，每次现算。**为什么不是只存客户区**：窗口一移动，光标停在屏幕原处，
  客户区坐标应跟着变；存屏幕、客户区现算 ⇒ 窗口移动自动正确。
- 配方 / 控制层的 `(x,y)` 是 **1280×720 参考系**（人手工量出来的数）；投递前过
  `referenceToClientPixel`（**唯一公式**；目标尺寸 = 主窗口 `HostWindow::client_w/h`，
  0 ⇒ 退回 1280×720）。`click` 的 move / down / up 共用**同一个**映射结果。
- overlay 的命中表是**窗口像素**（DPR 之后）；指针位置来自设备：`device client` →
  `imageToWindowPixel` → 窗口点 → `pointerWindowPixels` 的 DPR 换算（overlay 路径整条封装在
  `overlayPointerWindowPixels`，与 `pointerWindowPixels` 同一口径）→ 窗口像素 → `hoverKey`。
  ⇒ 同一帧只有一个鼠标位置（`readings.mouse` 与 `readings.hover` 同源），
  `--present window --input-plan` 的 HUD hover 会跟随注入。
- ★ **`pointerWindowPixels` 是换算函数，不是空间**：`warp_echo` 与 `imageToWindowPixel` 在
  **窗口点**（逻辑坐标），overlay 命中表在**窗口像素**（DPR 后）。两者不能互相替换。
- overlay 的命中矩形仍是 `ui` 层的合同（窗口像素），**不动**。

## 5. 推进时机

- **设备自己永远不推进**：无时钟、无队列、无回调。
- **调度层只在 guest 读点推进**：`atPoint` 的 `flush-before-read`；顺序 =
  到期的抬起（`releaseDue`）→ 到期的按下（`pressDue`）→ 立即队列（`drainImmediate`）→
  配方当步（flush 循环）。门 / `wait` / `profile_*` / `quit` 也在这里消费，但它们不是写动作。
- **一个读点最多一条写动作**（S7）：`writes_this_point_` 在每个 `atPoint` 开头清零；
  `releaseDue` / `pressDue` / `drainImmediate` / flush 四段都在写过一条之后停。
  - 停下时**记完账**：`++applied_` / `++next_` / `guard_armed_=false` /
    `traceStepConsumed` 在停之前完成（⛔ 在 `applyClick` 后直接 return）。
  - **会话收口是例外**：`release(Program)` / `dropOsLevels()` 一次可以投影多条 up。
- **`click` 是三段复合**：`armClickAt`（只把光标放到目标上）→ `pressDue`（按下，闸门③）→
  `releaseDue`（抬起，闸门①/②）。`armClickAt` **不再**「同按钮先抬起」：
  - `atPoint` 遇 `click` / `mouse_down` 且该按钮仍在 `pending_` ⇒ **不消费**（留 `next_`）；
  - `drainImmediate` 遇同样的 click 停在队首；
  - `pressDue` 一次最多按一条；`buttonPending` 跳过已按下未抬起的按钮。
  - 代价（显式）：连点总时长变长；每条 click 的 down 都能等到闸门①的 2 个 KeyLevel 读点。
  - ★ **闸门②'（回调边界，2026-11-22 加）**：`releaseDue` 原先只看**读点个数** —— 2 个 KeyLevel
    读点可以在**同一个 guest 回调**里凑满，抬起就落在施加按下的那个回调**内部**；而引擎是**按回调**
    比电平（`3f1`/`3f2` 这种"这一趟读到的电平"），那一趟根本没看到按下 ⇒ 边沿被丢（实测单条
    `click … right` 4/5；失败臂 `reason=keylevel`、press→release 只跨 6 个读点）。现在：**驱动供给
    帧线时**（`PointerPoint::callback_boundary`，唯一来源 = `InputPlan` 的 `presenting()`）还要求
    `frames >= press_frames + 2`，它与 ①/② **并联**（任一成立之后仍要过这一条）。为什么是 **2**：
    帧计数在回调**末尾**（Present）才 +1 ⇒ `+1` 只证明"按下那一帧走完了"，证明不了"**下一个**回调的
    按键读发生在抬起之前"。★ **两条驱动都供给帧线了**（唯一来源都是同一个 Present 帧计数）：
    `InputPlan` 用 `presenting()`，插件 `IntentExecutor` 用 `setFrameSource(presenter.frameCount())`
    （`aot_main` 装配；缺省 nullptr ⇒ 纯夹具行为不变）。守卫：`inject/click-crosses-callback`
    （plan 路径）· `inject/executor-click-crosses-callback`（插件路径）+ 负对照
    `inject/executor-click-without-frame-line`（不注帧线 ⇒ 抬起不跨边界，正判据必然红）；
    `tools/mutate-check.mjs` 有对应条目。
- **`mouse_move` / `mouse_down` / `mouse_up`**：写动作，一条一次直写。
  - `mouse_move` → `armMoveAt`（与 click 第一段同一段代码）；
  - `mouse_down` → 轻闸门：上一条 `mouse_move` 之后**完成过**一次 CursorPos 读点，或
    `kPressFallbackMs` 墙钟兜底；不成立就不消费（留 `next_`）；
  - `mouse_up` → 直写；坐标 = **设备当前位置**（⛔ 不带 x/y，拖拽 up 不回跳）。
- **⛔ 不许为了「移动更连续」加块边界 pump**：连续移动的粒度就是 guest 读点密度（显式代价）。
- **暂停**：没有读点 ⇒ `atPoint` 不跑，`pending_` / `armed_` / `immediate_` 停住；
  `InputPlan::nowMs()` 扣掉 `VirtualClock::pausedTotalMs()` ⇒ 墙钟兜底不会在暂停期间走掉。
  租约与 Program 抬起的电平**不因暂停改变**。

## 6. S9：注入点击的叠层路由（`--ui-input`）

### 6.1 值域与安装

- `--ui-input=consume|pass`（也收分离形式 `--ui-input consume|pass`）；缺省 **consume**；
  非法值 `exit 2` 并点名合法值。值域唯一一份在 `runtime/ui_input.h` 的 `parseUiInputMode`。
- 它是**窗口参数**：`--present headless` 与 `--overlay` 走同一条 `present_window_flag` 拒绝。
  没有 `--overlay`、也没有插件的 `render` 时它是无害 no-op（没有 router，两种模式都不产生行为）。
  ★ S3 起 `--ui` 已删除（⇒ 未知参数 `exit 2`）；叠层 / 插件的唯一装载入口是 `--plugin <id|path>`
  （`--overlay` 是它的窗口预设）。
- `UiInputRouter`（`runtime/ui_input.h`，SDL-free）只有一条：
  `virtual bool routeInjectedPointer(UiPointerPhase, MouseButton, int client_x, int client_y)`。
  返回 true = 叠层吞掉（调用方**不得**调用 `input::*`）；`client_x/y` = **guest 客户区**。
- 实现 = `SdlWindow::Impl`（`present_sdl.cpp`），内部仍是 `ui/hit.h` 的同一个
  `ClickRouter::handlePointer`；坐标链 = `overlayPointerWindowPixels` → 窗口像素 → 命中表。
- 安装条件只有一条：`overlay && ui_script != nullptr`（`syncUiInputRouter` 是唯一同步点）。
  `Pass` 模式**也装** —— 它的语义在 `routeInjectedPointer` 里（不碰 `click_router`），
  这样「装不装」与「吞不吞」是两件事。`InputPlan` 不另存指针，施加时问
  `wm.uiInputRouter()`（非拥有指针，站点名 `window.ui-input`）。

### 6.2 `click` 的三段怎么问 router

1. **臂上**（`armClickAt`）：在任何 `input::*` **之前**问 `Down`。命中 ⇒
   `ui_consumed_++`，**不调 `input::mouseMove`**（光标不动、guest 一条 `WM_MOUSEMOVE` 都收不到），
   但仍压 `ArmedPress{swallowed=true}`（三段时序不变，下一读点 `pressDue` 仍受闸门③约束）。
2. **按下**（`pressDue`）：只建 `PendingRelease{swallowed=true}`，**不调**
   `input::mouseButton(down)`；up 的时机仍走闸门①/②（与不吞时同一个形状）。
3. **抬起**（`emitRelease`）：**不管 down 吞没吞都问 `Up`**，但 up 的返回值**不改变决定**：
   - down 吞 ⇒ up **必须**吞（`ClickRouter` 的 capture / pending click 靠这条收口；guest
     ⛔ 不许收到没有 down 的 up）；
   - down 放 ⇒ up **必须**放（overlay 中途出现 / 消失都不改变这次点击的归属）。
   吞掉 ⇒ 不调 `input::mouseButton(up)`；`fired` 照旧 +1，`released` 照旧 +1。

`Pass`：`routeInjectedPointer` 直接 `return false`（不碰 `click_router`）⇒ click 的
move / down / up 与没有 UI 时**逐位相同**；右 / 中键同样 `return false`。

**摘要**：`InputPlan::summary()` 只在 `uiConsumed() != 0` 时追加 ` ui_consumed=N`
（Down 那一瞬 +1；一条 click 的 down/up 是同一个决定，⛔ 不数两次）。默认档（没有叠层 /
没有命中）这一行**逐字不变**。

### 6.3 residual（明确未做）

- `mouse_move` / `mouse_down` / `mouse_up` **原语不过 router**：它们是「手」而不是「一次点击」；
  接上去会改变拖拽 / 显式 hold 的语义。
- 控制层回执里的 `consumed_by_ui` **未做**：当前只有 `InputPlan` 的摘要字段 `ui_consumed=N`。
- 滚轮 / motion / 右中键永远放行（D6）；capture 只服务 OS 与注入的 **click**。

## 7. 守卫（真实 CTest 名 + 红了意味着什么）

| CTest / 子用例 | 红了意味着什么 |
|---|---|
| `input-injection`：`orphan-up` · `program-hold-os-release` · `release-program-posts-up` · `focus-loss-posts-up` · `wheel-while-program-drives` · `program-down-takeover` · `program-up-without-lease` · `program-up-human-hold` · `key-repeat-bit30` · `guest-setcursorpos-while-program` | 转移表 / 租约 / 收口投影被改坏：孤儿 up 回来了、OS up 取消了 Program 的 hold、收口不发 up、失焦不清 OS 电平、滚轮被整条丢、接管重复 down、Program up 无租约也生效、repeat 不投影、guest 的 `SetCursorPos` 被租约误伤 |
| `input-plan-read-points`：`input-plan/mouse-primitives` · `drag` · `drag-repeat` · `one-write-per-read-point` · `control-queue-one-write` · `two-clicks-serialized` · `click-consumed-by-ui` · `click-router-miss-identical` · `gates-and-releases` · `completion` · `trace` · `wallclock-release` | 配方解析 / 施加 / 写预算 / 连点串行化 / S9 路由被改坏。例如 `two-clicks-serialized` 红 = 两条 click 又合并成 `DOWN,DOWN,UP,UP`；`click-router-miss-identical` 红 = 未命中却改了 guest 消息序列；`trace` 红 = 抬起那一行的 `reason=keylevel` / `reason=wallclock` / `reason=keylevel+wallclock` 不再如实区分两条腿 |
| `input-inject-visible`：`inject/click-two-phase` · `inject/drag` · `inject/move-only` · `inject/handback` · `inject/wallclock` · `inject/click-crosses-callback` | 端到端消息序列 / lParam / MK 位被改坏：down 与 up 落在不同坐标、拖拽 up 回跳、move 变成点击、收口不交还、墙钟兜底不抬起；`click-crosses-callback` 红 = 抬起又退回"只按读点个数判"（2 个 KeyLevel 读点可以在同一个回调里凑满 ⇒ 边沿在慢屏/20 Hz 屏上被丢） |
| `overlay`：`overlay/pointer-map-letterbox` · `overlay/pointer-map-dpr` · `overlay/pointer-map-hover` · `overlay/pointer-map-invalid-geometry` · `overlay/ui-input-parse` | overlay 指针映射链（客户区 → 窗口点 → 窗口像素）或 `--ui-input` 值域被改坏；`ui-input-parse` 红 = 非法值静默成 consume / 缺省不是 consume |
| `lift-control-endpoint`：`click-enqueue-at-read-point` · `click-coordinate-map` · `clock-freeze+plan-guard` · `control-resume-arms-host-hook` | 控制层立即队列的落点 / 坐标映射 / 暂停补偿被改坏（控制层 click 绕过 guest 读点、映射分叉、暂停期间墙钟走掉、恢复不清 OS 电平） |
| `ui-click-router`：`ui-click-router/capture` | 纯 `ClickRouter` 状态机（down 吞 ⇒ up 吞 / down 放 ⇒ up 放）被改坏；它证明的是叠层状态机，不是注入路径 |
| `cursor-warp`：`cursor-warp/*` · `warp-echo/*` · `cursor-map/*` | `--cursor-warp` 的返回值 / 回灌抑制 / 空间换算被改坏（吞错回灌、warp 失败仍 TRUE、窗口点与窗口像素混用） |
| `cli-aot-flags`：`ui-input-values`（⚠ 非 Windows SKIP） | `--ui-input` 的帮助 / 非法值 / 窗口参数拒绝被改坏；⛔ 不作 S9 的唯一判据（pure 值域在 `overlay/ui-input-parse`） |

★ **一条要记住的分级**：如果一条守卫的「红」只是因为编译不过（断言用到新增 API），它并没有
证明运行期行为 ⇒ 必须在报告里标成「证明力弱」，⛔ 不许与运行时红并列。

★ **还没被守卫钉住的一条**：`window.key` / `window.mouse` / `window.cursor` 这三个 off-thread
站点名没有断言（`wm_token` 只钉 `window.find` / `window.post`）⇒ 状态搬家时站点名可静默改。
这是诊断面，不进「用户可见」的三问，所以它留在本文而不是 test。

## 8. 怎么查

```bash
# 1) 设备 / 注入 / overlay / 控制层 / warp 的合同面
cd apps/emulator-lift
ctest -R 'input-injection|input-plan-read-points|input-inject-visible|overlay|ui-click-router|lift-control-endpoint|cursor-warp' --output-on-failure

# 2) 设备状态只剩一份：实现文件里 0 命中（头注释可能提到旧名，不算状态）
rg -n 'cursor_x_|key_down_|mouse_buttons_|key_owner_|button_owner_|cursor_script_owned_' \
  apps/emulator-lift/src/runtime/host_window.cpp

# 3) 旧收口入口 / 死 capture 已删：0 个**调用点**（注释里提到旧名不算）
rg -n '\.releaseScriptOwnership\(|->releaseScriptOwnership\(|\.clearOsInput\(|->clearOsInput\(' \
  apps/emulator-lift/src apps/emulator-lift/tests
rg -n '\.setCapture\(|->setCapture\(|\.capture\(|->capture\(' \
  apps/emulator-lift/src apps/emulator-lift/tests

# 4) 写入点两张账（口径见 §4.1）：看实际落点，不要手抄过期计数
rg -n -U 'input::(mouseMove|mouseButton|mouseWheel|key)\(' apps/emulator-lift/src

# 5) S9：两端都问 router；摘要只在非 0 时追加
rg -n 'routeInjectedPointer|uiConsumed|ui_consumed' apps/emulator-lift/src/runtime/input_plan.cpp
rg -n 'ui-input' apps/emulator-lift/src apps/emulator-lift/tests

# 6) 坐标公式只有一处：`input_plan.cpp` 里的 1280/720 只应出现在注释，除法应在 cursor_map.cpp
rg -n '1280|720' apps/emulator-lift/src/runtime/input_plan.cpp
rg -n 'referenceToClientPixel' apps/emulator-lift/src/runtime/cursor_map.cpp

# 7) 变异自检（只跑这一条，别开全量）
node tools/mutate-check.mjs --list --match 'input'
```

**读数口径**：`ctest` 只证明「合同面没被改坏」；性能 / 墙钟读数不进本文（见 `measurement.md`）。
设计稿与复审在 `.tmp/`，**不入库、不作长期真源**；要判「现在到底是什么行为」，读代码与
CTest，不要读设计稿的「待裁决 / 推荐」。

## 9. 不做（显式非目标）

1. **多窗口 per-window 光标**：本作单窗口，设备只有一个光标；`postToPrimary` 恒发主窗口。
2. **IME**：既有裁决在 `shims/shell32.cpp` 的 `ImmAssociateContext`（`hIMC!=0` 硬 trap）；不改。
3. **Win32 capture**：`GetCapture` / `SetCapture` / `ReleaseCapture` 未实现；`WindowManager`
   的 `capture_` 死状态已删。`ui::ClickRouter` 的 capture 是**叠层捕获**，与 Win32 无关。
4. **`GetKeyState` / `GetKeyboardState` / `SetKeyboardState` 族**：未实现，导入即
   `trapUnimplemented`（响亮失败）；另开单，不并入设备。
5. **`mouse_down` / `mouse_up` 的 HUD 路由**：S9 只接 `click`（residual，见 §6.3）。
6. **`MSG.time` 统一**：不统一。`WM_*` 的 `time` 仍是三来源（SDL `SDL_GetTicks` / 配方恒 0 /
   `InputScript` 的 guest_ms）；「消息 = 设备投影」只承诺 `wParam/lParam`，⛔ 不把 `time` 包进去。
7. **滚轮变成电平 / 滚轮累计 / 惯性**：滚轮只是事件（#21）。
8. **Program→OS 交还时的电平 resync**：需要 SDL 快照，与「输入不依赖 SDL」冲突（§3.8）。
9. **无 SDL 的 headless**：`--present headless` 仍要 SDL 视频 + GPU + 隐藏窗口；GPU 是唯一
   光栅器，缺它 `exit 2`。
10. **把推进塞回执行循环**：没时钟 / 没 pump；`machine.cpp` / `aot_run.cpp` 的禁用词继续
    0 命中（守卫 `tools/test/input-plan-shape.test.mjs`）。
11. **控制层运行态 `move` / `click`**：`control_ws.cpp` 仍要求暂停态；这是协议裁决，另开单。
12. **`consumed_by_ui` 控制层字段**：未做（S9 residual）。
