# AOT · S7（结构翻译）的降级普查与一条被回退的放宽

> 读数日期 2026-11-22 / win32 / `E:\Projects\amayui-re-dev`（dev 车道，实验形态未提交）。
> 口径：`RelWithDebInfo`、缺省 AOT 形状 `split8-O2-lto-abi`、同一份 PE 与同一闭包（单元根 **10,980**）。
> 原始记录（gitignored，未入库）：`.tmp/lift-opt/report.md`（实测报告，本文件的所有读数出自它）·
> `.tmp/lift-opt/s7-relax.patch`（被回退的那份 diff）· `build-01.log`（放宽前）· `build-all-s7.log` /
> `s7-relax1.txt`（放宽后）· `build-02.log`（leave 修正实验）· `log-mf-{bin-all,s7}.txt`（30 帧指纹）。
> ★ 主树 `apps/emulator-lift/src/aot/lift_struct.cpp` 现在是**没有放宽**的形态：`transferEsp` 里没有
> `espAdd`，`analyzeStructure` 的 `②-a` 仍然"有间接 `call` ⇒ 整单元判死"（`:285-316`）。§3 说的是 patch 里的事。

## 0 怎么跑

**生成期**（判据读数与降级清单都在 `lift-gen` 的 stderr；`--closure-gate` 由 CMake 的生成步骤传入）：

```bash
cd apps/emulator-lift
cmake --build --preset windows-x64 2>&1 | tee ../../.tmp/lift-opt/build.log
rg '\[S7 criteria\]|\[S7 criteria/site accounts\]|\[S7 downgrade\]' ../../.tmp/lift-opt/build.log
```

`lift-gen` 在生成那一趟打三行汇总 + **每个降级单元根一行**（`lift-gen: [S7 downgrade] <根> reason=… site=…`，
传染的再带 `contagious-from=`），最后一行 `total <N> unit roots are not structurable`。
增量构建不会重跑生成步骤 —— 要重跑就删 `out/aot/lift_aot.part*.ll` 或 touch 闭包输入。

**运行期**（30 帧定长；每趟用全新空 `--appdata`）：

```bash
# cwd = apps/emulator-lift
out/emulator-lift-aot.exe --present window --present-hidden --skip-logo --audio off \
    --max-frames 30 --game-dir ..\..\links\install --appdata <每趟新建的空目录>
```

- `frames : 30（present 计数；本臂 --max-frames=30）` 这一行自证帧数口径；
- `table lookups` 在停止摘要的 `table : … lookups=… hits=… misses=…`（`src/aot/aot_host.cpp:547`）；
- `dispatch : total=…（lifted=… shim=…）`、`insns :`、`region :`、`synthetic :` 同一段摘要；
- `boundary` **不在这份摘要里**：它是窗口 profile 的桶，读数形如 `boundary : … ms` + `iter=`；
  `boundary ns/派发 = boundary_ms × 1e6 / iter`。窗口档的采集脚本是 `.tmp/lift-opt/measure.ps1`
  （`--profile` + 一个 boot→TITLE 窗口 `.tmp/lift-opt/lift-opt-boot.plan`，每配置 5 趟取中位）。

## 1 判据是什么

S7 问的是**单元根能不能一对一结构翻译**：guest `call` → 宿主 `call`、guest `ret` → 宿主 `ret`。
前提是"guest 栈上那一格返回地址 ↔ 宿主栈上的返回地址"一一对应 —— 这只能**按函数**判定，判据在
`apps/emulator-lift/src/aot/lift_struct.h`：

| # | 判据 | 锚 |
|---|---|---|
| 1 | `esp` 全程可静态跟踪（`push/pop`、`add/sub esp,imm`、`mov esp,ebp`、`lea esp,[esp/ebp+k]`、`leave`；`call` 的栈清理按被调方 `retn imm16` 建模） | `lift_struct.h:13-16` |
| 2 | 没有任何指令写"返回地址那一格"（`[esp+0]` / `[ebp+4]`，语法判据） | `:17-20` |
| 3 | 无动态返回：每个 `ret` 站点的 `esp` 偏移必须是静态已知的 **0** | `:21-22` |
| 4 | **无间接 `call reg/mem`**（这些站点要落 `lift_dyn`） | `:23-25` |
| 5 | 所有静态 callee 都是已知的已发射单元（表外 / 被丢弃 ⇒ 不可证明） | `:26-27` |

