# 🐞S2 shim user32：PeekMessageA 的 hwnd/min/max 过滤会静默丢掉不匹配的消息；TranslateMessage 永不产生 WM_CHAR

- id: REQ-01M7ATQ4XWSKPW1CKXAHQKNSA2
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/user32_window.cpp#peek-filter-keeps-messages
- repro: apps/emulator-lift/src/shims/user32_window.cpp#void shimPeekMessageA(X86& cpu, ShimCall& call)
- severity: S2
- done_reason: 两条都落地：① PeekMessageA 的 hwnd/min/max 过滤改成'从队首起找第一条匹配'（新原语 WindowManager::peekMatching(pred,out,remove)，不匹配的原地保留、FIFO 不变、PM_NOREMOVE 不取走、WM_QUIT 也参与 pred），删掉旧的两处'过滤即 peek(true)'；② TranslateMessage 真投 WM_CHAR/WM_SYSCHAR（字符表 + Shift/CapsLock 只读既有 keyDown 影子表、lParam 沿用按键消息的重复计数/scancode、未覆盖键不投并写明理由与文档 URL）。守卫 4 条合同（keeps-messages / range / noremove-keeps / translate-keydown-char），修前红 306 checks 24 failures（第二步 Peek(B) 拿到 0、字符 Peek 全 0）；变异把 peekMatching 改成不匹配也 pop_front ⇒ 当场红（父级复跑 1/1 红）。合同表新增 L1-MSG-06 并把 L1-MSG-01 的'URL 待补'与过期行号修正。已知未建模：PeekMessageA(hWnd=-1) 的线程消息特例（此前就没有，建议单开）；数字+Shift 与 VK_OEM_102/ABNT/假名/IME 不投（不臆造布局）。
- tags: [emulator-lift, shim, contract, user32]

## 现象（2026-11-13，分析时读代码发现；父级已逐行复核）

**① `PeekMessageA` 的过滤会把不匹配的消息从队列里丢掉**（`src/shims/user32_window.cpp:558-568`）：

```cpp
if (hwnd_filter && m.hwnd != hwnd_filter) {
    // 简化：本作只有主窗口；过滤不匹配时把它留在队列里会死循环 ⇒ 直接取走（P0 只支持单窗口）
    cpu.windows->peek(&m, true);   // ← remove=true：真取走
    continue;
}
if (min_filter || max_filter) { ... cpu.windows->peek(&m, true); continue; }
```

**微软文档**：`PeekMessage` 只**查看**与 `hWnd` / `wMsgFilterMin` / `wMsgFilterMax` 匹配的消息，
**不匹配的必须留在队列里**。现在的实现把它们**删除**了（注释自己写着"直接取走"）。

**② `TranslateMessage` 是空实现**（`src/shims/user32_window.cpp:590-594`）：

```cpp
call.result = 1;  // P0 不生成 WM_CHAR（P3 接键盘后补）
```

全仓 `rg 'WM_CHAR|0x0102'` 在 `src/shims|src/runtime` 下**只有这一处注释** ⇒ 任何走
`TranslateMessage → WM_CHAR` 的 guest 代码**永远收不到字符消息**。文档语义：`TranslateMessage`
在收到 `WM_KEYDOWN` 时把它翻译成 `WM_CHAR`/`WM_SYSCHAR` 投回队列。

## 为什么这是缺陷而不是"P0 简化"

- ① 是**静默数据丢失**：一条消息被取走 = guest 永远看不到它。当前只有一个主窗口所以看不出来，
  但它同时把"单窗口"这个**实现限制**编码进了 API 语义（换一个会自建子窗口/工具窗口的程序就错）。
- ② 字符输入是**用户可见**的：任何靠 `WM_CHAR` 取文本的路径（引擎里大概率有名字输入 / 配置文件编辑）
  会收不到字，而进程不崩、不报错。

## 判据（修好后）

- `PeekMessageA(hwnd=A)` 在队列里有 `B` 的消息时：**返回 0（无匹配）**，且 `B` 的消息**仍在队列里**
  （下次 `PeekMessageA(hwnd=B)` 能取到）；`min/max` 过滤同理；
- `TranslateMessage` 收到 `WM_KEYDOWN`（可打印键）后，队列里出现 `WM_CHAR`（`wParam` = 该字符）；
- 守卫：加在 `user32_window`（合同档），⛔ 不用真游戏；红 = 消息被吞 / 没有 `WM_CHAR`。

## 证伪条件

若引擎**从不**调用 `TranslateMessage` 或从不读 `WM_CHAR`（给出 `disasm` 的调用点 EA），
则 ② 降级为"已知取舍"（但 ① 仍然成立）。

## 归口

两条都属于"我们实现了、但**未按文档**实现且**静默**"的一类 —— 正是新测试模型（按 shim 接口设计测试）
应该抓到的东西；旧测试套（跑业务流）结构上抓不到它。
