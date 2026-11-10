# 补字节 · EH 链的 6 段在 .lst 里全 COLLAPSED（_CxxThrowException / ___CxxFrameHandler3 / FindHandler / _global_unwind2 / _local_unwind2 / j_RtlUnwind）

- id: REQ-01M713MSM6WKDDTMHCXR6FJX5Q
- type: req
- status: done
- parent: REQ-01M708DF24KY0QZNEHY66S0HYW
- done_reason: 三条 <待核> 全部闭合（语料证据，无常识补）：#1 _CxxThrowException 用 call（0x4E8AB0 call [0x51d1dc]）不是尾部 jmp（其后 leave/ret 8 填满 0x4C）；#2 RaiseException(0xE06D7363, flags=1, 3, &Info)，Info={0x19930520, 对象, 0x544774}，且 guest 的 CallCatchBlock 会校验 record（Info[0] 只接受 0x19930520/21/22）；#3 j_RtlUnwind 是 6 字节 thunk，TaskIp 口径 = 形同 ret 16 正常返回，__local_unwind2 不调 RtlUnwind、__global_unwind2 冷代码。副产品：折叠是导出视图状态（1805/1805 全是库名、跨模块复现），代价是这 1519 条不在 disasm-fn item 目录 ⇒ 调用边静默消失。可复跑 node .tmp/reveq/01M713MSM6WKDDTMHCXR6FJX5Q/disasm-eh.mjs --write（6 条自校验全绿）。仍未取证：RtlUnwind 本体 TargetFrame 值语义（IAT 导入，实现不在镜像内）。
- tags: [corpus, disasm, seh]

## 结果（2026-11-10）：**三条 `<待核>` 全部闭合**（全部来自语料，未用 x86 常识补）

这 6 段在 `.lst` 里是 COLLAPSED（只有名字 + EA + 长度），本轮用"PE 字节 + 反汇编"把它们逐行反解出来（**没有改语料**，那是只读真源）：

- **#1 是 `call`，不是尾部 `jmp`**（强）：`0x4E8AB0 FF 15 DC D1 51 00  call dword ptr [0x51d1dc]`（= `RaiseException`），其后
  `0x4E8AB6 C9 leave` / `0x4E8AB7 C2 08 00 ret 8` 正好填满折叠长度 +0x4C ⇒ 有返回路径；且 `0x4E8AA3 lea eax,[ebp-0xc]` 把 `lpArguments`
  指向**本帧**，尾部 jmp 会悬空。★ `0x4123C9` 是**调用方** `sub_412290` 的返回点（`0x4123C4 call 0x4e8a6e` 的下一条），与 `RaiseException`
  的返回地址 `0x4E8AB6` 是**两层**，⛔ 别混用。
- **#2 `RaiseException(0xE06D7363, flags=1, 3, &Info)`，`Info = {0x19930520, pExceptionObject, 0x00544774}`**（强：`.data` 与 `.lst` 的 `db` 双源逐字节一致）。
  模板在 `0x52FDE8` 起 8 dword。`Info[0]` **不会**被改写成 `0x1994000`（`pThrowInfo=0x544774` 的 `_ThrowInfo <0,…>` ⇒ attributes=0）。
  ★ **硬约束**：guest 的 `CallCatchBlock` **自己校验 record**（`0x4F00F8 cmp [esi],0xE06D7363` / `0x4F0100 cmp [esi+0x10],3` / `0x4F0109…`
  `Info[0] ∈ {0x19930520,21,22}`，不等就 `jne 0x4F0142` 直接 ret）⇒ 宿主合成 `0x1994000` 会让这个 catch **静默不成立**。
