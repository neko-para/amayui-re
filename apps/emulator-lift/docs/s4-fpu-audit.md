# S4 逐项复核：x87 / MMX / SSE 的实际缺口

> **本文件是事实表**（形态 → 状态），不是进度表：它回答"每条形态现在是什么状态、被哪条守卫钉住、
> 没实现的去了哪"。做到哪一步看 `pnpm tools requirements plan`。
> 上游判据：[`plan-m1-cpu-base.md`](plan-m1-cpu-base.md) §6 S4 · 已批准偏差：[`../SCOPE.md`](../SCOPE.md) §1。
> 复核结论的锚 = **守卫用例 id**（`tests/conformance_cases.cpp`）；语料条数**现查**，方法见 §1。

## 0. 一句话结论

`.lst` 里 S4 的形态**全部有实现**：x87 **47 个形态 / 18,668 条**、MMX **19 个形态 / 950 条**，两条都不存在
"执行到就响亮失败"的形态。真正的缺口在别处 —— **复核过程修掉了 4 处静默偏差**（§3），
补了 **29 条此前不存在的守卫**（§4），并把"语料里没有、但别名表里登记了"的 14 条 SSE 浮点形态
排成了实现单（§5）。

## 1. 语料条数怎么现查（**不要抄本文件的数字，它们会随语料变**）

```bash
# ① 家族级总数 + 指令行数（口径 = tools/lift.mjs 的 X87 / MMX_SSE 集合）
pnpm tools lift corpus-report

# ② 逐形态条数（把 corpusReport() 的 mnemonics 按家族过滤即可；本文件的两个数字就是它算的）
node -e "import('./tools/lift.mjs').then(m=>{const r=m.corpusReport();for(const x of r.mnemonics)console.log(x.mnemonic,x.count)})"
```

★ **`corpus-report` 的两个数是下界，不是全集**：`tools/lift.mjs` 的 `MMX_SSE` 集合漏了
`psrlw` / `psllw` / `packuswb` / `psubsw` / `paddusw`（合计 184 条），`X87` 集合漏了 `wait`（7 条）。
⇒ 报出来的 `MMX/SSE = 778` 实际是 **950**（19 个形态），`x87 = 18,668` 加上 `wait` 是 **18,675**。
本文件按**修正后的集合**列表，并在对应行注明。修 `tools/lift.mjs` 是另一个文件的事（本单的边界外）。

★ **`movsd` 的 12 条不是 SSE2**：`.lst` 里 `movsd` 共 279 条 = `rep movsd` 267 + 裸 `movsd` 12，
两者都是**串操作**（`0x5074CD` 一带，无操作数）。SSE2 标量 double 的 `movsd xmm, xmm` 在本语料里 **0 条**。
⇒ SSE 家族在 `.lst` 里是 **0 个形态 / 0 条**（判据：全文件没有一处 `xmm<数字>` 寄存器操作数）。

## 2. 形态 → 状态表

**状态只有四种**：`已实现（逐位夹具）` / `已实现（有登记偏差）` / `未实现（响亮失败）` / `未取证`。
"有登记偏差"= 偏差写在 `SCOPE.md` §1（本文件不重复登记）；其余"已实现"都必须有一条**改坏就红**的夹具，
`守卫用例 id` 列写的就是它（名字 = `tests/conformance_cases.cpp` 里 `kCases` 的第一列）。

### 2.1 x87（47 个形态 / 18,668 条）

