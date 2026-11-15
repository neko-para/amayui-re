; emulator-lift · Shape A / T1c —— 由 `emulator-lift-gen` 生成，⛔ 不手改、不 commit。
; 契约：%st = LiftRegs*（r[0..7] + eflags@+32）；%mem = guest EA 0 的宿主基址；
;       %ea = resume EA；%cpu = X86*；返回值 = 下一个 guest EA。
; 不 inline 的指令 = 一次 @lift_one（语义在解释器里）；
; 指令计数 = 块边界 / 宿主调用前的 @lift_tick（D2 / D2b：只记条数，不推进虚拟时间）。

declare void @lift_note_write(ptr, i32, i32)
declare void @lift_tick(ptr, i32)
declare void @llvm.trap()
declare i32 @llvm.ctpop.i32(i32)

define i32 @lift_cc_4AA1D0(ptr %st, ptr %mem, i32 %ea, ptr %cpu) {
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
  switch i32 %ea, label %bad [
    i32 4891088, label %bb_4AA1D0
    i32 4891119, label %bb_4AA1EF
    i32 4891253, label %bb_4AA275
  ]

bad:
  call void @llvm.trap()
  unreachable

bb_4AA1D0:
  %t0 = getelementptr i8, ptr %st, i64 20
  %t1 = load i32, ptr %t0, align 4
  %t2 = getelementptr i8, ptr %st, i64 16
  %t3 = load i32, ptr %t2, align 4
  %t4 = sub i32 %t3, 4
  %t5 = getelementptr i8, ptr %st, i64 16
  store i32 %t4, ptr %t5, align 4
  %t6 = zext i32 %t4 to i64
  %t7 = getelementptr i8, ptr %mem, i64 %t6
  store i32 %t1, ptr %t7, align 1
  %t8 = getelementptr i8, ptr %st, i64 16
  %t9 = load i32, ptr %t8, align 4
  %t10 = getelementptr i8, ptr %st, i64 20
  store i32 %t9, ptr %t10, align 4
  %t11 = getelementptr i8, ptr %st, i64 4
  %t12 = load i32, ptr %t11, align 4
  %t13 = getelementptr i8, ptr %st, i64 16
  %t14 = load i32, ptr %t13, align 4
  %t15 = sub i32 %t14, 4
  %t16 = getelementptr i8, ptr %st, i64 16
  store i32 %t15, ptr %t16, align 4
  %t17 = zext i32 %t15 to i64
  %t18 = getelementptr i8, ptr %mem, i64 %t17
  store i32 %t12, ptr %t18, align 1
  %t19 = getelementptr i8, ptr %st, i64 20
  %t20 = load i32, ptr %t19, align 4
  %t21 = add i32 16, %t20
  %t22 = zext i32 %t21 to i64
  %t23 = getelementptr i8, ptr %mem, i64 %t22
  %t24 = load i32, ptr %t23, align 1
  %t25 = getelementptr i8, ptr %st, i64 8
  store i32 %t24, ptr %t25, align 4
  %t26 = getelementptr i8, ptr %st, i64 24
  %t27 = load i32, ptr %t26, align 4
  %t28 = getelementptr i8, ptr %st, i64 16
  %t29 = load i32, ptr %t28, align 4
  %t30 = sub i32 %t29, 4
  %t31 = getelementptr i8, ptr %st, i64 16
  store i32 %t30, ptr %t31, align 4
  %t32 = zext i32 %t30 to i64
  %t33 = getelementptr i8, ptr %mem, i64 %t32
  store i32 %t27, ptr %t33, align 1
  %t34 = getelementptr i8, ptr %st, i64 4
  %t35 = load i32, ptr %t34, align 4
  %t36 = getelementptr i8, ptr %st, i64 24
  store i32 %t35, ptr %t36, align 4
  %t37 = getelementptr i8, ptr %st, i64 24
  %t38 = load i32, ptr %t37, align 4
  %t39 = add i32 4, %t38
  %t40 = zext i32 %t39 to i64
  %t41 = getelementptr i8, ptr %mem, i64 %t40
  %t42 = load i32, ptr %t41, align 1
  %t43 = getelementptr i8, ptr %st, i64 0
  store i32 %t42, ptr %t43, align 4
  %t44 = getelementptr i8, ptr %st, i64 20
  %t45 = load i32, ptr %t44, align 4
  %t46 = add i32 12, %t45
  %t47 = zext i32 %t46 to i64
  %t48 = getelementptr i8, ptr %mem, i64 %t47
  %t49 = load i32, ptr %t48, align 1
  %t50 = getelementptr i8, ptr %st, i64 4
  store i32 %t49, ptr %t50, align 4
  %t51 = getelementptr i8, ptr %st, i64 4
  %t52 = load i32, ptr %t51, align 4
  %t53 = getelementptr i8, ptr %st, i64 0
  %t54 = load i32, ptr %t53, align 4
  %t55 = add i32 0, %t54
  %t56 = zext i32 %t55 to i64
  %t57 = getelementptr i8, ptr %mem, i64 %t56
  %t58 = load i32, ptr %t57, align 1
  %t59 = getelementptr i8, ptr %st, i64 32
  %t60 = load i32, ptr %t59, align 4
  %t61 = sub i32 %t52, %t58
  %t62 = and i32 %t52, -1
  %t63 = and i32 %t58, -1
  %t64 = and i32 %t61, -1
  %t65 = zext i32 %t62 to i64
  %t66 = zext i32 %t63 to i64
  %t67 = icmp ult i64 %t65, %t66
  %t68 = and i32 %t64, -2147483648
  %t69 = icmp ne i32 %t68, 0
  %t70 = and i32 %t62, -2147483648
  %t71 = icmp ne i32 %t70, 0
  %t72 = and i32 %t63, -2147483648
  %t73 = icmp ne i32 %t72, 0
  %t74 = icmp ne i1 %t69, %t71
  %t75 = icmp ne i1 %t71, %t73
  %t76 = and i1 %t75, %t74
  %t77 = xor i32 %t62, %t63
  %t78 = xor i32 %t77, %t64
  %t79 = and i32 %t78, 16
  %t80 = icmp ne i32 %t79, 0
  %t81 = icmp eq i32 %t64, 0
  %t82 = and i32 %t64, 255
  %t83 = call i32 @llvm.ctpop.i32(i32 %t82)
  %t84 = and i32 %t83, 1
  %t85 = icmp eq i32 %t84, 0
  %t86 = select i1 %t67, i32 1, i32 0
  %t87 = select i1 %t76, i32 2048, i32 0
  %t88 = select i1 %t80, i32 16, i32 0
  %t89 = select i1 %t81, i32 64, i32 0
  %t90 = select i1 %t69, i32 128, i32 0
  %t91 = select i1 %t85, i32 4, i32 0
  %t92 = or i32 %t86, %t87
  %t93 = or i32 %t92, %t88
  %t94 = or i32 %t93, %t89
  %t95 = or i32 %t94, %t90
  %t96 = or i32 %t95, %t91
  %t97 = and i32 %t60, -2262
  %t98 = or i32 %t97, %t96
  %t99 = getelementptr i8, ptr %st, i64 32
  store i32 %t98, ptr %t99, align 4
  call void @lift_tick(ptr %cpu, i32 10)
  %t100 = getelementptr i8, ptr %st, i64 32
  %t101 = load i32, ptr %t100, align 4
  %t102 = and i32 %t101, 1
  %t103 = icmp ne i32 %t102, 0
  %t104 = and i32 %t101, 4
  %t105 = icmp ne i32 %t104, 0
  %t106 = and i32 %t101, 64
  %t107 = icmp ne i32 %t106, 0
  %t108 = and i32 %t101, 128
  %t109 = icmp ne i32 %t108, 0
  %t110 = and i32 %t101, 2048
  %t111 = icmp ne i32 %t110, 0
  %t112 = xor i1 %t107, true
  br i1 %t112, label %bb_4AA200, label %bb_4AA1E4

bb_4AA1E4:
  %t113 = getelementptr i8, ptr %st, i64 8
  %t114 = load i32, ptr %t113, align 4
  %t115 = getelementptr i8, ptr %st, i64 0
  %t116 = load i32, ptr %t115, align 4
  %t117 = getelementptr i8, ptr %st, i64 32
  %t118 = load i32, ptr %t117, align 4
  %t119 = sub i32 %t114, %t116
  %t120 = and i32 %t114, -1
  %t121 = and i32 %t116, -1
  %t122 = and i32 %t119, -1
  %t123 = zext i32 %t120 to i64
  %t124 = zext i32 %t121 to i64
  %t125 = icmp ult i64 %t123, %t124
  %t126 = and i32 %t122, -2147483648
  %t127 = icmp ne i32 %t126, 0
  %t128 = and i32 %t120, -2147483648
  %t129 = icmp ne i32 %t128, 0
  %t130 = and i32 %t121, -2147483648
  %t131 = icmp ne i32 %t130, 0
  %t132 = icmp ne i1 %t127, %t129
  %t133 = icmp ne i1 %t129, %t131
  %t134 = and i1 %t133, %t132
  %t135 = xor i32 %t120, %t121
  %t136 = xor i32 %t135, %t122
  %t137 = and i32 %t136, 16
  %t138 = icmp ne i32 %t137, 0
  %t139 = icmp eq i32 %t122, 0
  %t140 = and i32 %t122, 255
  %t141 = call i32 @llvm.ctpop.i32(i32 %t140)
  %t142 = and i32 %t141, 1
  %t143 = icmp eq i32 %t142, 0
  %t144 = select i1 %t125, i32 1, i32 0
  %t145 = select i1 %t134, i32 2048, i32 0
  %t146 = select i1 %t138, i32 16, i32 0
  %t147 = select i1 %t139, i32 64, i32 0
  %t148 = select i1 %t127, i32 128, i32 0
  %t149 = select i1 %t143, i32 4, i32 0
  %t150 = or i32 %t144, %t145
  %t151 = or i32 %t150, %t146
  %t152 = or i32 %t151, %t147
  %t153 = or i32 %t152, %t148
  %t154 = or i32 %t153, %t149
  %t155 = and i32 %t118, -2262
  %t156 = or i32 %t155, %t154
  %t157 = getelementptr i8, ptr %st, i64 32
  store i32 %t156, ptr %t157, align 4
  call void @lift_tick(ptr %cpu, i32 2)
  %t158 = getelementptr i8, ptr %st, i64 32
  %t159 = load i32, ptr %t158, align 4
  %t160 = and i32 %t159, 1
  %t161 = icmp ne i32 %t160, 0
  %t162 = and i32 %t159, 4
  %t163 = icmp ne i32 %t162, 0
  %t164 = and i32 %t159, 64
  %t165 = icmp ne i32 %t164, 0
  %t166 = and i32 %t159, 128
  %t167 = icmp ne i32 %t166, 0
  %t168 = and i32 %t159, 2048
  %t169 = icmp ne i32 %t168, 0
  %t170 = xor i1 %t165, true
  br i1 %t170, label %bb_4AA200, label %bb_4AA1E8

bb_4AA1E8:
  %t171 = getelementptr i8, ptr %st, i64 24
  %t172 = load i32, ptr %t171, align 4
  %t173 = getelementptr i8, ptr %st, i64 4
  store i32 %t172, ptr %t173, align 4
  call void @lift_tick(ptr %cpu, i32 2)
  %t174 = getelementptr i8, ptr %st, i64 16
  %t175 = load i32, ptr %t174, align 4
  %t176 = sub i32 %t175, 4
  %t177 = getelementptr i8, ptr %st, i64 16
  store i32 %t176, ptr %t177, align 4
  %t178 = zext i32 %t176 to i64
  %t179 = getelementptr i8, ptr %mem, i64 %t178
  store i32 4891119, ptr %t179, align 1
  ret i32 4243328

bb_4AA1EF:
  %t180 = getelementptr i8, ptr %st, i64 24
  %t181 = load i32, ptr %t180, align 4
  %t182 = add i32 4, %t181
  %t183 = zext i32 %t182 to i64
  %t184 = getelementptr i8, ptr %mem, i64 %t183
  %t185 = load i32, ptr %t184, align 1
  %t186 = getelementptr i8, ptr %st, i64 0
  store i32 %t185, ptr %t186, align 4
  %t187 = getelementptr i8, ptr %st, i64 0
  %t188 = load i32, ptr %t187, align 4
  %t189 = add i32 0, %t188
  %t190 = zext i32 %t189 to i64
  %t191 = getelementptr i8, ptr %mem, i64 %t190
  %t192 = load i32, ptr %t191, align 1
  %t193 = getelementptr i8, ptr %st, i64 4
  store i32 %t192, ptr %t193, align 4
  %t194 = getelementptr i8, ptr %st, i64 20
  %t195 = load i32, ptr %t194, align 4
  %t196 = add i32 8, %t195
  %t197 = zext i32 %t196 to i64
  %t198 = getelementptr i8, ptr %mem, i64 %t197
  %t199 = load i32, ptr %t198, align 1
  %t200 = getelementptr i8, ptr %st, i64 0
  store i32 %t199, ptr %t200, align 4
  %t201 = getelementptr i8, ptr %st, i64 4
  %t202 = load i32, ptr %t201, align 4
  call void @lift_tick(ptr %cpu, i32 3)
  %t203 = getelementptr i8, ptr %st, i64 0
  %t204 = load i32, ptr %t203, align 4
  %t205 = add i32 0, %t204
  call void @lift_note_write(ptr %cpu, i32 %t205, i32 4)
  %t206 = zext i32 %t205 to i64
  %t207 = getelementptr i8, ptr %mem, i64 %t206
  store i32 %t202, ptr %t207, align 1
  %t208 = getelementptr i8, ptr %st, i64 16
  %t209 = load i32, ptr %t208, align 4
  %t210 = zext i32 %t209 to i64
  %t211 = getelementptr i8, ptr %mem, i64 %t210
  %t212 = load i32, ptr %t211, align 1
  %t213 = add i32 %t209, 4
  %t214 = getelementptr i8, ptr %st, i64 16
  store i32 %t213, ptr %t214, align 4
  %t215 = getelementptr i8, ptr %st, i64 24
  store i32 %t212, ptr %t215, align 4
  %t216 = getelementptr i8, ptr %st, i64 20
  %t217 = load i32, ptr %t216, align 4
  %t218 = getelementptr i8, ptr %st, i64 16
  store i32 %t217, ptr %t218, align 4
  %t219 = getelementptr i8, ptr %st, i64 16
  %t220 = load i32, ptr %t219, align 4
  %t221 = zext i32 %t220 to i64
  %t222 = getelementptr i8, ptr %mem, i64 %t221
  %t223 = load i32, ptr %t222, align 1
  %t224 = add i32 %t220, 4
  %t225 = getelementptr i8, ptr %st, i64 16
  store i32 %t224, ptr %t225, align 4
  %t226 = getelementptr i8, ptr %st, i64 20
  store i32 %t223, ptr %t226, align 4
  call void @lift_tick(ptr %cpu, i32 5)
  %t227 = getelementptr i8, ptr %st, i64 16
  %t228 = load i32, ptr %t227, align 4
  %t229 = zext i32 %t228 to i64
  %t230 = getelementptr i8, ptr %mem, i64 %t229
  %t231 = load i32, ptr %t230, align 1
  %t232 = add i32 %t228, 4
  %t233 = getelementptr i8, ptr %st, i64 16
  store i32 %t232, ptr %t233, align 4
  %t234 = getelementptr i8, ptr %st, i64 16
  %t235 = load i32, ptr %t234, align 4
  %t236 = add i32 %t235, 12
  %t237 = getelementptr i8, ptr %st, i64 16
  store i32 %t236, ptr %t237, align 4
  ret i32 %t231

bb_4AA200:
  %t238 = getelementptr i8, ptr %st, i64 4
  %t239 = load i32, ptr %t238, align 4
  %t240 = getelementptr i8, ptr %st, i64 8
  %t241 = load i32, ptr %t240, align 4
  %t242 = getelementptr i8, ptr %st, i64 32
  %t243 = load i32, ptr %t242, align 4
  %t244 = sub i32 %t239, %t241
  %t245 = and i32 %t239, -1
  %t246 = and i32 %t241, -1
  %t247 = and i32 %t244, -1
  %t248 = zext i32 %t245 to i64
  %t249 = zext i32 %t246 to i64
  %t250 = icmp ult i64 %t248, %t249
  %t251 = and i32 %t247, -2147483648
  %t252 = icmp ne i32 %t251, 0
  %t253 = and i32 %t245, -2147483648
  %t254 = icmp ne i32 %t253, 0
  %t255 = and i32 %t246, -2147483648
  %t256 = icmp ne i32 %t255, 0
  %t257 = icmp ne i1 %t252, %t254
  %t258 = icmp ne i1 %t254, %t256
  %t259 = and i1 %t258, %t257
  %t260 = xor i32 %t245, %t246
  %t261 = xor i32 %t260, %t247
  %t262 = and i32 %t261, 16
  %t263 = icmp ne i32 %t262, 0
  %t264 = icmp eq i32 %t247, 0
  %t265 = and i32 %t247, 255
  %t266 = call i32 @llvm.ctpop.i32(i32 %t265)
  %t267 = and i32 %t266, 1
  %t268 = icmp eq i32 %t267, 0
  %t269 = select i1 %t250, i32 1, i32 0
  %t270 = select i1 %t259, i32 2048, i32 0
  %t271 = select i1 %t263, i32 16, i32 0
  %t272 = select i1 %t264, i32 64, i32 0
  %t273 = select i1 %t252, i32 128, i32 0
  %t274 = select i1 %t268, i32 4, i32 0
  %t275 = or i32 %t269, %t270
  %t276 = or i32 %t275, %t271
  %t277 = or i32 %t276, %t272
  %t278 = or i32 %t277, %t273
  %t279 = or i32 %t278, %t274
  %t280 = and i32 %t243, -2262
  %t281 = or i32 %t280, %t279
  %t282 = getelementptr i8, ptr %st, i64 32
  store i32 %t281, ptr %t282, align 4
  call void @lift_tick(ptr %cpu, i32 2)
  %t283 = getelementptr i8, ptr %st, i64 32
  %t284 = load i32, ptr %t283, align 4
  %t285 = and i32 %t284, 1
  %t286 = icmp ne i32 %t285, 0
  %t287 = and i32 %t284, 4
  %t288 = icmp ne i32 %t287, 0
  %t289 = and i32 %t284, 64
  %t290 = icmp ne i32 %t289, 0
  %t291 = and i32 %t284, 128
  %t292 = icmp ne i32 %t291, 0
  %t293 = and i32 %t284, 2048
  %t294 = icmp ne i32 %t293, 0
  br i1 %t290, label %bb_4AA27D, label %bb_4AA204

bb_4AA204:
  %t295 = getelementptr i8, ptr %st, i64 4
  %t296 = load i32, ptr %t295, align 4
  %t297 = add i32 757, %t296
  %t298 = zext i32 %t297 to i64
  %t299 = getelementptr i8, ptr %mem, i64 %t298
  %t300 = load i8, ptr %t299, align 1
  %t301 = zext i8 %t300 to i32
  %t302 = and i32 %t301, 255
  %t303 = getelementptr i8, ptr %st, i64 32
  %t304 = load i32, ptr %t303, align 4
  %t305 = sub i32 %t302, 0
  %t306 = and i32 %t302, 255
  %t307 = and i32 0, 255
  %t308 = and i32 %t305, 255
  %t309 = zext i32 %t306 to i64
  %t310 = zext i32 %t307 to i64
  %t311 = icmp ult i64 %t309, %t310
  %t312 = and i32 %t308, 128
  %t313 = icmp ne i32 %t312, 0
  %t314 = and i32 %t306, 128
  %t315 = icmp ne i32 %t314, 0
  %t316 = and i32 %t307, 128
  %t317 = icmp ne i32 %t316, 0
  %t318 = icmp ne i1 %t313, %t315
  %t319 = icmp ne i1 %t315, %t317
  %t320 = and i1 %t319, %t318
  %t321 = xor i32 %t306, %t307
  %t322 = xor i32 %t321, %t308
  %t323 = and i32 %t322, 16
  %t324 = icmp ne i32 %t323, 0
  %t325 = icmp eq i32 %t308, 0
  %t326 = and i32 %t308, 255
  %t327 = call i32 @llvm.ctpop.i32(i32 %t326)
  %t328 = and i32 %t327, 1
  %t329 = icmp eq i32 %t328, 0
  %t330 = select i1 %t311, i32 1, i32 0
  %t331 = select i1 %t320, i32 2048, i32 0
  %t332 = select i1 %t324, i32 16, i32 0
  %t333 = select i1 %t325, i32 64, i32 0
  %t334 = select i1 %t313, i32 128, i32 0
  %t335 = select i1 %t329, i32 4, i32 0
  %t336 = or i32 %t330, %t331
  %t337 = or i32 %t336, %t332
  %t338 = or i32 %t337, %t333
  %t339 = or i32 %t338, %t334
  %t340 = or i32 %t339, %t335
  %t341 = and i32 %t304, -2262
  %t342 = or i32 %t341, %t340
  %t343 = getelementptr i8, ptr %st, i64 32
  store i32 %t342, ptr %t343, align 4
  %t344 = getelementptr i8, ptr %st, i64 4
  %t345 = load i32, ptr %t344, align 4
  %t346 = getelementptr i8, ptr %st, i64 8
  store i32 %t345, ptr %t346, align 4
  call void @lift_tick(ptr %cpu, i32 3)
  %t347 = getelementptr i8, ptr %st, i64 32
  %t348 = load i32, ptr %t347, align 4
  %t349 = and i32 %t348, 1
  %t350 = icmp ne i32 %t349, 0
  %t351 = and i32 %t348, 4
  %t352 = icmp ne i32 %t351, 0
  %t353 = and i32 %t348, 64
  %t354 = icmp ne i32 %t353, 0
  %t355 = and i32 %t348, 128
  %t356 = icmp ne i32 %t355, 0
  %t357 = and i32 %t348, 2048
  %t358 = icmp ne i32 %t357, 0
  %t359 = xor i1 %t354, true
  br i1 %t359, label %bb_4AA269, label %bb_4AA20F

bb_4AA20F:
  %t360 = getelementptr i8, ptr %st, i64 4
  %t361 = load i32, ptr %t360, align 4
  %t362 = add i32 8, %t361
  %t363 = zext i32 %t362 to i64
  %t364 = getelementptr i8, ptr %mem, i64 %t363
  %t365 = load i32, ptr %t364, align 1
  %t366 = getelementptr i8, ptr %st, i64 0
  store i32 %t365, ptr %t366, align 4
  %t367 = getelementptr i8, ptr %st, i64 0
  %t368 = load i32, ptr %t367, align 4
  %t369 = add i32 757, %t368
  %t370 = zext i32 %t369 to i64
  %t371 = getelementptr i8, ptr %mem, i64 %t370
  %t372 = load i8, ptr %t371, align 1
  %t373 = zext i8 %t372 to i32
  %t374 = and i32 %t373, 255
  %t375 = getelementptr i8, ptr %st, i64 32
  %t376 = load i32, ptr %t375, align 4
  %t377 = sub i32 %t374, 0
  %t378 = and i32 %t374, 255
  %t379 = and i32 0, 255
  %t380 = and i32 %t377, 255
  %t381 = zext i32 %t378 to i64
  %t382 = zext i32 %t379 to i64
  %t383 = icmp ult i64 %t381, %t382
  %t384 = and i32 %t380, 128
  %t385 = icmp ne i32 %t384, 0
  %t386 = and i32 %t378, 128
  %t387 = icmp ne i32 %t386, 0
  %t388 = and i32 %t379, 128
  %t389 = icmp ne i32 %t388, 0
  %t390 = icmp ne i1 %t385, %t387
  %t391 = icmp ne i1 %t387, %t389
  %t392 = and i1 %t391, %t390
  %t393 = xor i32 %t378, %t379
  %t394 = xor i32 %t393, %t380
  %t395 = and i32 %t394, 16
  %t396 = icmp ne i32 %t395, 0
  %t397 = icmp eq i32 %t380, 0
  %t398 = and i32 %t380, 255
  %t399 = call i32 @llvm.ctpop.i32(i32 %t398)
  %t400 = and i32 %t399, 1
  %t401 = icmp eq i32 %t400, 0
  %t402 = select i1 %t383, i32 1, i32 0
  %t403 = select i1 %t392, i32 2048, i32 0
  %t404 = select i1 %t396, i32 16, i32 0
  %t405 = select i1 %t397, i32 64, i32 0
  %t406 = select i1 %t385, i32 128, i32 0
  %t407 = select i1 %t401, i32 4, i32 0
  %t408 = or i32 %t402, %t403
  %t409 = or i32 %t408, %t404
  %t410 = or i32 %t409, %t405
  %t411 = or i32 %t410, %t406
  %t412 = or i32 %t411, %t407
  %t413 = and i32 %t376, -2262
  %t414 = or i32 %t413, %t412
  %t415 = getelementptr i8, ptr %st, i64 32
  store i32 %t414, ptr %t415, align 4
  call void @lift_tick(ptr %cpu, i32 3)
  %t416 = getelementptr i8, ptr %st, i64 32
  %t417 = load i32, ptr %t416, align 4
  %t418 = and i32 %t417, 1
  %t419 = icmp ne i32 %t418, 0
  %t420 = and i32 %t417, 4
  %t421 = icmp ne i32 %t420, 0
  %t422 = and i32 %t417, 64
  %t423 = icmp ne i32 %t422, 0
  %t424 = and i32 %t417, 128
  %t425 = icmp ne i32 %t424, 0
  %t426 = and i32 %t417, 2048
  %t427 = icmp ne i32 %t426, 0
  %t428 = xor i1 %t423, true
  br i1 %t428, label %bb_4AA242, label %bb_4AA21B

bb_4AA21B:
  %t429 = getelementptr i8, ptr %st, i64 0
  %t430 = load i32, ptr %t429, align 4
  %t431 = getelementptr i8, ptr %st, i64 4
  store i32 %t430, ptr %t431, align 4
  %t432 = getelementptr i8, ptr %st, i64 4
  %t433 = load i32, ptr %t432, align 4
  %t434 = add i32 0, %t433
  %t435 = zext i32 %t434 to i64
  %t436 = getelementptr i8, ptr %mem, i64 %t435
  %t437 = load i32, ptr %t436, align 1
  %t438 = getelementptr i8, ptr %st, i64 0
  store i32 %t437, ptr %t438, align 4
  %t439 = getelementptr i8, ptr %st, i64 0
  %t440 = load i32, ptr %t439, align 4
  %t441 = add i32 757, %t440
  %t442 = zext i32 %t441 to i64
  %t443 = getelementptr i8, ptr %mem, i64 %t442
  %t444 = load i8, ptr %t443, align 1
  %t445 = zext i8 %t444 to i32
  %t446 = and i32 %t445, 255
  %t447 = getelementptr i8, ptr %st, i64 32
  %t448 = load i32, ptr %t447, align 4
  %t449 = sub i32 %t446, 0
  %t450 = and i32 %t446, 255
  %t451 = and i32 0, 255
  %t452 = and i32 %t449, 255
  %t453 = zext i32 %t450 to i64
  %t454 = zext i32 %t451 to i64
  %t455 = icmp ult i64 %t453, %t454
  %t456 = and i32 %t452, 128
  %t457 = icmp ne i32 %t456, 0
  %t458 = and i32 %t450, 128
  %t459 = icmp ne i32 %t458, 0
  %t460 = and i32 %t451, 128
  %t461 = icmp ne i32 %t460, 0
  %t462 = icmp ne i1 %t457, %t459
  %t463 = icmp ne i1 %t459, %t461
  %t464 = and i1 %t463, %t462
  %t465 = xor i32 %t450, %t451
  %t466 = xor i32 %t465, %t452
  %t467 = and i32 %t466, 16
  %t468 = icmp ne i32 %t467, 0
  %t469 = icmp eq i32 %t452, 0
  %t470 = and i32 %t452, 255
  %t471 = call i32 @llvm.ctpop.i32(i32 %t470)
  %t472 = and i32 %t471, 1
  %t473 = icmp eq i32 %t472, 0
  %t474 = select i1 %t455, i32 1, i32 0
  %t475 = select i1 %t464, i32 2048, i32 0
  %t476 = select i1 %t468, i32 16, i32 0
  %t477 = select i1 %t469, i32 64, i32 0
  %t478 = select i1 %t457, i32 128, i32 0
  %t479 = select i1 %t473, i32 4, i32 0
  %t480 = or i32 %t474, %t475
  %t481 = or i32 %t480, %t476
  %t482 = or i32 %t481, %t477
  %t483 = or i32 %t482, %t478
  %t484 = or i32 %t483, %t479
  %t485 = and i32 %t448, -2262
  %t486 = or i32 %t485, %t484
  %t487 = getelementptr i8, ptr %st, i64 32
  store i32 %t486, ptr %t487, align 4
  call void @lift_tick(ptr %cpu, i32 4)
  %t488 = getelementptr i8, ptr %st, i64 32
  %t489 = load i32, ptr %t488, align 4
  %t490 = and i32 %t489, 1
  %t491 = icmp ne i32 %t490, 0
  %t492 = and i32 %t489, 4
  %t493 = icmp ne i32 %t492, 0
  %t494 = and i32 %t489, 64
  %t495 = icmp ne i32 %t494, 0
  %t496 = and i32 %t489, 128
  %t497 = icmp ne i32 %t496, 0
  %t498 = and i32 %t489, 2048
  %t499 = icmp ne i32 %t498, 0
  %t500 = xor i1 %t495, true
  br i1 %t500, label %bb_4AA23D, label %bb_4AA228

bb_4AA228:
  call void @lift_tick(ptr %cpu, i32 1)
  br label %bb_4AA230

bb_4AA230:
  %t501 = getelementptr i8, ptr %st, i64 0
  %t502 = load i32, ptr %t501, align 4
  %t503 = getelementptr i8, ptr %st, i64 4
  store i32 %t502, ptr %t503, align 4
  %t504 = getelementptr i8, ptr %st, i64 4
  %t505 = load i32, ptr %t504, align 4
  %t506 = add i32 0, %t505
  %t507 = zext i32 %t506 to i64
  %t508 = getelementptr i8, ptr %mem, i64 %t507
  %t509 = load i32, ptr %t508, align 1
  %t510 = getelementptr i8, ptr %st, i64 0
  store i32 %t509, ptr %t510, align 4
  %t511 = getelementptr i8, ptr %st, i64 0
  %t512 = load i32, ptr %t511, align 4
  %t513 = add i32 757, %t512
  %t514 = zext i32 %t513 to i64
  %t515 = getelementptr i8, ptr %mem, i64 %t514
  %t516 = load i8, ptr %t515, align 1
  %t517 = zext i8 %t516 to i32
  %t518 = and i32 %t517, 255
  %t519 = getelementptr i8, ptr %st, i64 32
  %t520 = load i32, ptr %t519, align 4
  %t521 = sub i32 %t518, 0
  %t522 = and i32 %t518, 255
  %t523 = and i32 0, 255
  %t524 = and i32 %t521, 255
  %t525 = zext i32 %t522 to i64
  %t526 = zext i32 %t523 to i64
  %t527 = icmp ult i64 %t525, %t526
  %t528 = and i32 %t524, 128
  %t529 = icmp ne i32 %t528, 0
  %t530 = and i32 %t522, 128
  %t531 = icmp ne i32 %t530, 0
  %t532 = and i32 %t523, 128
  %t533 = icmp ne i32 %t532, 0
  %t534 = icmp ne i1 %t529, %t531
  %t535 = icmp ne i1 %t531, %t533
  %t536 = and i1 %t535, %t534
  %t537 = xor i32 %t522, %t523
  %t538 = xor i32 %t537, %t524
  %t539 = and i32 %t538, 16
  %t540 = icmp ne i32 %t539, 0
  %t541 = icmp eq i32 %t524, 0
  %t542 = and i32 %t524, 255
  %t543 = call i32 @llvm.ctpop.i32(i32 %t542)
  %t544 = and i32 %t543, 1
  %t545 = icmp eq i32 %t544, 0
  %t546 = select i1 %t527, i32 1, i32 0
  %t547 = select i1 %t536, i32 2048, i32 0
  %t548 = select i1 %t540, i32 16, i32 0
  %t549 = select i1 %t541, i32 64, i32 0
  %t550 = select i1 %t529, i32 128, i32 0
  %t551 = select i1 %t545, i32 4, i32 0
  %t552 = or i32 %t546, %t547
  %t553 = or i32 %t552, %t548
  %t554 = or i32 %t553, %t549
  %t555 = or i32 %t554, %t550
  %t556 = or i32 %t555, %t551
  %t557 = and i32 %t520, -2262
  %t558 = or i32 %t557, %t556
  %t559 = getelementptr i8, ptr %st, i64 32
  store i32 %t558, ptr %t559, align 4
  call void @lift_tick(ptr %cpu, i32 4)
  %t560 = getelementptr i8, ptr %st, i64 32
  %t561 = load i32, ptr %t560, align 4
  %t562 = and i32 %t561, 1
  %t563 = icmp ne i32 %t562, 0
  %t564 = and i32 %t561, 4
  %t565 = icmp ne i32 %t564, 0
  %t566 = and i32 %t561, 64
  %t567 = icmp ne i32 %t566, 0
  %t568 = and i32 %t561, 128
  %t569 = icmp ne i32 %t568, 0
  %t570 = and i32 %t561, 2048
  %t571 = icmp ne i32 %t570, 0
  br i1 %t567, label %bb_4AA230, label %bb_4AA23D

bb_4AA23D:
  %t572 = getelementptr i8, ptr %st, i64 4
  %t573 = load i32, ptr %t572, align 4
  %t574 = getelementptr i8, ptr %st, i64 20
  %t575 = load i32, ptr %t574, align 4
  %t576 = add i32 12, %t575
  call void @lift_note_write(ptr %cpu, i32 %t576, i32 4)
  %t577 = zext i32 %t576 to i64
  %t578 = getelementptr i8, ptr %mem, i64 %t577
  store i32 %t573, ptr %t578, align 1
  call void @lift_tick(ptr %cpu, i32 2)
  br label %bb_4AA269

bb_4AA242:
  %t579 = getelementptr i8, ptr %st, i64 4
  %t580 = load i32, ptr %t579, align 4
  %t581 = add i32 4, %t580
  %t582 = zext i32 %t581 to i64
  %t583 = getelementptr i8, ptr %mem, i64 %t582
  %t584 = load i32, ptr %t583, align 1
  %t585 = getelementptr i8, ptr %st, i64 0
  store i32 %t584, ptr %t585, align 4
  %t586 = getelementptr i8, ptr %st, i64 0
  %t587 = load i32, ptr %t586, align 4
  %t588 = add i32 757, %t587
  %t589 = zext i32 %t588 to i64
  %t590 = getelementptr i8, ptr %mem, i64 %t589
  %t591 = load i8, ptr %t590, align 1
  %t592 = zext i8 %t591 to i32
  %t593 = and i32 %t592, 255
  %t594 = getelementptr i8, ptr %st, i64 32
  %t595 = load i32, ptr %t594, align 4
  %t596 = sub i32 %t593, 0
  %t597 = and i32 %t593, 255
  %t598 = and i32 0, 255
  %t599 = and i32 %t596, 255
  %t600 = zext i32 %t597 to i64
  %t601 = zext i32 %t598 to i64
  %t602 = icmp ult i64 %t600, %t601
  %t603 = and i32 %t599, 128
  %t604 = icmp ne i32 %t603, 0
  %t605 = and i32 %t597, 128
  %t606 = icmp ne i32 %t605, 0
  %t607 = and i32 %t598, 128
  %t608 = icmp ne i32 %t607, 0
  %t609 = icmp ne i1 %t604, %t606
  %t610 = icmp ne i1 %t606, %t608
  %t611 = and i1 %t610, %t609
  %t612 = xor i32 %t597, %t598
  %t613 = xor i32 %t612, %t599
  %t614 = and i32 %t613, 16
  %t615 = icmp ne i32 %t614, 0
  %t616 = icmp eq i32 %t599, 0
  %t617 = and i32 %t599, 255
  %t618 = call i32 @llvm.ctpop.i32(i32 %t617)
  %t619 = and i32 %t618, 1
  %t620 = icmp eq i32 %t619, 0
  %t621 = select i1 %t602, i32 1, i32 0
  %t622 = select i1 %t611, i32 2048, i32 0
  %t623 = select i1 %t615, i32 16, i32 0
  %t624 = select i1 %t616, i32 64, i32 0
  %t625 = select i1 %t604, i32 128, i32 0
  %t626 = select i1 %t620, i32 4, i32 0
  %t627 = or i32 %t621, %t622
  %t628 = or i32 %t627, %t623
  %t629 = or i32 %t628, %t624
  %t630 = or i32 %t629, %t625
  %t631 = or i32 %t630, %t626
  %t632 = and i32 %t595, -2262
  %t633 = or i32 %t632, %t631
  %t634 = getelementptr i8, ptr %st, i64 32
  store i32 %t633, ptr %t634, align 4
  call void @lift_tick(ptr %cpu, i32 3)
  %t635 = getelementptr i8, ptr %st, i64 32
  %t636 = load i32, ptr %t635, align 4
  %t637 = and i32 %t636, 1
  %t638 = icmp ne i32 %t637, 0
  %t639 = and i32 %t636, 4
  %t640 = icmp ne i32 %t639, 0
  %t641 = and i32 %t636, 64
  %t642 = icmp ne i32 %t641, 0
  %t643 = and i32 %t636, 128
  %t644 = icmp ne i32 %t643, 0
  %t645 = and i32 %t636, 2048
  %t646 = icmp ne i32 %t645, 0
  %t647 = xor i1 %t642, true
  br i1 %t647, label %bb_4AA266, label %bb_4AA24E

bb_4AA24E:
  %t648 = getelementptr i8, ptr %st, i64 28
  %t649 = load i32, ptr %t648, align 4
  %t650 = getelementptr i8, ptr %st, i64 28
  store i32 %t649, ptr %t650, align 4
  call void @lift_tick(ptr %cpu, i32 1)
  br label %bb_4AA250

bb_4AA250:
  %t651 = getelementptr i8, ptr %st, i64 4
  %t652 = load i32, ptr %t651, align 4
  %t653 = getelementptr i8, ptr %st, i64 0
  %t654 = load i32, ptr %t653, align 4
  %t655 = add i32 8, %t654
  %t656 = zext i32 %t655 to i64
  %t657 = getelementptr i8, ptr %mem, i64 %t656
  %t658 = load i32, ptr %t657, align 1
  %t659 = getelementptr i8, ptr %st, i64 32
  %t660 = load i32, ptr %t659, align 4
  %t661 = sub i32 %t652, %t658
  %t662 = and i32 %t652, -1
  %t663 = and i32 %t658, -1
  %t664 = and i32 %t661, -1
  %t665 = zext i32 %t662 to i64
  %t666 = zext i32 %t663 to i64
  %t667 = icmp ult i64 %t665, %t666
  %t668 = and i32 %t664, -2147483648
  %t669 = icmp ne i32 %t668, 0
  %t670 = and i32 %t662, -2147483648
  %t671 = icmp ne i32 %t670, 0
  %t672 = and i32 %t663, -2147483648
  %t673 = icmp ne i32 %t672, 0
  %t674 = icmp ne i1 %t669, %t671
  %t675 = icmp ne i1 %t671, %t673
  %t676 = and i1 %t675, %t674
  %t677 = xor i32 %t662, %t663
  %t678 = xor i32 %t677, %t664
  %t679 = and i32 %t678, 16
  %t680 = icmp ne i32 %t679, 0
  %t681 = icmp eq i32 %t664, 0
  %t682 = and i32 %t664, 255
  %t683 = call i32 @llvm.ctpop.i32(i32 %t682)
  %t684 = and i32 %t683, 1
  %t685 = icmp eq i32 %t684, 0
  %t686 = select i1 %t667, i32 1, i32 0
  %t687 = select i1 %t676, i32 2048, i32 0
  %t688 = select i1 %t680, i32 16, i32 0
  %t689 = select i1 %t681, i32 64, i32 0
  %t690 = select i1 %t669, i32 128, i32 0
  %t691 = select i1 %t685, i32 4, i32 0
  %t692 = or i32 %t686, %t687
  %t693 = or i32 %t692, %t688
  %t694 = or i32 %t693, %t689
  %t695 = or i32 %t694, %t690
  %t696 = or i32 %t695, %t691
  %t697 = and i32 %t660, -2262
  %t698 = or i32 %t697, %t696
  %t699 = getelementptr i8, ptr %st, i64 32
  store i32 %t698, ptr %t699, align 4
  call void @lift_tick(ptr %cpu, i32 2)
  %t700 = getelementptr i8, ptr %st, i64 32
  %t701 = load i32, ptr %t700, align 4
  %t702 = and i32 %t701, 1
  %t703 = icmp ne i32 %t702, 0
  %t704 = and i32 %t701, 4
  %t705 = icmp ne i32 %t704, 0
  %t706 = and i32 %t701, 64
  %t707 = icmp ne i32 %t706, 0
  %t708 = and i32 %t701, 128
  %t709 = icmp ne i32 %t708, 0
  %t710 = and i32 %t701, 2048
  %t711 = icmp ne i32 %t710, 0
  %t712 = xor i1 %t707, true
  br i1 %t712, label %bb_4AA266, label %bb_4AA255

bb_4AA255:
  %t713 = getelementptr i8, ptr %st, i64 0
  %t714 = load i32, ptr %t713, align 4
  %t715 = getelementptr i8, ptr %st, i64 4
  store i32 %t714, ptr %t715, align 4
  %t716 = getelementptr i8, ptr %st, i64 4
  %t717 = load i32, ptr %t716, align 4
  call void @lift_tick(ptr %cpu, i32 1)
  %t718 = getelementptr i8, ptr %st, i64 20
  %t719 = load i32, ptr %t718, align 4
  %t720 = add i32 12, %t719
  call void @lift_note_write(ptr %cpu, i32 %t720, i32 4)
  %t721 = zext i32 %t720 to i64
  %t722 = getelementptr i8, ptr %mem, i64 %t721
  store i32 %t717, ptr %t722, align 1
  %t723 = getelementptr i8, ptr %st, i64 0
  %t724 = load i32, ptr %t723, align 4
  %t725 = add i32 4, %t724
  %t726 = zext i32 %t725 to i64
  %t727 = getelementptr i8, ptr %mem, i64 %t726
  %t728 = load i32, ptr %t727, align 1
  %t729 = getelementptr i8, ptr %st, i64 0
  store i32 %t728, ptr %t729, align 4
  %t730 = getelementptr i8, ptr %st, i64 0
  %t731 = load i32, ptr %t730, align 4
  %t732 = add i32 757, %t731
  %t733 = zext i32 %t732 to i64
  %t734 = getelementptr i8, ptr %mem, i64 %t733
  %t735 = load i8, ptr %t734, align 1
  %t736 = zext i8 %t735 to i32
  %t737 = and i32 %t736, 255
  %t738 = getelementptr i8, ptr %st, i64 32
  %t739 = load i32, ptr %t738, align 4
  %t740 = sub i32 %t737, 0
  %t741 = and i32 %t737, 255
  %t742 = and i32 0, 255
  %t743 = and i32 %t740, 255
  %t744 = zext i32 %t741 to i64
  %t745 = zext i32 %t742 to i64
  %t746 = icmp ult i64 %t744, %t745
  %t747 = and i32 %t743, 128
  %t748 = icmp ne i32 %t747, 0
  %t749 = and i32 %t741, 128
  %t750 = icmp ne i32 %t749, 0
  %t751 = and i32 %t742, 128
  %t752 = icmp ne i32 %t751, 0
  %t753 = icmp ne i1 %t748, %t750
  %t754 = icmp ne i1 %t750, %t752
  %t755 = and i1 %t754, %t753
  %t756 = xor i32 %t741, %t742
  %t757 = xor i32 %t756, %t743
  %t758 = and i32 %t757, 16
  %t759 = icmp ne i32 %t758, 0
  %t760 = icmp eq i32 %t743, 0
  %t761 = and i32 %t743, 255
  %t762 = call i32 @llvm.ctpop.i32(i32 %t761)
  %t763 = and i32 %t762, 1
  %t764 = icmp eq i32 %t763, 0
  %t765 = select i1 %t746, i32 1, i32 0
  %t766 = select i1 %t755, i32 2048, i32 0
  %t767 = select i1 %t759, i32 16, i32 0
  %t768 = select i1 %t760, i32 64, i32 0
  %t769 = select i1 %t748, i32 128, i32 0
  %t770 = select i1 %t764, i32 4, i32 0
  %t771 = or i32 %t765, %t766
  %t772 = or i32 %t771, %t767
  %t773 = or i32 %t772, %t768
  %t774 = or i32 %t773, %t769
  %t775 = or i32 %t774, %t770
  %t776 = and i32 %t739, -2262
  %t777 = or i32 %t776, %t775
  %t778 = getelementptr i8, ptr %st, i64 32
  store i32 %t777, ptr %t778, align 4
  call void @lift_tick(ptr %cpu, i32 4)
  %t779 = getelementptr i8, ptr %st, i64 32
  %t780 = load i32, ptr %t779, align 4
  %t781 = and i32 %t780, 1
  %t782 = icmp ne i32 %t781, 0
  %t783 = and i32 %t780, 4
  %t784 = icmp ne i32 %t783, 0
  %t785 = and i32 %t780, 64
  %t786 = icmp ne i32 %t785, 0
  %t787 = and i32 %t780, 128
  %t788 = icmp ne i32 %t787, 0
  %t789 = and i32 %t780, 2048
  %t790 = icmp ne i32 %t789, 0
  br i1 %t786, label %bb_4AA250, label %bb_4AA266

bb_4AA266:
  %t791 = getelementptr i8, ptr %st, i64 0
  %t792 = load i32, ptr %t791, align 4
  %t793 = getelementptr i8, ptr %st, i64 20
  %t794 = load i32, ptr %t793, align 4
  %t795 = add i32 12, %t794
  call void @lift_note_write(ptr %cpu, i32 %t795, i32 4)
  %t796 = zext i32 %t795 to i64
  %t797 = getelementptr i8, ptr %mem, i64 %t796
  store i32 %t792, ptr %t797, align 1
  call void @lift_tick(ptr %cpu, i32 1)
  br label %bb_4AA269

bb_4AA269:
  %t798 = getelementptr i8, ptr %st, i64 8
  %t799 = load i32, ptr %t798, align 4
  %t800 = getelementptr i8, ptr %st, i64 16
  %t801 = load i32, ptr %t800, align 4
  %t802 = sub i32 %t801, 4
  %t803 = getelementptr i8, ptr %st, i64 16
  store i32 %t802, ptr %t803, align 4
  %t804 = zext i32 %t802 to i64
  %t805 = getelementptr i8, ptr %mem, i64 %t804
  store i32 %t799, ptr %t805, align 1
  %t806 = getelementptr i8, ptr %st, i64 20
  %t807 = load i32, ptr %t806, align 4
  %t808 = add i32 -4, %t807
  %t809 = getelementptr i8, ptr %st, i64 8
  store i32 %t808, ptr %t809, align 4
  %t810 = getelementptr i8, ptr %st, i64 8
  %t811 = load i32, ptr %t810, align 4
  %t812 = getelementptr i8, ptr %st, i64 16
  %t813 = load i32, ptr %t812, align 4
  %t814 = sub i32 %t813, 4
  %t815 = getelementptr i8, ptr %st, i64 16
  store i32 %t814, ptr %t815, align 4
  %t816 = zext i32 %t814 to i64
  %t817 = getelementptr i8, ptr %mem, i64 %t816
  store i32 %t811, ptr %t817, align 1
  %t818 = getelementptr i8, ptr %st, i64 24
  %t819 = load i32, ptr %t818, align 4
  %t820 = getelementptr i8, ptr %st, i64 4
  store i32 %t819, ptr %t820, align 4
  call void @lift_tick(ptr %cpu, i32 5)
  %t821 = getelementptr i8, ptr %st, i64 16
  %t822 = load i32, ptr %t821, align 4
  %t823 = sub i32 %t822, 4
  %t824 = getelementptr i8, ptr %st, i64 16
  store i32 %t823, ptr %t824, align 4
  %t825 = zext i32 %t823 to i64
  %t826 = getelementptr i8, ptr %mem, i64 %t825
  store i32 4891253, ptr %t826, align 1
  ret i32 4885232

bb_4AA275:
  %t827 = getelementptr i8, ptr %st, i64 20
  %t828 = load i32, ptr %t827, align 4
  %t829 = add i32 12, %t828
  %t830 = zext i32 %t829 to i64
  %t831 = getelementptr i8, ptr %mem, i64 %t830
  %t832 = load i32, ptr %t831, align 1
  %t833 = getelementptr i8, ptr %st, i64 4
  store i32 %t832, ptr %t833, align 4
  %t834 = getelementptr i8, ptr %st, i64 4
  %t835 = load i32, ptr %t834, align 4
  %t836 = getelementptr i8, ptr %st, i64 20
  %t837 = load i32, ptr %t836, align 4
  %t838 = add i32 16, %t837
  %t839 = zext i32 %t838 to i64
  %t840 = getelementptr i8, ptr %mem, i64 %t839
  %t841 = load i32, ptr %t840, align 1
  %t842 = getelementptr i8, ptr %st, i64 32
  %t843 = load i32, ptr %t842, align 4
  %t844 = sub i32 %t835, %t841
  %t845 = and i32 %t835, -1
  %t846 = and i32 %t841, -1
  %t847 = and i32 %t844, -1
  %t848 = zext i32 %t845 to i64
  %t849 = zext i32 %t846 to i64
  %t850 = icmp ult i64 %t848, %t849
  %t851 = and i32 %t847, -2147483648
  %t852 = icmp ne i32 %t851, 0
  %t853 = and i32 %t845, -2147483648
  %t854 = icmp ne i32 %t853, 0
  %t855 = and i32 %t846, -2147483648
  %t856 = icmp ne i32 %t855, 0
  %t857 = icmp ne i1 %t852, %t854
  %t858 = icmp ne i1 %t854, %t856
  %t859 = and i1 %t858, %t857
  %t860 = xor i32 %t845, %t846
  %t861 = xor i32 %t860, %t847
  %t862 = and i32 %t861, 16
  %t863 = icmp ne i32 %t862, 0
  %t864 = icmp eq i32 %t847, 0
  %t865 = and i32 %t847, 255
  %t866 = call i32 @llvm.ctpop.i32(i32 %t865)
  %t867 = and i32 %t866, 1
  %t868 = icmp eq i32 %t867, 0
  %t869 = select i1 %t850, i32 1, i32 0
  %t870 = select i1 %t859, i32 2048, i32 0
  %t871 = select i1 %t863, i32 16, i32 0
  %t872 = select i1 %t864, i32 64, i32 0
  %t873 = select i1 %t852, i32 128, i32 0
  %t874 = select i1 %t868, i32 4, i32 0
  %t875 = or i32 %t869, %t870
  %t876 = or i32 %t875, %t871
  %t877 = or i32 %t876, %t872
  %t878 = or i32 %t877, %t873
  %t879 = or i32 %t878, %t874
  %t880 = and i32 %t843, -2262
  %t881 = or i32 %t880, %t879
  %t882 = getelementptr i8, ptr %st, i64 32
  store i32 %t881, ptr %t882, align 4
  call void @lift_tick(ptr %cpu, i32 3)
  %t883 = getelementptr i8, ptr %st, i64 32
  %t884 = load i32, ptr %t883, align 4
  %t885 = and i32 %t884, 1
  %t886 = icmp ne i32 %t885, 0
  %t887 = and i32 %t884, 4
  %t888 = icmp ne i32 %t887, 0
  %t889 = and i32 %t884, 64
  %t890 = icmp ne i32 %t889, 0
  %t891 = and i32 %t884, 128
  %t892 = icmp ne i32 %t891, 0
  %t893 = and i32 %t884, 2048
  %t894 = icmp ne i32 %t893, 0
  %t895 = xor i1 %t890, true
  br i1 %t895, label %bb_4AA204, label %bb_4AA27D

bb_4AA27D:
  %t896 = getelementptr i8, ptr %st, i64 20
  %t897 = load i32, ptr %t896, align 4
  %t898 = add i32 8, %t897
  %t899 = zext i32 %t898 to i64
  %t900 = getelementptr i8, ptr %mem, i64 %t899
  %t901 = load i32, ptr %t900, align 1
  %t902 = getelementptr i8, ptr %st, i64 0
  store i32 %t901, ptr %t902, align 4
  %t903 = getelementptr i8, ptr %st, i64 4
  %t904 = load i32, ptr %t903, align 4
  call void @lift_tick(ptr %cpu, i32 1)
  %t905 = getelementptr i8, ptr %st, i64 0
  %t906 = load i32, ptr %t905, align 4
  %t907 = add i32 0, %t906
  call void @lift_note_write(ptr %cpu, i32 %t907, i32 4)
  %t908 = zext i32 %t907 to i64
  %t909 = getelementptr i8, ptr %mem, i64 %t908
  store i32 %t904, ptr %t909, align 1
  %t910 = getelementptr i8, ptr %st, i64 16
  %t911 = load i32, ptr %t910, align 4
  %t912 = zext i32 %t911 to i64
  %t913 = getelementptr i8, ptr %mem, i64 %t912
  %t914 = load i32, ptr %t913, align 1
  %t915 = add i32 %t911, 4
  %t916 = getelementptr i8, ptr %st, i64 16
  store i32 %t915, ptr %t916, align 4
  %t917 = getelementptr i8, ptr %st, i64 24
  store i32 %t914, ptr %t917, align 4
  %t918 = getelementptr i8, ptr %st, i64 20
  %t919 = load i32, ptr %t918, align 4
  %t920 = getelementptr i8, ptr %st, i64 16
  store i32 %t919, ptr %t920, align 4
  %t921 = getelementptr i8, ptr %st, i64 16
  %t922 = load i32, ptr %t921, align 4
  %t923 = zext i32 %t922 to i64
  %t924 = getelementptr i8, ptr %mem, i64 %t923
  %t925 = load i32, ptr %t924, align 1
  %t926 = add i32 %t922, 4
  %t927 = getelementptr i8, ptr %st, i64 16
  store i32 %t926, ptr %t927, align 4
  %t928 = getelementptr i8, ptr %st, i64 20
  store i32 %t925, ptr %t928, align 4
  call void @lift_tick(ptr %cpu, i32 5)
  %t929 = getelementptr i8, ptr %st, i64 16
  %t930 = load i32, ptr %t929, align 4
  %t931 = zext i32 %t930 to i64
  %t932 = getelementptr i8, ptr %mem, i64 %t931
  %t933 = load i32, ptr %t932, align 1
  %t934 = add i32 %t930, 4
  %t935 = getelementptr i8, ptr %st, i64 16
  store i32 %t934, ptr %t935, align 4
  %t936 = getelementptr i8, ptr %st, i64 16
  %t937 = load i32, ptr %t936, align 4
  %t938 = add i32 %t937, 12
  %t939 = getelementptr i8, ptr %st, i64 16
  store i32 %t938, ptr %t939, align 4
  ret i32 %t933
}