**传递闭包**（`lift_struct.h:29-33`）：`structured(F) = selfOk(F) && ∀C∈callees(F): structured(C)`，
按不动点从"自身不过"的集合反向传染（单调递减 ⇒ 必收敛），降级清单带原因。
**单调可回退**（`:35-36`）：任何没能转换的站点保持原语义 —— 非结构化臂逐块回 `ret i32 <EA>`，
由 trampoline 接（AOT 臂里**没有解释器**可回退，所以"回退"是回派发器，不是回解释器）。

实现：`analyzeStructure`（`lift_struct.cpp:243`）= `②-a` 语法判据（`:283-316`）→ `②-b` esp 数据流
（`:318-411`，`Ret` 的 `esp==0` 判据在 `:362-370`）→ `③` fixpoint（`:452-472`）→ `④` 站点账（`:479-526`）。

## 2 降级普查

放宽前一次生成的全量降级清单：**5,637 个降级单元根**。把每个根的 `contagious-from=` 链
**追到自身不过的那个根**、按它的原因分组：

| 首因（追到根） | 单元根 | 占降级 | 自身 / 传染 | 例（EA） |
|---|---:|---:|---|---|
| **间接 `call`** | **4,300** | **76%** | 1,682 / 2,618 | 单元根 `004E8C21`（站 `004E8C57`）；最大来源根 `004E73EB` · `004E8A6E` |
| 动态返回（`ret` 的 esp 不可知 / 非 0） | 1,201 | — | 653 / 548 | —（本轮未记 EA） |
| 可变长栈 | 104 | — | 31 / 73 | — |
| 写返回槽 | 32 | — | 14 / 18 | — |
| 表外 callee | **0** | — | 0 / 0 | — |

> 104 / 32 的"传染"数由总数减自身得出（报告只给自身数）。**降级根最大来源** = `contagious-from=`
> **直接**指向它的降级根数（一跳、不含自身、不含传递）：`004E73EB` **614** 条、`004E8A6E` **180** 条。
> 分组口径在 dev 的 `build-01.log` 降级清单上复核过：**5,637** 行按上法正好分到 4,300 / 1,201 / 104 / 32 / 0。

⇒ 间接 `call` 一条占掉约四分之三；其中 2,618 个根自己是干净的，只是**调用图上有它**。

## 3 放宽了什么（已实现，在 patch 里）

只放宽**判据 4**，而且收窄成"可证"：

> 间接 `call` 之后把 `esp` 标成**未知**（⛔ **不假设** callee 的 `ret imm16` 清理字节数 —— 那才是不能证的东西）；
> 只有当函数自己**可证明地**把 `esp` 重建回入口值（`mov esp,ebp` / `leave`，且 `ebp` 自入口起可跟踪）时，
> `ret` 的 "esp == 0" 判据才成立。

配套（**前提，不是装饰**）：`kUnknown` 参与算术必须仍是未知（新增 `espAdd()`）。旧代码里
`kUnknown + 4` 会变成一个"看着已知"的偏移（判据假绿），而且是有符号溢出（UB）；
间接 `call` 之后紧跟 `push` / `add esp,imm` 正是常见形态，所以这条守卫必须先有。

**为什么语义不变**：发射器的间接 `call` 是 `%r = tail call i32 @lift_dyn(...)` 紧跟
`emitCallReturnCheck(%r, resume)`；`%r != resume` 的兜底是 `ret i32 %r`（原样上抛给 trampoline），
与今天的 `ret i32` 出口逐位相同；而 `emitRet` 在结构化与非结构化单元里发的是**同一条 IR**。
⇒ 放宽只改变**调用点**用宿主 `call` 还是 `ret i32 <目标>`，不改变任何单元的出口语义。

## 4 读数

### 4.1 生成期（同一份 PE、同一闭包）