| 形态 | 语料条数 | 状态 | 守卫用例 id | 备注 |
|---|---:|---|---|---|
| `fstp` | 4735 | 已实现（逐位夹具） | `x87-fld-fstp` · `x87-fstp-st-i` | 内存 4105 / `st` 409 / `st(i)` 221；`st(i)` 形态由 `x87-fstp-st-i` 单独钉住 |
| `fld` | 4514 | 已实现（逐位夹具） | `x87-fld-fstp` · `x87-fild16` | 内存 3971 / `st` 372 / `st(i)` 171 |
| `fmul` | 1630 | 已实现（逐位夹具） | `x87-fmul` · `x87-fmulp` | 内存 798 / `st, st(i)` 755（方向由 `x87-fsub-st-sti` 同构覆盖） |
| `fst` | 1558 | 已实现（逐位夹具） | `x87-fst` | **不弹栈**；与 `fstp` 的唯一区别就是这条 |
| `fadd` | 1055 | 已实现（逐位夹具） | `x87-fadd` · `x87-faddp` | 内存 797 / `st, st(i)` 191 / `st(i), st` 48 |
| `fild` | 934 | 已实现（逐位夹具） | `x87-fild16` · `x87-fild64` · `x87-fild-fistp` | 16 / 32 / 64 位三种整数宽度都走 `loadIntMem` |
| `fxch` | 707 | 已实现（逐位夹具） | `x87-fxch` | 707 条全是 `st(i)` |
| `fsub` | 525 | 已实现（逐位夹具） | `x87-fsub` · `x87-fsub-st-sti` | 内存 372 / `st, st(i)` 100 / `st(i), st` 53（两个方向都有守卫） |
| `faddp` | 430 | 已实现（逐位夹具） | `x87-faddp` | 430 条全是 `st(i), st`；夹具用 `st(2)` 钉住下标处理 |
| `fnstsw` | 362 | 已实现（逐位夹具） | `x87-fcom-fnstsw` · `x87-fstsw` | 362 条全是 `ax` 形态 |
| `fldz` | 294 | 已实现（逐位夹具） | `x87-fldz` | 按 10 字节位模式钉住（+0，不是伪零） |
| `fmulp` | 283 | 已实现（逐位夹具） | `x87-fmulp` | 283 条全是 `st(i), st` |
| `fdiv` | 207 | 已实现（逐位夹具） | `x87-fdiv` | 内存 187 / 寄存器 20 |
| `fld1` | 185 | 已实现（逐位夹具） | `x87-fld1` | |
| `fsubp` | 155 | 已实现（逐位夹具） | `x87-fsubp` | 155 条全是 `st(i), st` |
| `fcom` | 147 | 已实现（逐位夹具） | `x87-fcom-less` · `x87-fcom-unordered` · `x87-fcom-fnstsw` | `st(i)` 123 / 内存 24 |
| `fcomp` | 129 | 已实现（逐位夹具） | `x87-fcomp` | `st(i)` 105 / 内存 24；内存形态与 `fcom` 共用 `loadMemValue` |
| `fldcw` | 94 | 已实现（逐位夹具） | `x87-fldcw-roundtrip` | 94 条全是内存 |
| `fsubrp` | 82 | 已实现（逐位夹具） | `x87-fsubrp` | 反向减法；与 `fsubp` 的差是**操作数方向** |
| `fiadd` | 72 | 已实现（逐位夹具） | `x87-fiadd` | `fi` 族 = 唯一要 `i32 → extF80` 的一族 |
| `fchs` | 61 | 已实现（逐位夹具） | `x87-fchs` | |
| `fidiv` | 59 | 已实现（逐位夹具） | `x87-fidiv` | |
| `fsubr` | 59 | 已实现（逐位夹具） | `x87-fsubr` | 内存 36 / `st, st(i)` 23 |
| `fcompp` | 56 | 已实现（逐位夹具） | `x87-fcompp` | |
| `fimul` | 50 | 已实现（逐位夹具） | `x87-fimul` | |
| `fistp` | 50 | 已实现（逐位夹具） | `x87-fistp64` · `x87-fistp-half` | `fistp-half` 钉住 .5 边界的舍入方向 |
| `fnstcw` | 48 | 已实现（逐位夹具） | `x87-fstcw` | `.lst` 里写 `fnstcw`；`9B` 前缀形态由 `x87-fstcw` 覆盖合并逻辑 |
| `fdivp` | 42 | 已实现（逐位夹具） | `x87-fdivp` | |
| `fabs` | 31 | 已实现（逐位夹具） | `x87-fabs` | |
| `fdivr` | 29 | 已实现（逐位夹具） | `x87-fdivr` | 内存 25 / `st, st(i)` 4 |
| `fdivrp` | 28 | 已实现（逐位夹具） | `x87-fdivrp` | |
| `fucompp` | 18 | 已实现（逐位夹具） | `x87-fucompp` | |
| `fucomp` | 11 | 已实现（逐位夹具） | `x87-fucomp` | ★ 修复前它是**静默算错**（见 §3.1） |
| `fisub` | 8 | 已实现（逐位夹具） | `x87-fisub` | |
| `fyl2x` | 3 | 已实现（有登记偏差） | `x87-fyl2x` | SCOPE §1「超越函数」：宿主 libm，1 ulp |
| `fidivr` | 2 | 已实现（逐位夹具） | `x87-fidivr` | |
| `fcos` | 2 | 已实现（有登记偏差） | `x87-fcos` | SCOPE §1：1 ulp |
| `fpatan` | 2 | 已实现（有登记偏差） | `x87-fpatan` | SCOPE §1：1 ulp |
| `fstsw` | 2 | 已实现（逐位夹具） | `x87-fstsw` | `ax` 1 / 内存 1；`9B` 前缀合并成一条 |
| `fldln2` | 2 | 已实现（逐位夹具） | `x87-fldln2` | ★ 修复前低 11 位恒为 0（见 §3.3） |
| `fucom` | 1 | 已实现（逐位夹具） | `x87-fucom` | |
| `fsin` | 1 | 已实现（有登记偏差） | `x87-fsin` | SCOPE §1：1 ulp |
| `fprem` | 1 | 已实现（逐位夹具） | `x87-fprem` | ★ C2=1（商 ≥ 2^62）的分支未取证，见 §6.2 |
| `fldl2e` | 1 | 已实现（逐位夹具） | `x87-fldl2e` | ★ 修复前低 11 位恒为 0（见 §3.3） |
| `fscale` | 1 | 已实现（有登记偏差） | `x87-fscale` | SCOPE §1：指数经 double 取整 |
| `ftst` | 1 | 已实现（逐位夹具） | `x87-ftst` | |
| `fldlg2` | 1 | 已实现（逐位夹具） | `x87-fldlg2` | ★ 修复前低 11 位恒为 0（见 §3.3） |
| **`wait`** | **7** | 已实现（逐位夹具） | `x87-fclex-wait` | 不在 `corpus-report` 的 X87 集合里（那是 47 的由来）。语义 = no-op（见 §6.2） |

