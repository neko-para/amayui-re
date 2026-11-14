; =====================================================================================
; Shape A 原型阶段 B · `sub_4D4B40`（0x4D4B40..0x4D4B6C，14 条 / 45 B）的**手写** lift
;
; 真源：`corpus/assets/age/AGE.EXE__dumped.sectfix.EXE` 的 PE 原始字节（不是手编）：
;   pnpm tools disasm-at at --ea 0x4D4B40 --lines 20 --bytes 50
; 逐条（与 .lst 窗口 0x4D4B40..0x4D4B6A 一致）：
;   4D4B40 push esi · 4D4B41 mov esi,ecx · 4D4B43 mov ecx,[esi+154h] · 4D4B49 mov eax,[esi] ·
;   4D4B4B mov edx,[esi+150h] · 4D4B51 mov eax,[eax+0B4h] · 4D4B57 push ecx · 4D4B58 push edx ·
;   4D4B59 mov ecx,esi · 4D4B5B call eax · 4D4B5D call dword ptr [0051D450] ·
;   4D4B63 mov [esi+0FCh],eax · 4D4B69 pop esi · 4D4B6A retn 4
;
; 契约（与 `lift_436D00.ll` 同一份）：
;   %st  = LiftRegs*：r[0..7] = EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI（+0..+28），eflags 在 +32
;   %mem = guest EA 0 的宿主基址（EA 一律 zext 到 i64 再接 GEP）
;   %ea  = **resume EA**（本函数 3 个入口：0x4D4B40 / 0x4D4B5D / 0x4D4B63）
;   %cpu = X86*，只给 lift_note_write 用
;   返回 = 下一个 guest EA；0xFFFFFFFF = 回 trampoline 的哨兵
;
; ★ 为什么有 3 个入口：两条 `call` 的返回点都在函数**中段**。一旦被调方（lifted / 解释器 /
;   shim）把控制权交回 trampoline，回来的 EA 就是 0x4D4B5D / 0x4D4B63，必须能从这里接着跑。
; ★ ③ `call eax`（动态目标 = 槽值 `[[E]+0xB4]`）：把返回 EA 0x4D4B5D 压 **guest 栈**，返回
;   **槽值 EA**。本载荷的槽指向一条 **lifted EA**（0x436D00）⇒ ③ 走"EA → lifted 表"这一支；
;   ④ 走"IAT 槽 → 桩 EA → shim"——两条路**必须**分开（否则 ③ 等于没测）。
; ★ ④ `call dword ptr [0051D450]`：把返回 EA 0x4D4B63 压 guest 栈，返回**从 guest 0x51D450
;   读到的桩 EA**。⛔ 不在这里调 shim：物化 / 回读 / 弹参全在 trampoline 的 `lift_shim` 里。
;   ★ 这也是"不需要加载 PE"的原因：lifted 与解释器臂都只是从这个 guest 地址读一个 EA。
; ★ `retn 4`：ret_ea = [esp]，esp += 4 + 4；连同 prologue 的 `push esi` 与两条参数 push，
;   出口 esp = 入口 + 8（本载荷调用者的帧在 [E]=返回地址、[E+4]=那个 int 参数）。
; ★ flags：14 条全是 mov / push / pop / call / retn，**没有一条写 flags** ⇒ eflags 在本函数是
;   **恒等**：三个出口都原样写回。⚠ 因此"两臂 eflags 相同"在这条载荷上是**恒等、不是证据**。
; ★ `mov [esi+0FCh],eax` 是 lifted 侧**唯一**的数据写：解释器的 `writeMem` 先
;   `noteGuestWrite(ea,4)`（SMC 失效）再写 ⇒ 这里也先调 `lift_note_write`。
;   `push`/`pop` 走 `push32`/`pop32`（**不走** writeMem / SMC）⇒ 这里也不调（与解释器逐位一致）。
; ★ `bad:` 是**响亮**失败（`llvm.trap` + `unreachable`）：表外 EA 只可能是 trampoline 建错表，
;   ⛔ 不许静默返回哨兵（那会把"派发错了"伪装成"跑完了"）。
; =====================================================================================

declare void @lift_note_write(ptr, i32, i32)
declare void @llvm.trap()