| 读数 | 放宽前 | 放宽后 | Δ |
|---|---:|---:|---:|
| 单元根 | 10,980 | 10,980 | — |
| 自身通过 | 8,600 | **9,533** | +933 |
| **可结构化** | **5,343** | **6,411** | **+1,068（+20.0%）** |
| 传染降级 | 3,257 | 3,122 | −135 |
| 首因 间接 call | 1,682 | 703 | −979 |
| 首因 动态返回 | 653 | 696 | +43 |
| **宿主 `call` 站点** | **1,572** | **2,967** | **+1,395（+89%）** |
| 其余派发站点 | 18,499 | 17,104 | −1,395 |
| `ret` 站点（在可结构化单元内） | 1,863 | 2,234 | +371 |

★ 读数口径：`lift-gen` 的 `[S7 结构翻译]` 与 `[S7 criteria/site accounts]` 是两套措辞，个别站点数会差个位数；
本文（与报告）一律取 **site-accounts** 那一行。

### 4.2 运行期（30 帧定长 + 窗口 profile，同一负载）

| 读数 | Phase 1 | Phase 1 + S7 | Δ |
|---|---:|---:|---|
| `table lookups` | 40,822,828 | **40,697,791** | **−125,037（−0.31%）** |
| `lift_dyn lookups` | 13,888,190 | 13,910,314 | +22,124 |
| `insns` | 804,666,097 | 805,640,407 | +0.12%（**不作判据**） |
| `boundary`（boot 窗口 5 趟中位） | 11.320（交错 A/B 的 B 臂）/ 11.768（串行序列） | **11.412**（极差 11.227–11.443） | −0.36 ns/派发（噪声内） |

**五项指纹逐位相同**：`exit 0` · `frames 30` · `stop @ 00498AE4`（max-frames，Present）·
arena `in_use=108595152 peak=108595152 surfaces=21 heap_blocks=6691` · `table misses 0` ·
`synthetic 定时器回调=0 续延=18 SEH(0/0/0)`。

门禁：`ctest --test-dir out --output-on-failure` ⇒ **137/137**（331.8 s）；`pnpm test` ⇒ **614 pass / 0 fail**。
⚠ 时点口径：那趟全量 `ctest` 是在 `host_window` 那条 `MessageFilter` 改动**之前**的形态上跑的；
改成 POD 之后只编过 `emulator-lift-aot` 目标，全量没在新形态上重跑（报告 §4.2 自记）。

⚠ **读数方法学**（报告 §7）：本机同时跑着另一条车道的 exe 时，同一份二进制全线慢 **15–25%**，那一轮全部作废。
⇒ 可信的只有两类：**交错 A/B 的配对差**（同时间窗内交替跑 A/B）与**同一轮 5 趟内部**的相对关系；
跨轮的**绝对值**含机器漂移，只当方向参考。

### 4.3 放宽**没有**解锁的东西

`FELLOW` 主循环 `00412290` 仍因 `004E8C21` 传染而降级，而 `004E8C21` 自身仍是「间接 `call`」
（site `004E8C57`）—— 它在那条间接 `call` 之后**没有**可跟踪的帧恢复，本次的"可证"放宽**结构上**管不到它。
`004BBAB0` 同理（卡 `004B60C0`）。⇒ 任务书估的 **85–105 ms** 没有拿到：新解锁的 1,395 个宿主 `call`
站点几乎都是冷代码（CRT 辅助函数一类），本负载上只值 −0.31% 派发。

## 5 为什么父级没把它并进这一批

**裁决：这一批不收**（不是"以后再说"）。四条理由，前两条是量，后两条是判据：

1. **收益 ≈ 0.03 ms/切换帧**（本负载 −0.31% 派发），而它改的是**发射形状**的判据面；
   按"改判据要付形状审查的账"这条口径，这个账与收益不成比例。
2. 本批真正站得住的收益在 shim chrome / 消息泵 / 派发序言那一侧；S7 这条即使全拿也只有 0.03 ms。
3. **论证有一处对不上**：判据 4 的理由是"caller 的宿主帧在那次尾调用里就没了"
   （`lift_struct.h:23-25` 与 `:29-33` 都是这个理由）。但当前发射器发的是
   `%r = tail call i32 @lift_dyn(...)` 紧跟 `emitCallReturnCheck(%r, resume)` —— **`%r` 在被用** ⇒
   该点**不是**真尾调用 ⇒ **D53 的前提在现状下需要重新证明**。放宽在实测上是安全的（五项指纹相同），
   但那是"读数相同"，不是"理由成立"；两者不能互相顶替。
