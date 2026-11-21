# 🐞S2 合成 click 丢边沿：抬起只看 KeyLevel 读点个数、不看属于哪个 guest 回调（实测 4/5）

- id: REQ-01M7ZFF4FQ4V9JDMY4K72PXYP6
- type: bug
- status: done
- parent: REQ-01M7JHSDWT13XEZJHEET2BDH1C
- verify: apps/emulator-lift/tests/input_inject_visible.cpp#inject/click-crosses-callback
- repro: apps/emulator-lift/tests/input_inject_visible.cpp#inject/click-crosses-callback
- severity: S2
- done_reason: 判据 = CTest input-inject-visible 的用例 inject/click-crosses-callback（同一个回调里把墙钟推过兜底 + 灌 12 个位置读点都不许抬起；下一个完整回调必须仍读到按下；两个完整回调之后才抬）+ tools/mutate-check.mjs 对应条（callback_ok 恒 true ⇒ 12 failures、CTest 退出码 8）；读数 4/5 → 14/14、每条 frames = press_frames + 2。「未做」三条见正文（插件 IntentExecutor 的注入 click 无帧线 / 键盘 key_down/up 无回调边界 / kPressFallbackMs=250 墙钟兜底仍可绕过结构条件）。
- tags: [emulator-lift, input, click, recipe]

## 一句话

配方里的单发 `click … right` 会**随机**被 guest 丢掉边沿（实测 4/5）：抬起判据只看 KeyLevel **读点个数**，不看读点属于哪个 guest 回调 ⇒ 两个读点在同一回调里凑满就释放，而回调体的电平状态机那一趟根本没看到按下。

## 现象与根因（读数）

- 失败臂 `release` 的 `reason=keylevel`：按下→抬起只跨 **6** 个读点（含 2 个 KeyLevel 读点）—— 闸门①明明满足，但那 6 个读点全在**同一个** guest 回调里；回调体比 `3f1` / `3f2` 的电平，这一趟没看到按下。
- 根因：`repo:apps/emulator-lift/src/runtime/pointer_core.cpp` 的 `PointerCore::releaseDue` = KeyLevel 读点个数 + 500 ms 墙钟兜底，对"读点属于哪个回调"**零约束**。

## 修法（提交 `4f8aa5c` / 2026-11-22）

- `PointerPoint` 新增 `frames` / `callback_boundary`；帧线用**已有的** `PlanEventKind::Frame`（`present.cpp` 在 `++frame_count_` 同一刻 `note(Frame)`），`aot_main.cpp` 装配 `requireCallbackBoundary()`。
- `releaseDue` 加**闸门②'**：`frames >= press_frames + kCallbackFramesBeforeRelease`（= 2；Present 在回调**末尾**才 +1 ⇒ `+2` 才保证"按下之后一整帧"的按键读发生在抬起之前）。它与① / ② **并联**：① / ② 任一成立之后仍要过这一条。
- 缺省 `callback_boundary=false` ⇒ 纯夹具行为逐位不变；trace 的 `release` 行**追加** `frames=` / `press_frames=`。

## 判据（守卫 + 读数）

- CTest `input-inject-visible`（默认档）的用例 `inject/click-crosses-callback`：假 guest 每个回调只读一次电平；按下那一帧里把墙钟推过兜底 + 灌 12 个位置读点**不许**抬起，下一个完整回调必须**仍读到按下**，两个完整回调之后才抬。
- `repo:tools/mutate-check.mjs` 有对应条目（`callback_ok` 恒 true ⇒ 本用例 12 failures、CTest 退出码 8；`--match click-crosses-callback`）。
- 读数（win32 / 2026-11-22）：配方 `click 640 360 right` 进 FELLOW —— 修前 **4/5**（失败臂 exit 5 卡在 `await position FELLOW`）；修后 3/3 + f1–f10 10/10 + 变异重建后 g4 ⇒ **14/14**，每条 `frames = press_frames + 2`，trace 落到 `script=FELLOW.BIN pos=00001684` 且 `pending=0`；`mouse_down/up` 对照配方仍 exit 0。门禁：`ctest` 137/137、`pnpm test` 614、`pnpm verify` 500 pass / 2 skip。

## 未做（三条，都还在）

- 插件 `IntentExecutor` 的注入 click **没有帧线**（`frames=0, callback_boundary=false`）⇒ 仍是旧行为。
- 键盘 `key_down` / `key_up`（`repo:apps/emulator-lift/src/runtime/input_script.cpp`）没有回调边界。
- 按下侧的 `kPressFallbackMs=250` 墙钟兜底仍可绕过结构条件（本次未动）。

## 复现（能照抄）

```bash
cd apps/emulator-lift
ctest --test-dir out -R '^input-inject-visible$' --output-on-failure
node ../../tools/mutate-check.mjs --match click-crosses-callback   # 期望：该条当场红
```
