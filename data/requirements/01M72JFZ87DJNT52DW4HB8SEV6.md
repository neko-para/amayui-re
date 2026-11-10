# emulator-lift · 关窗二次确认：把 AGERC 的 _ShowDialog@12 桥接成自建模态（设计 + 分阶段）

- id: REQ-01M72JFZ87DJNT52DW4HB8SEV6
- type: spike
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, agerc, dialog, modal]

## 一句话
真机上**点击窗口关闭（X）**会走 AGERC.DLL 的一个**二次确认对话框**（「終了」：退出程序 / 返回标题 / 取消）。本线现在把这 3 个导出做成 no-op（`_ShowDialog@12` 恒返回 1 = "退出程序"）。本单的产出是**设计结论**：能不能桥接成我们自己的普通模态对话框、要什么能力、最小第一步是什么。

## 已核（全部 EA 锚定；详见 `repo:.tmp/agerc-dialog/design.md`）
1. 链：`WM_CLOSE(0)`（X / ESC）→ `sub_4B92F0` case16（`0x4B95FA cmp ebx,1` / `0x4B95FD jz` / `0x4B95FF call sub_4057B0`）→ `sub_4057B0` 先减光（`0x4058A9 push 80h` → `0x4058BC call sub_4A4C70`，layer `0x1F`、alpha 128）→ `0x40597B call ds:dword_55E1B4` = **`_ShowDialog@12`**（`dword_55E1B4 = [RCDLL+0x0C]`，WinMain 在 `0x4BA947` 落盘；`sub_453870` = `mov eax,[ecx+0Ch]`）。
2. 调用参数 `(id=3, hWndOwner, 0)`：**id 是选择子**，不是模板句柄（AGERC 内部再 switch 到 `DialogBoxParamA(hInstance, 3, hWndParent, proc=0x1003269F, 0)` @`0x10038420`）。
3. 对话框 = AGERC `RT_DIALOG id=3`（DIALOGEX，lang 1041）：标题「終了」、187×93 DLU、`ＭＳ Ｐゴシック` 9pt、6 控件（Static 图标 1028、提示文 Static -1、Radio 1002「プログラムを終了する(&E)」默认选中、Radio 1003「タイトルに戻る(&T)」、默认按钮 IDOK、IDCANCEL）。
4. 返回语义（proc `0x10037580` → `sub_4057B0` 的三臂）：**1 = 退出程序**（→ `0x405B0E/0x405B21` alpha **255** 全黑 → 继续关窗）；**2 = 返回标题**（`0x405A70 push 0FAh / Sleep` + `0x405A7D sub_428A60` + `0x405A82 xor eax,eax` → 返回 0，**不**关窗）；**0 = 取消**（`0x4059BD` alpha **0** 撤减光 → 返回 0）。
5. 运行期实测（`--trace-shims` + `--until ea`）：`agerc!_ShowDialog@12(00000003,00010000,00000000) -> 00000001 ret=00405981`；返回 1 真的继续关窗；像素 f-0004 减光 / f-0005 全黑 / f-0006 恢复。
6. **与「TITLE→Quit 没有渐变」互不影响**：Quit 支由引擎退出清理段发 `SendMessageA(hwnd, WM_CLOSE, 1, 0)`（ret `0x4138C8`）⇒ 直接跳过 `sub_4057B0`；桥接对话框**既不**让 Quit 有确认/渐变，**也修不了** `REQ-01M72GW002QVRSG5J5WJ0KB1KF` 的 (B)。

## 结论（评估）
- **可以桥接**，而且**不必碰 AGERC 的代码/资源**：只要拦 `_ShowDialog@12` 这一个导出（`id` 是选择子，模板只被 AGERC 自己用）。
- 现有 no-op **已经"脱离 AGERC 资源"**（没人读模板），且恒 1 = 引擎的"退出程序"臂 ⇒ 行为上**够用**；不够的是"误点 X 不能撤销"与"范围承诺"（SCOPE §1 说模态 no-op）。
- 方案取舍：**A 宿主原生（SDL_ShowMessageBox）** —— headless 无 video、注入进不去、不可测 ⇒ 只配人工调试档；**B lift 自建（推荐）** —— shim 里进宿主模态循环、画在最后一帧副本上、输入走 `runtime/input.h` 那一份（注入可答）；**C 继续 no-op** —— 今天的行为，零风险。

## 建议（分阶段；每步都有机械判据）
1. **第一步（不画任何 UI）**：`_ShowDialog@12` 从 `addStub` 改成 `add`：打参数 + 只对 `id==3` 出手 + 显式策略（例 `--close-confirm exit|title|cancel`，缺省 `exit`）。判据：`pnpm tools lift check` 绿 + 新 assets CTest 三次子进程分别 `--until ea 0x405B21 / 0x405A82 / 0x405A95` 命中 `stop : until-ea @ eip=…` 且 exit 0（后两条今天到不了 ⇒ 天然可证伪）。
2. **第二步（自建对话框）**：shim 里进宿主模态循环 + 画在最后一帧副本 + 输入走 `input.h` + **关窗宽限必须挂起**（`present_sdl.cpp` 的 `advanceCloseFrame` → `requestStop`，3 帧/500ms）。判据：注入点击的 trace + 三臂像素（减光/全黑/撤销）。
3. ⛔ 不做：从 AGERC 资源取文案（汉化件那份文本是乱码）、把 no-op 改成"静默成功"。

## 未做 / <待核>
- 黑帧未逐 draw 归因；DLU→像素未换算；window 模式的 X 支与关窗宽限挂起未实测；真机是否也发 `WM_CLOSE(1)` 未闭合。
- 本单只出设计，未改任何代码。