4. **缺一条形状级差分**：现有 `lift-s7-structured` 只跑 `tests/ir/s7_roots.txt` 的固定根
   （`CMakeLists.txt` 里那对 `LIFT_S7_CALLER_EA` / `LIFT_S7_CALLEE_EA` 是一对一硬编码），
   覆盖不到新解锁的 **1,395** 个宿主 `call` 站点。

**接受条件（判据，不是愿望）**：补一条 CTest / 差分用例，形状至少覆盖
「单元内有间接 `call` **+** callee 用 `ret imm16` 清字节 **+** 函数自己用 `mov esp,ebp` / `leave` 恢复帧」，
要求与解释器（`Machine`，即 `tests/*` 用的语义 oracle）臂**逐位相同**。
有了这条再评估 —— 在那之前，patch 只留在 `.tmp/lift-opt/s7-relax.patch`，主树保持未放宽形态。

## 6. `leave` 的 esp 模型：查清了 —— 是三条偏差，且**没有**错编译

读数日期 2026-11-22 / 主树 `E:\Projects\amayui-re`（HEAD `a6674dc`）/ 生成期口径同 §4；完整取证在
`.tmp/leave-model/report.md`（gitignored：三趟 stderr、`structured` 清单、11 个风险单元的反汇编窗口）。
**§4 里"这 63 个靠偏差才通过、真 esp 是入口+4"的推断被证伪**。

1. **那 63 个单元在 `ret` 处的真 esp 是入口 +0**：把模型按 Intel 改对（三处一起补）重跑，它们
   **63/63 仍然自证通过**（可结构化 5,343 → 5,335，掉的 8 个与这 63 不相交）。
2. **偏差是三条，不是一条**：`transferEsp` 里 `push ebp` / `pop ebp` 也被"目的操作数是 EBP"分支
   （`lift_struct.cpp:163-191` 的 `if (isEbp32)`）截走 —— 只把 `ebp` 置 `kUnknown`、**不动 esp**。
   于是标准帧上 `push ebp` 漏的 −4 与 `leave` 漏的 +4 **成对抵消**（`mov esp,ebp; pop ebp` 尾声同理）。
   机械核对：234/234 个含 `leave` 的单元，`leave` 命中的 `ebp` 都是"`push ebp` 紧接 `mov ebp,esp`"。
3. **真正"靠偏差"的集合 = 11 个（8 个进 `structured`）**，形态全是 `… pop ebp; ret [imm16]` 的
   **共享尾声块**（单元根在函数中间）。它们的 `ret` 处真 esp 确实是**入口+4**，但那 4 字节正是返回槽
   （入口 esp 指"保存的 ebp"那一格）⇒ **弹对了**。4 个在 `closure.txt` 里是 `why: indirect-entry`、
   2 个有跳表证据 ⇒ 不是死代码；但即使执行也对。
4. **对当前 HEAD 没有潜伏错编译**，三重证据：
   - **集合**：base 与 full 的可结构化差集只有 8 个，且它们**不是任何宿主 `call` 的靶子**
     （宿主 `call` 站点 1,572 → 1,572，一个没变）；
   - **产物**：full 模型生成的 8 份 `.ll` 与 base **逐字节只差 header 的两处计数器**（每份 4 字节；
     总字节 132,744,647 相同）⇒ 发射物等价；
   - **设计**：`emitRet` 在 structured / 非 structured 里发**同一条 IR**（`lift_gen_emit.cpp:1952-1967`），
     弹出的永远是**真 guest 栈那一格**；`structured` 只影响**调用点**，而宿主 `call` 后**必带**
     `emitCallReturnCheck`（`:1857-1879`）：`%r != resume` ⇒ `ret i32 %r` 上抛 ⇒ trampoline 按 `%r` 派发。
     ⇒ 判据错了最多**多一次派发**，不会把错值当返回地址。

### 6.1 修法：三条一起补，⛔ 不要只补 +4

**裁决（2026-11-23）：不修。** 行为已经正确、发射物逐字节等价 ⇒ 收益 0；这是"把证明做对"的整洁性改动，
只在将来真要动 S7 判据（间接 `call` 放宽 / "单元根必须是函数入口"）时一并做。⛔ 唯一不许做的是"只补 +4"。
下面的表留作那时的手册。