★ **表外但已实现且有用例的 x87 形态**（语料里 0 条）：`fsqrt` `frndint` `fxam` `fxtract` `fsincos`
`fptan` `fyl2xp1` `f2xm1` `fldpi` `fldl2t` `fcomi` `fcomip` `fucomi` `fucomip` `fcmovb` `fcmove` `fcmovbe`
`fcmovu` `fcmovnb` `fcmovne` `fcmovnbe` `fcmovnu` `fisttp` `fist` `ffree` `fdecstp` `fincstp` `fnop`
`fninit` `finit` `fclex` `fnclex` `fstcw` `fldenv` `fnstenv` `fnsave` `frstor` `fsave`。
其中 `fsincos` / `fptan` / `fyl2xp1` / `f2xm1` 属 SCOPE §1 的 1 ulp 偏差族。

### 2.2 MMX（19 个形态 / 950 条）

| 形态 | 语料条数 | 状态 | 守卫用例 id | 备注 |
|---|---:|---|---|---|
| `movq` | 198 | 已实现（逐位夹具） | `mmx-movq` | `mm,mm` 104 / `mm,[mem]` 72 / `[mem],mm` 22 |
| `pand` | 102 | 已实现（逐位夹具） | `mmx-pand` | |
| `psrlw` | 94 | 已实现（逐位夹具） | `mmx-psrlw` | `mm,imm` 84 / `mm,mm` 10；`corpus-report` 的集合漏了这条 |
| `movd` | 78 | 已实现（逐位夹具） | `mmx-movd` · `sse-movd-xmm` | 写进 mm/xmm 时**零扩展**；本语料 276 条 `movd/movq` 全是 mm/GPR 形态 |
| `paddw` | 74 | 已实现（逐位夹具） | `mmx-paddw` | |
| `pmullw` | 62 | 已实现（逐位夹具） | `mmx-pmullw` | |
| `psllw` | 60 | 已实现（逐位夹具） | `mmx-psllw` · `mmx-psllw-count-256` | ★ 计数 ≥ 256 是**静默算错**（见 §3.2） |
| `psraw` | 50 | 已实现（逐位夹具） | `mmx-psraw` | 计数 > 15 ⇒ 整 lane 填符号位 |
| `punpcklbw` | 42 | 已实现（逐位夹具） | `mmx-punpcklbw` | |
| `pxor` | 40 | 已实现（逐位夹具） | `mmx-pxor` | |
| `por` | 38 | 已实现（逐位夹具） | `mmx-por` | |
| `psubw` | 36 | 已实现（逐位夹具） | `mmx-psubw` | |
| `emms` | 28 | 已实现（逐位夹具） | `mmx-emms` | tag word 全 empty、TOP 不动 |
| `packuswb` | 14 | 已实现（逐位夹具） | `mmx-packuswb` | 集合漏了这条 |
| `psubsw` | 8 | 已实现（逐位夹具） | `mmx-psubsw` | 集合漏了这条 |
| `paddusw` | 8 | 已实现（逐位夹具） | `mmx-paddusw` | 集合漏了这条（§1 的 184 条之一） |
| `pcmpeqw` | 6 | 已实现（逐位夹具） | `mmx-pcmpeqw` | |
| `pandn` | 6 | 已实现（逐位夹具） | `mmx-pandn` | |
| `psrlq` | 6 | 已实现（逐位夹具） | `mmx-psrlq` | |

