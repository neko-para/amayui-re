# 🐞S2 emulator-lift · SEH：handler 返回 ContinueExecution 且 CONTEXT 未改 ⇒ 异常被静默丢弃（落到 _Xout_of_range 的 int3，报成'软件中断'）

- id: REQ-01M72GAA7CVKBPP9QRT0PRPBTM
- type: bug
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: apps/emulator-lift/src/runtime/seh.cpp#Seh::step
- severity: S2
- tags: [emulator-lift, seh, seh-dispatch]

## 一句话
guest 的 C++ 异常（`0xE06D7363`，`ExceptionFlags=0x1` NONCONTINUABLE）派发时，handler 返回 `disposition=1`（`ExceptionContinueExecution`）而 **CONTEXT 一个字节都没改**，本线照字面"装载 CONTEXT 继续执行" ⇒ 回到 `RaiseException` 的返回地址继续跑，异常被**静默丢弃**；于是 `0x4586AF call _Xout_of_range` 之后一路 `leave;ret` 落进 `_Xout_of_range` 尾部 padding 的 `int3`（`0x4E6E3F`），摘要写成误导性的 `stop: unimplemented` / `detail: 软件中断（M1 不建模中断表）`。

## 现象（实测，2026-11-10，macOS arm64 / RelWithDebInfo）
`LIFT_SEH_TRACE=1` 的两条路径（`TITLE→OPTION→ADV设定` / `TITLE→GameStart`）trace **逐字段相同**：
```text
[seh] RaiseException(code=0xE06D7363 flags=0x1 nargs=3) rec=… ctx=… 链头=…
[seh] 找 handler 调 0x00513B90（pRN=0x6FFFFB74）⇒ 返回 disposition=1
[seh] 整份装载 CONTEXT：eip=0x004E8AB6（= raise 的 ret_ea，CONTEXT 未被 handler 改）
→ 0x4E8AB6 leave / 0x4E8AB7 ret → 0x4E6E3F int3（`std::_Xout_of_range` 0x4E6E10 + 0x2F 处）
```
反证：`sub_412290`（WinMain）的 FuncInfo `stru_5447B4`（`.data:005447B4`）在 try 状态 2 上有一处 `catch(...)`（HandlerType `<0,0,0,loc_4137B7>`，`.data:00544784`，`0x4137B7` 的 IDA 注释就是 `catch(...)`）；状态 0 上是 5 个 typed catch（`Command_Exit_Exception` / `Command_ShowMessage_Exception` / `Command_Type_Exception` / `SE_Exception` / `BadAlloc_Exception`）。⇒ 这个 `std::out_of_range` **只可能**被那个 `catch(...)` 接住，而它没被接。

## <待核>（这条单要的取证）
1. handler 为什么返回 1 而不是把 CONTEXT 改成 catch pad / 走 `RtlUnwind`？嫌疑：`DISPATCHER_CONTEXT` 现在是一块**全 0**（`src/runtime/seh.cpp` 的 `kDcBytes` + 注释），而 `__CxxFrameHandler3` 可能读 `ControlPc` / `FunctionEntry` / `EstablisherFrame`。
2. handler 调用前后 `ctx_ea` 的字节对比 + `[pRN+8]` 状态字 + `_CallCatchBlock2`（`0x4E738B`）入口参数 —— 需要加诊断（本次只读，没加）。
3. 同类形状在**其它**路径是否也静默丢异常（`Command_Exit_Exception` 走的是 typed catch 分支，可能恰好不触发；要判清楚）。

## 判据（修好后必须成立）
1. `handler 返回 1 ∧ (ExceptionFlags & 1) ∧ CONTEXT 未被改` ⇒ **响亮停**（独立 stop 原因 / 明确 detail），⛔ 不再报"软件中断"。
2. 有 `catch(...)` 可接时，异常必须**转到 catch pad**（观测点 = CONTEXT.eip 落到 FuncInfo 里那个 catch 块的 EA `0x4137B7` 附近，或 `_CallCatchBlock2` 被调用）。
3. 守卫：新的 pure CTest（`tests/seh.cpp` 一族）能红：构造"handler 返回 1 且不改 CONTEXT 且 flags=NONCONTINUABLE"的形状 ⇒ 必须停成响亮原因（修前是静默继续）。

## 边界
- ⛔ 不用它来掩盖字体崩溃：那条的根因是字体名表（另单 `REQ-01M72G8JY1Y989H9H8M02DJ2PX`），本单只管"异常派发不该静默"。
- ⛔ 不动 `corpus/`；结论锚 EA（本页所有 EA 可直接 `pnpm tools disasm-at at --ea` 复核）。

## 未做
- 未加诊断、未定位 handler 返回 1 的真实原因（本轮只读取证）。
