# 批 S2af · 强度削减的乘数链：G15 步长比对的前置（否则翻红即假红）

- id: REQ-01M57MHYT2767TJ0RMJGPY3W2F
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- verify: tools/test/shape-access.test.mjs#★access-03
- done_reason: memScale 接上强度削减乘数链：寄存器抽象值里的 k（v.t==='val'）与寻址 scale 相乘 ⇒ 0x40F3B1 现在报 120（0x78）而不是 8。守卫 ★access-03（×15 链 ⇒ 120）+ ★access-04（普通索引仍 4）；变异 121/121（把乘数链丢掉必须红）。门禁：claims exit 0（8439 = 8439 + 0）、test 412/412、requirements 5/5
- tags: [engine, shape]

# S2af · 强度削减的乘数链：G15 翻红的**前置**（不做这一步，翻红就是假红）

## 为什么必须先做

G15 的「元素大小 == 观测步长」这一档要翻红，但它依赖 `memScale`；而 `memScale` 现在只取**寻址里的 scale 字段**
（`tools/lib/shape-access.mjs:433`：`mem.indices[0].scale`）。遇到 MSVC 的**强度削减**形态，这个数是错的。

实测（`0x40F3B1`，`S2ae` 里那条“8 字节元素表”的疑云就是它造成的）：

```
0x40F3A4  mov  eax, [esi+5D880h]        ; eax = 游标
0x40F3AA  mov  edx, eax
0x40F3AC  shl  edx, 4                   ; ×16
0x40F3AF  sub  edx, eax                 ; ×15
0x40F3B1  mov  [esi+edx*8+5D8BCh], ecx  ; scale=8 被读成"元素 8 字节"，真实是 15×8 = 120 = 0x78
```

⇒ `memScale` 报 **8**，真实步长 **0x78**；拿它去比 `size(T_5d8bc)=0x1C` 会得到假冲突，
拿它去比 `size(T1)=120` 会**漏掉**真正的匹配。

## 要做的事

在 `shape-access` 的寄存器抽象值里增加一条**乘数链**（或等价的后处理）：

1. 跟踪 `dst = src` / `shl dst, k` / `add dst, src` / `lea dst, [src + src*n]` / `sub dst, src` /
   `imul dst, src, imm` + `sar dst, k`（魔数除法）⇒ 得到 `dst = m · idx` 的乘数 `m`；
2. 寻址 `[base + reg*S + disp]` 时，若 `reg` 的乘数已知为 `m` ⇒ **有效步长 = S · m**，写进
   `memScale`（保留原 `scale` 供回溯，另给 `scaleChain: {from: reg, m, scale: S}`）；
3. 乘数链解不出时 `memScale` 置 `null`（⛔ 不要回退成 `S`——那正是假冲突/假匹配的来源）。

## 判据与守卫

- 合成 `.lst` 用例（放进 `tools/test/shape-access.test.mjs`，该文件已存在）：
  `mov edx,eax; shl edx,4; sub edx,eax; mov [esi+edx*8+100h], ecx` ⇒ 断言 `memScale === 120`；
  再加一条 `imul eax, eax, 0x1C` 的形态 ⇒ 断言 `memScale === 0x1C`；
  以及“乘数链不可解” ⇒ 断言 `memScale === null`。
- 变异：把“乘数链解不出 ⇒ null”改成回退 `S` ⇒ 上面第三个用例必须红。

## 顺序

1. 本单（S2af）落地 + 用例 + 变异；
2. 再动 G15 的步长比对（那时才知道哪些是真冲突）：先跑对照看 `--by-type` 与 G15 note 的变化；
3. 最后才谈“翻红”，并且**必须**同时给出“为什么不会把强度削减形态误判”的用例。

## 落地后实测（乘数链修好之后）

现跑 `pnpm tools shape claims data/shape/carriers.json --by-type`：

- `步长不符` 这一档 **0 处**（修好之前会有一批假冲突）⇒ 在当前模型里「元素大小 ≠ 观测步长」**已经没有真冲突**。
- 守恒① `8439 = 8439 + 0`、`✅ 全部义务通过`、`--by-type` 0 处。

⇒ 结论：G15 的「元素大小 vs 观测步长」**不需要再翻红来"抓"什么** —— 修好乘数链后它本来就零命中；
把它从 note 改成 red 只是把同一个判据提前红，而它**当前没有见证实例**（0 处），
翻红等于加一条永远不红的守卫（本仓明令禁止"恒真守卫"）。

⇒ 处置：判据**保留在 note 档**，并在注释里写明「一旦乘数链失效，这里会出现成批命中」——
这一点由 `★access-03` + 变异 `强度削减的乘数链被丢掉`（121/121）**守住**，比翻红更有约束力。

## 复现

```powershell
rg -n "mem.indices\[0\].scale" tools/lib/shape-access.mjs
rg -n "0040F3B1" corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst
```