define i32 @lift_cc_4D4B40(ptr %st, ptr %mem, i32 %ea, ptr %cpu) {
entry:
  %p_s0 = getelementptr i8, ptr %st, i64 0
  %p_s1 = getelementptr i8, ptr %st, i64 4
  %p_s2 = getelementptr i8, ptr %st, i64 8
  %p_s3 = getelementptr i8, ptr %st, i64 12
  %p_s4 = getelementptr i8, ptr %st, i64 16
  %p_s5 = getelementptr i8, ptr %st, i64 20
  %p_s6 = getelementptr i8, ptr %st, i64 24
  %p_s7 = getelementptr i8, ptr %st, i64 28
  %p_s8 = getelementptr i8, ptr %st, i64 32
  %eax_in = load i32, ptr %p_s0, align 4
  %ecx_in = load i32, ptr %p_s1, align 4
  %edx_in = load i32, ptr %p_s2, align 4
  %ebx_in = load i32, ptr %p_s3, align 4
  %esp_in = load i32, ptr %p_s4, align 4
  %ebp_in = load i32, ptr %p_s5, align 4
  %esi_in = load i32, ptr %p_s6, align 4
  %edi_in = load i32, ptr %p_s7, align 4
  %eflags_in = load i32, ptr %p_s8, align 4
  switch i32 %ea, label %bad [
    i32 5065536, label %bb_4D4B40
    i32 5065565, label %bb_4D4B5D
    i32 5065571, label %bb_4D4B63
  ]

bad:
  ; 表外 EA：只有 trampoline 建错表才会到这儿。⛔ 不返回哨兵（那等于把"派发错了"当成"跑完了"）。
  call void @llvm.trap()
  unreachable

; ---- 入口 0x4D4B40（函数头）----
bb_4D4B40:
  ; mov esi, ecx ⇒ E = ECX（this）
  ; push esi（保存调用者的 ESI；⛔ 用入口 SSA 值，不做 phi）
  %e1 = sub i32 %esp_in, 4
  %e1_64 = zext i32 %e1 to i64
  %p_e1 = getelementptr i8, ptr %mem, i64 %e1_64
  store i32 %esi_in, ptr %p_e1, align 4
  ; mov ecx, [esi+154h]（= CRC 的 arg_4 = buf）
  %ea154 = add i32 %ecx_in, 340
  %ea154_64 = zext i32 %ea154 to i64
  %p_154 = getelementptr i8, ptr %mem, i64 %ea154_64
  %c154 = load i32, ptr %p_154, align 4
  ; mov eax, [esi]（vtable 指针 V）
  %ecx_in_64 = zext i32 %ecx_in to i64
  %p_this = getelementptr i8, ptr %mem, i64 %ecx_in_64
  %vptr = load i32, ptr %p_this, align 4
  ; mov edx, [esi+150h]（= CRC 的 arg_0 = n）
  %ea150 = add i32 %ecx_in, 336
  %ea150_64 = zext i32 %ea150 to i64
  %p_150 = getelementptr i8, ptr %mem, i64 %ea150_64
  %d150 = load i32, ptr %p_150, align 4
  ; mov eax, [eax+0B4h]（槽值 = ③ 的间接调用目标）
  %ea_slot = add i32 %vptr, 180
  %ea_slot_64 = zext i32 %ea_slot to i64
  %p_slot = getelementptr i8, ptr %mem, i64 %ea_slot_64
  %target = load i32, ptr %p_slot, align 4
  ; push ecx（先压 buf，再压 n ⇒ 被调方看到 [esp+4]=n、[esp+8]=buf，与 `retn 8` 一致）
  %e2 = sub i32 %e1, 4
  %e2_64 = zext i32 %e2 to i64
  %p_e2 = getelementptr i8, ptr %mem, i64 %e2_64
  store i32 %c154, ptr %p_e2, align 4
  ; push edx
  %e3 = sub i32 %e2, 4
  %e3_64 = zext i32 %e3 to i64
  %p_e3 = getelementptr i8, ptr %mem, i64 %e3_64
  store i32 %d150, ptr %p_e3, align 4
  ; mov ecx, esi（thiscall：ECX = this = E）
  ; call eax：push 返回 EA 0x4D4B5D，返回被调方 EA（= 槽值）
  %e4 = sub i32 %e3, 4
  %e4_64 = zext i32 %e4 to i64
  %p_e4 = getelementptr i8, ptr %mem, i64 %e4_64
  store i32 5065565, ptr %p_e4, align 4
  ; 写回调用现场：EAX = 槽值（`call eax` 不改 EAX）、ECX = E、EDX = [E+150h]
  store i32 %target, ptr %p_s0, align 4
  store i32 %ecx_in, ptr %p_s1, align 4
  store i32 %d150, ptr %p_s2, align 4
  store i32 %ebx_in, ptr %p_s3, align 4
  store i32 %e4, ptr %p_s4, align 4
  store i32 %ebp_in, ptr %p_s5, align 4
  store i32 %ecx_in, ptr %p_s6, align 4
  store i32 %edi_in, ptr %p_s7, align 4
  store i32 %eflags_in, ptr %p_s8, align 4
  ret i32 %target

; ---- 入口 0x4D4B5D（`call eax` 的返回点）----
bb_4D4B5D:
  ; 这里 ESI = E（被调方 0x436D00 保留 ESI）；ESP = E-4（指向 prologue 保存的 ESI）。
  ; call dword ptr [0051D450]：push 返回 EA 0x4D4B63，返回**从 IAT 槽读到的桩 EA**。
  %p_iat = getelementptr i8, ptr %mem, i64 5362768
  %shim_ea = load i32, ptr %p_iat, align 4
  %e5 = sub i32 %esp_in, 4
  %e5_64 = zext i32 %e5 to i64
  %p_e5 = getelementptr i8, ptr %mem, i64 %e5_64
  store i32 5065571, ptr %p_e5, align 4
  ; call 不写 flags；被调方的返回值 / 被它改过的 GPR 全部原样交回（%*_in 就是现场）。
  store i32 %eax_in, ptr %p_s0, align 4
  store i32 %ecx_in, ptr %p_s1, align 4
  store i32 %edx_in, ptr %p_s2, align 4
  store i32 %ebx_in, ptr %p_s3, align 4
  store i32 %e5, ptr %p_s4, align 4
  store i32 %ebp_in, ptr %p_s5, align 4
  store i32 %esi_in, ptr %p_s6, align 4
  store i32 %edi_in, ptr %p_s7, align 4
  store i32 %eflags_in, ptr %p_s8, align 4
  ret i32 %shim_ea

; ---- 入口 0x4D4B63（`call ds:[0051D450]` 的返回点）----
bb_4D4B63:
  ; ESI = E；ESP = E-4。mov [esi+0FCh], eax
  %ea_fc = add i32 %esi_in, 252
  call void @lift_note_write(ptr %cpu, i32 %ea_fc, i32 4)
  %ea_fc_64 = zext i32 %ea_fc to i64
  %p_fc = getelementptr i8, ptr %mem, i64 %ea_fc_64
  store i32 %eax_in, ptr %p_fc, align 4
  ; pop esi：必须真的从 guest 栈读回保存值（⛔ 不能用入口 SSA 值代填）
  %esp_esi_64 = zext i32 %esp_in to i64
  %p_pop_esi = getelementptr i8, ptr %mem, i64 %esp_esi_64
  %esi_pop = load i32, ptr %p_pop_esi, align 4
  %esp_ret = add i32 %esp_in, 4
  ; retn 4：ret_ea = [esp]，esp += 4 + 4
  %esp_ret_64 = zext i32 %esp_ret to i64
  %p_ret = getelementptr i8, ptr %mem, i64 %esp_ret_64
  %ret_ea = load i32, ptr %p_ret, align 4
  %esp_out = add i32 %esp_ret, 8
  store i32 %eax_in, ptr %p_s0, align 4
  store i32 %ecx_in, ptr %p_s1, align 4
  store i32 %edx_in, ptr %p_s2, align 4
  store i32 %ebx_in, ptr %p_s3, align 4
  store i32 %esp_out, ptr %p_s4, align 4
  store i32 %ebp_in, ptr %p_s5, align 4
  store i32 %esi_pop, ptr %p_s6, align 4
  store i32 %edi_in, ptr %p_s7, align 4
  store i32 %eflags_in, ptr %p_s8, align 4
  ret i32 %ret_ea
}