★ **MMX 的架构约束**：`MM0..MM7` 别名的是 x87 的**固定物理寄存器 R0..R7**，与 `TOP` 无关
（Intel SDM Vol.1 §9.2）⇒ 必须走 `fpu.regs[i]` 而**不是** `fpu.regs[phys(TOP+i)]`。修复前这里错了（§3.4）。

### 2.3 SSE（`.lst` 里 0 个形态 / 0 条）

★ **2026-11-09 后续**：下表是**审计当时**的状态快照。这 14 条现已按 Intel SDM 实现（`Op::Sse` +
`execSse()`：标量高位语义 / `comiss`-`ucomiss` 的 EFLAGS / NaN 与 MXCSR.IE），守卫 = `conformance`
的 24 条 `sse-*`（`ctest -R conformance`）。"未实现（响亮失败）"一列**不再成立**；本表保留为历史。

`.lst` **没有任何 SSE 浮点形态**（判据：全文件 `xmm<数字>` 操作数 = 0）。但别名表
（`src/runtime/mnemonics.cpp`）为了 A2 的可读性登记了下面这些拼写；它们在解码器里**没有** `Op`
⇒ 执行到就是 `unimplemented()`（**响亮失败**，打印 EA + 助记符 + 字节 + 寄存器 + guest 返回栈后停机），
**不是静默 no-op**。语料里没有 ⇒ 今天撞不到；`1,805` 个 `COLLAPSED` 区是未解码字节
（判据：`pnpm tools lift corpus-report` 的 `collapsed 行`），所以"`.lst` 里 0 条"**不等于**"二进制里没有"。

