; =====================================================================================
; Shape A 原型阶段 A · `sub_436D00`（0x436D00..0x436D4F，30 条 / 80 B）的**手写** lift
;
; 真源：`corpus/assets/age/AGE.EXE__dumped.sectfix.EXE` 的 PE 原始字节（不是手编）：
;   pnpm tools disasm-at at --ea 0x436D00 --lines 45 --bytes 80
;   逐条对过 `.lst` 的指令窗口（0x436D00 push ebp … 0x436D4D retn 8）。
;
; 契约（`.tmp/irshape/runtime-contract.md` §7 + 复审裁决 1a/1b）：
;   %st  = void*（AOT 臂指向 cpu_.r[0]；夹具传真 LiftRegs）：r[0..7] = EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI（+0..+28），eflags 在 +32
;   %mem = guest EA 0 的宿主基址（i8*；EA 一律 zext 到 i64 再接 GEP —— i32 索引会被符号扩展）
;   %ea  = **resume EA**（本函数 3 个入口：0x436D00 / 0x436D14 / 0x436D19）
;   %cpu = X86*，只给 lift_note_write 用
;   返回 = 下一个 guest EA；0xFFFFFFFF = 回 trampoline 的哨兵
;
; ★ 为什么有 3 个入口：两条 `call` 的返回点 0x436D14 / 0x436D19 都在函数**中段**。
;   两个建表器（0x436BD0 / 0x436C60）本阶段**故意留在解释器里** ⇒ 它们 `retn` 回来时
;   trampoline 拿到的就是这两个中段 EA，必须能从这里接着跑。
; ★ `call rel32`（复审 1b ①）：把返回 EA 压进 **guest 栈**（esp -= 4; 写 [esp]），
;   宿主动作只是 **return 被调方 EA**（⛔ 不是宿主 call —— 那会在 IR 里重造 dispatcher）。
; ★ `retn 8`（复审 1b ②）：ret_ea = [esp]，esp += 4 + 8，return ret_ea。
;   guest 栈是返回地址的**真源**；⛔ 不许用宿主栈替代。
; ★ 分支（`jnz` / `js` / `jns`）是函数内直接跳（复审 1b ③）：⛔ 不回 trampoline、⛔ 不压栈。
; ★ flags：只算**出口可比的那 6 位**（kDiffFlags = 0x8D5 = CF|PF|AF|ZF|SF|OF）。
;   分支只用 ZF（cmp）与 SF（dec）；出口的 CF 由 `or eax,-1`（n<=0 路径）或循环里
;   `xor eax,[esi+edi*4+400h]`（两条路径的最后一次 `dec edx` 之前）清 0，其余 5 位来自
;   最后一次 `dec edx`（`dec` 不动 CF）。`not` / `pop` / `retn` 都不写 flags。
;   ⛔ 如果将来把比较集合放宽（比如加 DF / 中间位），必须把这里的中间 flags 也逐位补全。
; ★ `mov [esi+800h],1` 是 lifted 侧**唯一**的数据写：解释器的 `writeMem` 会先
;   `noteGuestWrite(ea,4)`（SMC 失效）再写 ⇒ 这里也先调 `lift_note_write`。
;   ⚠ `push` 不调：解释器的 `X86::push32` 直接 `mem->wr32`（**不走** writeMem / SMC），
;   为了与解释器臂逐位一致，这里也不调（lifted 侧只复刻它的真实行为）。
; =====================================================================================

declare void @lift_note_write(ptr, i32, i32)
declare i32 @llvm.ctpop.i32(i32)
declare void @llvm.trap()