- **#3 `j_RtlUnwind` = 6 字节真 thunk**（`0x5130B4 FF 25 68 D2 51 00  jmp [0x51d268]`，`__stdcall` 四参）。全 `.text` 只有 **3** 处 `call 0x5130b4`
  （`0x4E7066` / `0x506071` / `0x5068E3`），三处 push 算术**一致要求**：到 `TargetIp` 时 `esp == call` 之前 + 0x10 ⇒ **形同被 `ret 16` 正常返回**。
  `ReturnValue` 三处都是 NULL、`ExceptionRecord` 可为 0。★ `__local_unwind2` **一次都没调 RtlUnwind**（自己装 `fs:[0]`、按 UnwindMap 走
  `__NLG_Notify/__NLG_Call`）⇒ 它对 RtlUnwind 要求**零**；`__global_unwind2` 传 `RtlUnwind(pRN, 0x5068E8, NULL, NULL)` 且**自己不再恢复 `fs:[0]`**
  （那是 RtlUnwind 的责任），但它在 AGE.EXE 里**无任何引用**（冷代码）。⚠ 宿主**不要**直接调 `0x4E7094`：`___CxxFrameHandler3` 是 thunk，
  `EAX` 是**隐式第 5 参**（`_s_FuncInfo*`）—— 直接调必给错 FuncInfo。

**折叠原因（判断，带依据）**：折叠是**导出时的视图状态**，被折的是**库 / CRT 函数（含导入 thunk）**，不是跳转表 / 数据混排 / 段边界 ——
① 折叠行带**完整带类型的原型**；② 反解全是干净指令流、边界精确（自校验全绿）；③ **1805/1805 折叠条目全是库名**，引擎自写的 `sub_412290`
一个都没折；④ 251 条**首尾相接的折叠带** ⇒ 折叠与展开逐函数交替；⑤ **跨模块复现**（`AGERC.DLL.lst` 也有 1491 条，同一批库名风格）。

⚠ **比"没字节"更贵的代价**：折叠行替掉了 `proc near … endp`，而 `disasm-fn` 的函数身份正是从它来的 ⇒ 这 **1519 条不在 item 目录里**，
后果是**调用边静默消失**（`.lst` 里 `0x5130B4` 只出现 1 次，二进制里却有 3 处 `call`）⇒ `callers/callees/impact` 在库函数上是
"间接调用 + 折叠"的**双重下界且不报错**。

**可复跑**：`node .tmp/reveq/01M713MSM6WKDDTMHCXR6FJX5Q/disasm-eh.mjs --write`（重出逐行反汇编；6 条自校验：C0 二进制 sha256 == `.lst` 头部 ·
C1 每段字节数 == 折叠行长度字段 · C2 指令边界连续且精确落段尾 · C3 每条字节 == PE 该 EA 处字节 · C4 分支目标自算 · C5 PE 段表 == `.lst` Section 1 ·
C6 `.data` 双源一致）。需要 `llvm-objdump`（本机 LLVM 22.1.2，`$LLVM_OBJDUMP` 可指路）。

**仍未取证**：`RtlUnwind` 的 `TargetFrame` **值语义**（unwind 到它为止还是包含它）与"`TargetIp≠0 ⇒ 不返回"这条规则本身 —— `0x51D268` 是
IAT 导入、实现不在镜像内；三处调用点只是原样下传 `pRN`。`flags=1` 的**后果**也只在文档层。`___InternalCxxFrameHandler`(0x4F085B, 0xE6) 与
`CallCatchBlock` 的三个 funclet 入口没逐行读（超出这 6 段）。

## 修法边界（保留，给下一个人）

- 现有工具**都不行**（实测）：`disasm-at at` 只把折叠行打回来 · `span` 要 `--match` 与字节无关 · `disasm verify` 的口径是**文本↔字节的 CP932 转写保真**
  （不是指令级）· `age-format/asm/disassemble` 是 **AGE 脚本字节码** · `disasm-fn show 0x4E8A6E` 报"不落在任何函数里"。
- ⛔ 不要去"展开语料"（那是改只读真源）；缺口是真的 ⇒ 最小可复用件 = 「PE 段表 → EA→文件偏移 → 取字节」纯 JS 段（零依赖），
  建议落点 `tools/lib/pe.mjs` + `disasm-at --bytes`，解码器选型另裁（见工具缺口单）。
- ⛔ 不许用 `Hex-Rays` 的 `.c` 当证据（改写视图、行号不是锚）。