| 形态 | 语料条数 | 状态 | 守卫用例 id | 备注 |
|---|---:|---|---|---|
| `movaps` | 0 | 未实现（响亮失败） | — | `S4-SSE` 子单 |
| `movups` | 0 | 未实现（响亮失败） | — | 同上 |
| `movss` | 0 | 未实现（响亮失败） | — | 同上 |
| `movdqa` | 0 | 未实现（响亮失败） | — | 同上 |
| `movdqu` | 0 | 未实现（响亮失败） | — | 同上 |
| `xorps` | 0 | 未实现（响亮失败） | — | 同上 |
| `cvtsi2ss` | 0 | 未实现（响亮失败） | — | 同上 |
| `cvttss2si` | 0 | 未实现（响亮失败） | — | 同上 |
| `addss` | 0 | 未实现（响亮失败） | — | 同上 |
| `subss` | 0 | 未实现（响亮失败） | — | 同上 |
| `mulss` | 0 | 未实现（响亮失败） | — | 同上 |
| `divss` | 0 | 未实现（响亮失败） | — | 同上 |
| `comiss` | 0 | 未实现（响亮失败） | — | 同上 |
| `ucomiss` | 0 | 未实现（响亮失败） | — | 同上 |

★ 同一类还有 **x87 的非语料拼写**：`fbld` `fbstp` `ficom` `ficomp` `feni` `fdisi` `fsetpm` `ffreep`
—— 解码器没登记 ⇒ `Op::Other` ⇒ 同样响亮失败。语料 0 条，**不排单**（真撞到再按"实现指令"流程走）。

## 3. 修掉的真 bug（3 处静默算错 + 1 处精度偏差）

★ 判据：**静默算错比"未实现"危险** —— 未实现会停在诊断上，算错会把错误数据带进后续几百帧。
四处都是"两条路径都自洽"型的错误，**只有专门构造的夹具能看见**；四处都配了回归夹具，并实测
"改坏 ⇒ 指定用例当场红"（§4.2 的变异记录）。

### 3.1 `exec_fpu.cpp` · `compare()` —— `fucom` / `fucomp` 永远比成"相等"

- **现象**：`fcom` / `fcomp` / `fucom` / `fucomp` 的**操作数形态不统一**（实测 Zydis）：
  `fcom st(i)` 给**一个**操作数（源就是它），`fucom st(i)` / `fucomp st(i)` 给**两个**（`st0, st(i)`，
  隐含的 ST(0) 也列出来），`fcompp` / `fucompp` 给**零个**。旧实现一律取 `op0` 当源
  ⇒ 对 `fucom`/`fucomp` 拿到的是 **ST(0) 自己** ⇒ `ST0 ? ST0` 恒等 ⇒ 条件码永远是 C3=1（"相等"）。
- **影响面**：语料 `fucomp` 11 条 + `fucom` 1 条。两条都是"用 `fucom` 做 quiet 比较、按 C0/C3 分支"的代码。
- **修法**：先看 `op1` 是不是寄存器（两操作数形态），再退回 `op0`，最后才是"无操作数 ⇒ ST(1)"。
- **守卫**：`x87-fucomp`（改坏 ⇒ 红：`expected 0x00000100, got 0x00004000`）。`x87-fucom` 同时补上。

### 3.2 `exec_fpu.cpp` · `execPacked()` —— MMX 移位计数被 `& 0xFF` 截断

- **现象**：`psllw/pslld/psllq/psrlw/…/psrad` 的**寄存器/内存**形态，计数是**完整的 64 位值**
  （Intel SDM Vol.2 对各条都写 "If the source operand is a register or a 64-bit memory operand, the full
  64-bit value is used"）；只有 `imm8` 形态才是 8 位。旧实现先把计数 `& 0xFF`，于是计数 **256** 被读成 **0**
  ⇒ 本该"整 lane 清 0"的地方变成"原样不动"。