define i32 @lift_cc_436D00(ptr %st, ptr %mem, i32 %ea, ptr %cpu) {
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
  ; 非比较位的保留值 = eflags & ~0x8D5（-2262 == ~0x8D5 == 0xFFFFF72A）。
  ; ⚠ 这是 `kDiffFlags`（测试里 `lift_ir_proto.cpp` / `lift_ir_shim.cpp` 的唯一定义）的**第二份表示**；
  ;   本阶段只许动 `bad:` + 注释 ⇒ 这里留给生成器阶段：由同一张表注入，⛔ 不再手写常量。
  %keep = and i32 %eflags_in, -2262
  switch i32 %ea, label %bad [
    i32 4418816, label %bb_436D00
    i32 4418836, label %bb_436D14
    i32 4418841, label %bb_436D19
  ]

bad:
  ; 表外 EA：只有 trampoline 建错表才会到这儿。
  ; ⛔ 这里**不许** `ret i32 -1`（那会被 trampoline 当成"guest 自己 ret 到哨兵"= 跑完了，
  ;   把一次派发错误静默变成一次假绿）⇒ **响亮**失败：`llvm.trap` 生成 ud2，进程当场死。
  call void @llvm.trap()
  unreachable

; ---- 入口 0x436D00（函数头）----
bb_436D00:
  ; push ebp
  %esp1 = sub i32 %esp_in, 4
  %esp1_64 = zext i32 %esp1 to i64
  %p_esp1 = getelementptr i8, ptr %mem, i64 %esp1_64
  store i32 %ebp_in, ptr %p_esp1, align 4
  ; push esi
  %esp2 = sub i32 %esp1, 4
  %esp2_64 = zext i32 %esp2 to i64
  %p_esp2 = getelementptr i8, ptr %mem, i64 %esp2_64
  store i32 %esi_in, ptr %p_esp2, align 4
  ; mov esi, ecx（ECX 进来就是 this）；cmp dword ptr [esi+800h], 0
  %flag_ea = add i32 %ecx_in, 2048
  %flag_ea_64 = zext i32 %flag_ea to i64
  %p_flag = getelementptr i8, ptr %mem, i64 %flag_ea_64
  %flag0 = load i32, ptr %p_flag, align 4
  %is_cold = icmp eq i32 %flag0, 0
  ; jnz short 0x436D23
  br i1 %is_cold, label %bb_cold_call, label %bb_after_cold

; ---- 冷路径：call sub_436BD0（0x436D0F）----
bb_cold_call:
  ; `cmp [esi+800h],0` 的 flags（走的是 ZF=1 那一支）：ZF=1、PF=1（低字节 0 有偶数个 1），
  ; CF/OF/AF/SF=0 ⇒ 六位 = 0x44。解释器在 call 前就是这组 flags。
  %flags_cmp = or i32 %keep, 68
  ; call rel32：push 返回 EA 0x436D14，返回被调方 EA 0x436BD0
  %esp3 = sub i32 %esp2, 4
  %esp3_64 = zext i32 %esp3 to i64
  %p_esp3 = getelementptr i8, ptr %mem, i64 %esp3_64
  store i32 4418836, ptr %p_esp3, align 4
  ; 写回当前状态（EBP = 0x436D01 的 mov ebp,esp 之后的值；ESI = ECX）
  store i32 %eax_in, ptr %p_s0, align 4
  store i32 %ecx_in, ptr %p_s1, align 4
  store i32 %edx_in, ptr %p_s2, align 4
  store i32 %ebx_in, ptr %p_s3, align 4
  store i32 %esp3, ptr %p_s4, align 4
  store i32 %esp1, ptr %p_s5, align 4
  store i32 %ecx_in, ptr %p_s6, align 4
  store i32 %edi_in, ptr %p_s7, align 4
  store i32 %flags_cmp, ptr %p_s8, align 4
  ret i32 4418512

; ---- 入口 0x436D14（`call sub_436BD0` 的返回点）----
bb_436D14:
  ; call rel32：push 返回 EA 0x436D19，返回被调方 EA 0x436C60。
  ; ⚠ `sub_436C60` 会 `add ecx,400h`（改写 ECX）—— 那是解释器里发生的事，本块不预演。
  %esp14 = sub i32 %esp_in, 4
  %esp14_64 = zext i32 %esp14 to i64
  %p_esp14 = getelementptr i8, ptr %mem, i64 %esp14_64
  store i32 4418841, ptr %p_esp14, align 4
  ; call 不写 flags ⇒ 原样交回解释器
  store i32 %eax_in, ptr %p_s0, align 4
  store i32 %ecx_in, ptr %p_s1, align 4
  store i32 %edx_in, ptr %p_s2, align 4
  store i32 %ebx_in, ptr %p_s3, align 4
  store i32 %esp14, ptr %p_s4, align 4
  store i32 %ebp_in, ptr %p_s5, align 4
  store i32 %esi_in, ptr %p_s6, align 4
  store i32 %edi_in, ptr %p_s7, align 4
  store i32 %eflags_in, ptr %p_s8, align 4
  ret i32 4418656

; ---- 入口 0x436D19（`call sub_436C60` 的返回点）----
bb_436D19:
  ; mov dword ptr [esi+800h], 1 —— 解释器 writeMem 先 noteGuestWrite（SMC 失效）
  %flag19_ea = add i32 %esi_in, 2048
  call void @lift_note_write(ptr %cpu, i32 %flag19_ea, i32 4)
  %flag19_ea_64 = zext i32 %flag19_ea to i64
  %p_flag19 = getelementptr i8, ptr %mem, i64 %flag19_ea_64
  store i32 1, ptr %p_flag19, align 4
  br label %bb_after_cold

; ---- 0x436D23（冷/热两路汇合）----
bb_after_cold:
  ; 从 0x436D00 进来时 ESP 已经压过 ebp/esi；从 0x436D19 进来时解释器交回的 ESP 就是
  ; "压过 ebp/esi、两次 call 的返回地址已弹掉"之后的值 —— 两者在**同一个调用**里相等，
  ; 但 SSA 值来源不同，所以用 phi 显式写出来。
  %esp2c = phi i32 [ %esp2, %bb_436D00 ], [ %esp_in, %bb_436D19 ]
  %esi_cur = phi i32 [ %ecx_in, %bb_436D00 ], [ %esi_in, %bb_436D19 ]
  %ebp_cur = phi i32 [ %esp1, %bb_436D00 ], [ %ebp_in, %bb_436D19 ]
  ; mov edx, [ebp+arg_0]（arg_0 = n）
  %arg0_ea = add i32 %ebp_cur, 8
  %arg0_ea_64 = zext i32 %arg0_ea to i64
  %p_arg0 = getelementptr i8, ptr %mem, i64 %arg0_ea_64
  %edx1 = load i32, ptr %p_arg0, align 4
  ; or eax, 0FFFFFFFFh（逻辑标志：CF=0、OF=0、AF=0、ZF=0、SF=1、PF=1）
  ; dec edx（十进制递减；不动 CF ⇒ CF 保持 or 清出的 0）
  %edx2 = sub i32 %edx1, 1
  %dec_sf = icmp slt i32 %edx2, 0
  ; js short 0x436D49
  br i1 %dec_sf, label %bb_exit_zero, label %bb_body_setup

; ---- n > 0：循环前序（0x436D2C..0x436D30）----
bb_body_setup:
  ; mov ecx, [ebp+arg_4]（arg_4 = buf）
  %arg4_ea = add i32 %ebp_cur, 12
  %arg4_ea_64 = zext i32 %arg4_ea to i64
  %p_arg4 = getelementptr i8, ptr %mem, i64 %arg4_ea_64
  %ecx1 = load i32, ptr %p_arg4, align 4
  ; push ebx
  %esp_b1 = sub i32 %esp2c, 4
  %esp_b1_64 = zext i32 %esp_b1 to i64
  %p_esp_b1 = getelementptr i8, ptr %mem, i64 %esp_b1_64
  store i32 %ebx_in, ptr %p_esp_b1, align 4
  ; push edi
  %esp_b2 = sub i32 %esp_b1, 4
  %esp_b2_64 = zext i32 %esp_b2 to i64
  %p_esp_b2 = getelementptr i8, ptr %mem, i64 %esp_b2_64
  store i32 %edi_in, ptr %p_esp_b2, align 4
  br label %loop

; ---- 每字节 8 条（0x436D31..0x436D45）----
loop:
  %edx_c = phi i32 [ %edx2, %bb_body_setup ], [ %edx_next, %loop ]
  %ecx_c = phi i32 [ %ecx1, %bb_body_setup ], [ %ecx_next, %loop ]
  %eax_c = phi i32 [ -1, %bb_body_setup ], [ %eax_x, %loop ]
  ; movzx edi, byte ptr [ecx]
  %ecx_c_64 = zext i32 %ecx_c to i64
  %p_byte = getelementptr i8, ptr %mem, i64 %ecx_c_64
  %byte_v = load i8, ptr %p_byte, align 1
  %edi_v = zext i8 %byte_v to i32
  ; movzx ebx, al
  %al = and i32 %eax_c, 255
  ; xor edi, ebx（标志在出口前会被 dec 覆盖；这里只需要值）
  %edi_x = xor i32 %edi_v, %al
  ; shr eax, 8（CF = 移出的 bit0；出口前会被下面的 xor 清 0）
  %eax_s = lshr i32 %eax_c, 8
  ; xor eax, [esi+edi*4+400h]（CF=0、OF=0、AF=0；索引寻址 + 0x400 表基）
  %idx = shl i32 %edi_x, 2
  %tab_ea = add i32 %esi_cur, %idx
  %tab_ea2 = add i32 %tab_ea, 1024
  %tab_ea2_64 = zext i32 %tab_ea2 to i64
  %p_tab = getelementptr i8, ptr %mem, i64 %tab_ea2_64
  %tab_v = load i32, ptr %p_tab, align 4
  %eax_x = xor i32 %eax_s, %tab_v
  ; inc ecx（不改 flags）
  %ecx_next = add i32 %ecx_c, 1
  ; dec edx（写 SF/ZF/AF/PF/OF；CF 不动）
  %edx_next = sub i32 %edx_c, 1
  %loop_sf = icmp slt i32 %edx_next, 0
  ; jns short 0x436D31
  br i1 %loop_sf, label %bb_exit_loop, label %loop

; ---- 循环出口（0x436D47..0x436D48 的 pop edi/pop ebx）----
bb_exit_loop:
  ; 这两条 pop 必须真的从 guest 栈读回保存值（⛔ 不能用入口 SSA 值代填：
  ; 第二次进来时（入口 0x436D19）`%ebx_in/%edi_in` 是解释器当时的值，不一定是这里保存的值）。
  %p_pop_edi_64 = zext i32 %esp_b2 to i64
  %p_pop_edi = getelementptr i8, ptr %mem, i64 %p_pop_edi_64
  %edi_pop = load i32, ptr %p_pop_edi, align 4
  %p_pop_ebx_64 = zext i32 %esp_b1 to i64
  %p_pop_ebx = getelementptr i8, ptr %mem, i64 %p_pop_ebx_64
  %ebx_pop = load i32, ptr %p_pop_ebx, align 4
  ; jns 条件 = `edx_c - 1 >= 0` ⇒ 出口时 edx_c 必为 0 ⇒ 最后一次 `dec edx` 的
  ; flags 是**常量**：ZF=0、SF=1、PF=1（0xFF 有 8 个 1）、AF=1、OF=0（0-1 不溢出：0 与 1 同号）；
  ; CF 由循环里的 `xor eax,[table]` 清 0（dec 不动 CF）⇒ 六位 = 0x94。
  %flags_loop = or i32 %keep, 148
  ; not eax（0x436D49 的收尾；不写 flags）
  %eax_not = xor i32 %eax_x, -1
  br label %bb_retn8

; ---- n <= 0：跳过循环（0x436D49 直接落）----
bb_exit_zero:
  ; 最后一次写 flags 的是 `dec edx`（edx1 = n ≤ 0）；CF 由 `or eax,-1` 清 0。
  ; 其余 5 位按解释器 `setSubFlags` 的公式逐位算（AF = (a^b^r)&0x10；PF = 偶校验）。
  %zf0 = icmp eq i32 %edx2, 0
  %sf0 = icmp slt i32 %edx2, 0
  %zb = and i32 %edx2, 255
  %pc = call i32 @llvm.ctpop.i32(i32 %zb)
  %pc_odd = and i32 %pc, 1
  %pf_even = icmp eq i32 %pc_odd, 0
  %af_x1 = xor i32 %edx1, 1
  %af_x2 = xor i32 %af_x1, %edx2
  %af_b = and i32 %af_x2, 16
  %af0 = icmp ne i32 %af_b, 0
  %of_a = icmp slt i32 %edx1, 0
  %of_b = icmp sge i32 %edx2, 0
  %of0 = and i1 %of_a, %of_b
  %zf_i = select i1 %zf0, i32 64, i32 0
  %sf_i = select i1 %sf0, i32 128, i32 0
  %pf_i = select i1 %pf_even, i32 4, i32 0
  %af_i = select i1 %af0, i32 16, i32 0
  %of_i = select i1 %of0, i32 2048, i32 0
  %six0 = or i32 %zf_i, %sf_i
  %six1 = or i32 %six0, %pf_i
  %six2 = or i32 %six1, %af_i
  %six3 = or i32 %six2, %of_i
  %flags_zero = or i32 %keep, %six3
  ; not eax：EAX 已被 `or eax,-1` 变成 0xFFFFFFFF
  %eax_not0 = xor i32 -1, -1
  br label %bb_retn8

; ---- 出口：pop esi / pop ebp / retn 8（0x436D4B..0x436D4D）----
bb_retn8:
  %eax_pre = phi i32 [ %eax_not, %bb_exit_loop ], [ %eax_not0, %bb_exit_zero ]
  %ecx_out = phi i32 [ %ecx_next, %bb_exit_loop ], [ %ecx_in, %bb_exit_zero ]
  %edx_out = phi i32 [ %edx_next, %bb_exit_loop ], [ %edx2, %bb_exit_zero ]
  %ebx_out = phi i32 [ %ebx_pop, %bb_exit_loop ], [ %ebx_in, %bb_exit_zero ]
  %edi_out = phi i32 [ %edi_pop, %bb_exit_loop ], [ %edi_in, %bb_exit_zero ]
  %flags_out = phi i32 [ %flags_loop, %bb_exit_loop ], [ %flags_zero, %bb_exit_zero ]
  ; 两条路径到这里时 ESP 相同（都是"压过 ebp/esi、循环的 push 已弹掉"）：
  ;   循环路径：pop edi/pop ebx 把 esp_b2 抬回 esp2c；
  ;   零迭代路径：从来没 push ebx/edi ⇒ esp 就是 esp2c。
  ; 于是 pop esi / pop ebp 从 [esp2c] / [esp2c+4] 读回**调用者的原值**（这两条也必须
  ; 真的读栈：入口 0x436D19 的 `%esi_in/%ebp_in` 是中段值，不是原值）。
  %p_pop_esi_64 = zext i32 %esp2c to i64
  %p_pop_esi = getelementptr i8, ptr %mem, i64 %p_pop_esi_64
  %esi_out = load i32, ptr %p_pop_esi, align 4
  %esp_pop_ebp = add i32 %esp2c, 4
  %esp_pop_ebp_64 = zext i32 %esp_pop_ebp to i64
  %p_pop_ebp = getelementptr i8, ptr %mem, i64 %esp_pop_ebp_64
  %ebp_out = load i32, ptr %p_pop_ebp, align 4
  ; retn 8：ret_ea = [esp]（= [esp2c+8]），esp += 4 + 8；连同 pop esi/pop ebp 的 8 字节，
  ; 出口 esp = esp2c + 20 = 入口 esp + 12（= +4 + imm16）。
  %esp_ret = add i32 %esp2c, 8
  %esp_ret_64 = zext i32 %esp_ret to i64
  %p_ret = getelementptr i8, ptr %mem, i64 %esp_ret_64
  %ret_ea = load i32, ptr %p_ret, align 4
  %esp_out = add i32 %esp2c, 20
  store i32 %eax_pre, ptr %p_s0, align 4
  store i32 %ecx_out, ptr %p_s1, align 4
  store i32 %edx_out, ptr %p_s2, align 4
  store i32 %ebx_out, ptr %p_s3, align 4
  store i32 %esp_out, ptr %p_s4, align 4
  store i32 %ebp_out, ptr %p_s5, align 4
  store i32 %esi_out, ptr %p_s6, align 4
  store i32 %edi_out, ptr %p_s7, align 4
  store i32 %flags_out, ptr %p_s8, align 4
  ret i32 %ret_ea
}