| 模型 | 自身通过 | 可结构化 | 宿主 `call` 站点 | 8 份 `.ll` 总字节 |
|---|---:|---:|---:|---:|
| 现状（三条都漏） | 8,600 | 5,343 | 1,572 | 132,744,647 |
| **只补 `leave` 的 +4** | 8,393 | **5,280（−63）** | **1,498（−74）** | 132,729,477 |
| **三条一起补（Intel 正确）** | 8,589（−11） | 5,335（−8） | **1,572（0）** | **132,744,647（相同）** |

⇒ 只补 +4 是**坏改动**（63 个正确单元失格、少 74 个宿主 `call`、IR 少 15,170 B）；三条一起补是
**中性改动**（−8 个单元、零发射差异），价值在**把证明做对**，不在提速。

### 6.2 不随本单收的两件事

1. **"单元根必须是函数入口"**：会把上面 11 个合法共享尾声判死，还牵动一大批 soft root
   ⇒ 要**单独立项、单独读数**，⛔ 不要和 6.1 一起收。
2. 其余四类降级（动态返回 / 可变长栈 / 写返回槽 / 表外 callee）的进一步放宽。

## 7 复现

```bash
# ① 放宽前的生成期读数（在主树跑即可：主树就是这个形态）—— 复现 §4.1 左列
cd apps/emulator-lift
cmake --build --preset windows-x64 2>&1 | tee ../../.tmp/lift-opt/build-01.log
rg '\[S7 criteria\]|\[S7 criteria/site accounts\]' ../../.tmp/lift-opt/build-01.log
rg '\[S7 downgrade\]' ../../.tmp/lift-opt/build-01.log > ../../.tmp/lift-opt/s7-degrades.raw.txt

# ② 放宽后的读数（★ 在 dev 车道 `E:\Projects\amayui-re-dev` 里跑：patch 与实验日志都在它的 .tmp 下）
cd E:/Projects/amayui-re-dev
git apply .tmp/lift-opt/s7-relax.patch
cd apps/emulator-lift
cmake --build out --target emulator-lift-aot 2>&1 | tee ../../.tmp/lift-opt/build-all-s7.log
rg '\[S7 criteria\]' ../../.tmp/lift-opt/build-all-s7.log
cd ../.. && git apply -R .tmp/lift-opt/s7-relax.patch        # 收工：回退

# ③ 运行期 30 帧指纹（从仓根起；每趟新建空 appdata）
cd apps/emulator-lift
out/emulator-lift-aot.exe --present window --present-hidden --skip-logo --audio off \
    --max-frames 30 --game-dir ..\..\links\install --appdata ..\..\.tmp\lift-opt\appdata-mf-1

# ④ boundary ns/派发（窗口档，5 趟）
pwsh -File ../../.tmp/lift-opt/measure.ps1 -Tag s7-1

# ⑤ §6 的 esp 模型三趟（base / 只补 leave+4 / 三条一起补）—— 探针是**临时的**，收工要还原
#    改动点：lift_struct.cpp 的 Leave 分支（esp = ebp → ebp+4）与 `if (isEbp32)` 两个分支
#    （push ebp 要 esp −4、pop ebp 要 esp +4；现状两处都只把 ebp 置未知）。
#    每趟只改 `[S7 criteria]` 与生成物，跑完 diff 两趟的 out/aot/lift_aot.part*.ll：
cmake --build out --target emulator-lift-gen 2>&1 | Tee-Object ..\..\.tmp\leave-model\full.log
#    判据：三处一起补 ⇒ 8 份 .ll 与 base **逐字节只差 header 两处计数器**（总字节相同）；
#    只补 +4 ⇒ host-call 1,572→1,498、IR 少 15,170 B（⇒ 坏改动）。
```

> ③ 的 flag 按本轮同一套口径写（日志自证 `frames : 30 … 本臂 --max-frames=30` 与 `present … window=hidden`）；
> 二进制不回显 argv。④ 的 `measure.ps1` 采集的是 **boot→TITLE** 窗口，不是 FELLOW 切换帧
> （`saves/` 多了 SAVE10 之后旧配方点空，报告 §0）。
> `.tmp/lift-opt/**` 与 `.tmp/leave-model/**` 是本地实验记录（gitignored）：patch、清单与日志都**不是入库件**。