- **影响面**：任何计数 ≥ 256 的寄存器前缀（游戏里典型是 `psllw mm, mm` 用循环变量当计数）。
- **修法**：寄存器/内存形态取完整 64 位（饱和到 `uint32`，判据仍是 `> 15 / > 31 / > 63`）。
- **守卫**：`mmx-psllw-count-256`（改坏 ⇒ 红：`expected 0x00000000, got 0xFFFFFFFF`）。

### 3.3 `fpu_bridge.cpp` —— 5 个超越常数只有 double 精度

- **现象**：`FLD1/FLDZ` 之外的常数（`FLDPI` / `FLDL2E` / `FLDL2T` / `FLDLG2` / `FLDLN2`）在 x87 上压的是
  **64 位有效位**的扩展精度常数（Intel SDM Vol.2 对每条都写明"按扩展精度四舍五入到 64 位"）。
  旧实现 `f64_to_extF80(double 字面量)` ⇒ 低 **11 位恒为 0**，与真机**逐位不同**。
- **影响面**：语料 `fldln2` 2 · `fldl2e` 1 · `fldlg2` 1（`fldpi` / `fldl2t` 0 条，但同一段代码）。
  相对误差 ~2⁻⁶⁴，肉眼不可见 —— 但"其余走 SoftFloat 逐位"这句话就不成立了。
- **修法**：直接写 64 位有效位的位模式。值不是抄来的：用 BigInt 定点（2⁻⁴⁰⁰）按
  `π = 16·atan(1/5) − 4·atan(1/239)`（Machin）· `ln2 = 2·atanh(1/3)` · `ln5 = 2·ln2 + 2·atanh(1/9)` ·
  `log2(e) = 1/ln2` · `log10(2) = ln2/ln10` · `log2(10) = ln10/ln2` 独立算到 64 位有效位（最近偶数）。
  10 字节位模式（内存序 = 低 8 字节尾数 + 2 字节符号/指数）：

  | 常数 | 10 字节（十六进制，内存序） |
  |---|---|
  | `FLDPI` | `35 C2 68 21 A2 DA 0F C9 00 40` |
  | `FLDL2E` | `BC F0 17 5C 29 3B AA B8 FF 3F` |
  | `FLDL2T` | `FE 8A 1B CD 4B 78 9A D4 00 40` |
  | `FLDLG2` | `99 F7 CF FB 84 9A 20 9A FD 3F` |
  | `FLDLN2` | `AC 79 CF D1 F7 17 72 B1 FE 3F` |

- **守卫**：`x87-fldpi` `x87-fldl2e` `x87-fldl2t` `x87-fldlg2` `x87-fldln2`（改坏 ⇒ 红：
  `expected 0xD1CF79AC, got 0xD1CF7800`）。

### 3.4 `exec_fpu.cpp` · `pkLoad` / `pkStore` —— MMX 寄存器被 x87 的 TOP 旋转了

- **现象**：MMX 读写用的是 `fpu.regs[cpu.fpu.phys(i)]`（`phys(i) = (TOP+i)&7`）。`phys()` 是 **x87 栈**的
  旋转，而 `MM0..MM7` 别名的是**固定的物理寄存器 R0..R7**，与 TOP 无关（Intel SDM Vol.1 §9.2）。
  ⇒ 只要 `TOP != 0`，MMX 的读写就落到别的槽上。
- **为什么以前没被发现**：`mmx-*` 夹具全部从 `TOP=0` 开始（此时 `phys(i)==i`），而且**同一条
  `phys()` 既写又读**会自洽地掩盖它 —— 这是"两条路径都自洽"型错误的典型。
- **影响面**：游戏里 MMX 代码与 x87 代码交替出现（`emms` 只清 tag word，**不动 TOP**）⇒ 只要进入
  MMX 那段时 TOP 不是 0（x87 栈很少恰好弹空到 0），后续 `movq` / `paddw` 全在错的槽上。
- **修法**：`regs[i]`（物理槽 = 寄存器号）。tag word 仍然不动（Intel：MMX 不影响 x87 tag）。
- **守卫**：`mmx-top-rotated`（改坏 ⇒ 红：`expected 0x00000000, got 0x3F800000`）。
  ★ 这条夹具踩过一个坑，值得记下来：一开始用 `fstp dword` 读回那个槽，但 MMX 的 8 字节当 ext80 是个
  **次正规**，转 f32 会下溢成 0 —— 两条路径都是 0，夹具变成恒等的。改成 `fstp tbyte`（10 字节原样搬）才判得出来。

## 4. 本轮补的守卫（29 条）与变异记录

### 4.1 补了哪 29 条

`lift-conformance` 从 183 条变成 212 条（新增 29 条，全在 `tests/conformance_cases.cpp`）：

- **23 条**给"语料里有、实现里有、但此前一条夹具都没有"的 x87 形态：
  `fst` `faddp` `fmulp` `fsubp` `fsubrp` `fdivp` `fdivrp` `fsubr` `fdivr` `fiadd` `fisub` `fisubr`
  `fimul` `fidiv` `fidivr` `fcomp` `fucom` `fucomp` `fld1` `fldz` `fldlg2` `fldln2` `fldl2e`；
- **2 条**给语料里 0 条、但同一段代码路径的常数：`fldpi` `fldl2t`；
- **2 条**给"此前只有内存形态被钉住"的高频寄存器形态：`x87-fstp-st-i`（630 条）· `x87-fsub-st-sti`（100 条）；
- **2 条**给 §3.2 / §3.4 的静默算错：`mmx-psllw-count-256` · `mmx-top-rotated`
  （§3.1 与 §3.3 的回归分别由 `x87-fucomp` 与 `x87-fldln2` 承担，它们同时是形态守卫）。

★ 为什么先补这几个：`.lst` 的形态频次就是"渲染 / 物理路径撞到的概率"的代理 ——
`fst` 1558 / `faddp` 430 / `fmulp` 283 / `fsubp` 155 排在最前面，而它们此前**一条夹具都没有**。
`boot_trace` 只断言"到达 L2 + 两次运行逐字节相同"，**不按助记符统计覆盖**，所以启动路径本身给不出
"哪条形态被撞到"的证据（plan §4 D3：走到 `_WinMain` 的路径几乎不碰 x87）；本表因此以**语料频次**排序。

### 4.2 变异验证（"改坏 ⇒ 指定用例当场红"）

做法：施加一处破坏 → 重编 `exec_fpu.cpp` / `fpu_bridge.cpp` → 重新归档 + 链接 → 跑 `lift-conformance`
→ **把源文件逐字节写回**（`finally` 里比对全文）→ 重编回绿。

| # | 破坏 | 变红的用例 | 失败信息 |
|---|---|---|---|
| A | `pkStore` 退回 `regs[phys(sp.index)]` | `mmx-top-rotated` | `expected 0x00000000, got 0x3F800000` |
| B | `compare()` 退回"只看 op0" | `x87-fucomp` | `expected 0x00000100, got 0x00004000` |
| C | MMX 移位计数退回 `& 0xFF` | `mmx-psllw-count-256` | `expected 0x00000000, got 0xFFFFFFFF` ×2 |
| D | `FLDLN2` 退回 double 字面量 | `x87-fldln2` | `expected 0xD1CF79AC, got 0xD1CF7800` |

每次复原后 `lift-conformance` 回到 `212 cases / 441 checks / 0 failures`。

## 5. 缺口去了哪

| 缺口 | 处置 |
|---|---|
| 14 条 SSE 浮点形态（语料 0 条，别名表里有） | 子单（`S4-SSE`），见 §5.1 |
| `fbld` / `fbstp` / `ficom` / `ficomp` / `feni` / `fdisi` / `fsetpm` / `ffreep`（语料 0 条、解码器没登记） | **不排单**：真撞到会响亮失败并停下，按"实现指令"流程走 |
| `fprem` 的 C2=1 分支（商 ≥ 2^62） | 记录在 §6.2，不排单（语料 1 条且不在启动路径上） |
| `tools/lift.mjs` 的 `X87` / `MMX_SSE` 集合漏了 5 + 1 个助记符 | §1 已注明；工具是别的文件的边界，本单不改 |

### 5.1 新建子单

见本单正文（`data/requirements/01M6K7PQK23EGJDJBQ6XKKE4HR.md`）里登记的 id 与标题。

## 6. 没能核完的 / 缺什么证据

### 6.1 形态级：0 条 `未取证`

47 个 x87 + 19 个 MMX 形态**每一个**都有：实现路径（读代码逐条确认落到哪个 `Op` / `PkOp`）+
至少一条会红的夹具（`变异` 或"构造上必然依赖"）。SSE 的 14 条是**确定未实现**（解码器里没有 `Op`，
`execInsn` 的 `Op::Other` 分支会 `unimplemented()` 停机），不是"未取证"。

### 6.2 非形态级的证据缺口（**没核完的**）

1. **`wait` 的 trap 语义**：真机上 `WAIT` 在有**未屏蔽**的挂起 x87 异常时触发 `#MF`。本模型里 x87 异常
   只置状态字、**从不 trap** ⇒ `WAIT` 是 no-op。这是"模型内等价"，不是"与真机等价"；没有真机 trace
   可证伪。**缺的证据**：一条能观测到挂起异常被 `WAIT` 触发的场景（本作是否依赖它，未知）。
2. **`fprem` 的 C2=1 分支**：真机的 `FPREM` 是**迭代**的部分余数（指数差 > 63 时置 C2=1 让调用方循环）。
   我们一次算完，C2 由 `|q| ≥ 2^62` 判出来。**缺的证据**：一条指数差 > 63 的真实用例
   （语料里 `fprem` 只有 1 条，且不在启动路径上）。
3. **`fscale` 的指数宽度**：指数经 `double` 取整（SCOPE §1 已登记）。`|e| > 2^53` 才有差 —— 实际撞不到，
   但"撞不到"来自推理，不是观测。
4. **SoftFloat 的 pseudo-denormal / unnormal**（plan §4 D3 的已知边界）：x87 寄存器里出现非规范编码时
   我们不复现真机的历史怪癖。**缺的证据**：语料里是否存在会产出这种编码的路径（未查）。
5. **`fst`/`fstp` 到 `tbyte` 且源是非规范编码**：没有夹具，也没有语料证据。
6. **`COLLAPSED` 区里的 SSE**：`1,805` 个 collapsed 区（`_memcpy` / `_memset` / `_rand` 等 CRT）
   在 `.lst` 里没有指令行。它们**都是 CRT**（plan §3 F2），理论上不会是 SSE 浮点，但这是**推理**，
   不是观测 —— "SSE 在二进制里 0 条"因此**没有**被证明。
7. **`fcom` / `fcomp` 的内存形态**（各 24 条）：没有专门的夹具，靠 `fadd`/`fsub` 的内存形态
   共用 `loadMemValue` 这条路径覆盖。**缺的证据**：一条 `fcom dword ptr [...]` 的夹具。

## 7. 怎么重算本文件的每一条

| 想知道 | 怎么查 |
|---|---|
| 某个形态的语料条数 | §1 的两条命令 |
| 某个形态有没有实现 | `rg -n '"(f|p)[a-z0-9]*", Op::' src/runtime/decode.cpp`（x87）/ `kPkMap`（MMX）；没有 = `Op::Other` = 响亮失败 |
| 某个形态有没有夹具 | `rg -n 'x87-<名>|<名>' tests/conformance_cases.cpp` 的 `kCases` 表 |
| 某条夹具会不会红 | §4.2 的做法（改坏 → 重编 → `lift-conformance` → 逐字节复原） |
| 未实现会不会静默 | `src/runtime/exec.cpp` 的 `unimplemented()`：打印诊断、`cpu.running=false`、**不返回成功** |
