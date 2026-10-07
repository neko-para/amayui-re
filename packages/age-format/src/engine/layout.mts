/**
 * packages/age-format/src/engine/layout.mjs —— **引擎镜像布局的观察记录**（不是模拟器的一部分）
 *
 * ## 这个文件是什么
 * 它是"**这份 `AGE.EXE__dumped.sectfix.EXE` 里，引擎把东西放在哪**"的**观察记录**：
 * 绝对地址（`Engine+0x5D880`）、帧内偏移、池的计数/基址槽、EA 出处。
 *
 * ## ★ 为什么它**不在** `apps/emulator` 里
 * 模拟器是"按建模后的语义**重新实现**"的东西 —— 它要回答的是
 * "有哪些槽位、谁装得下什么、怎么编码、越界怎么办"。
 * 而"某个槽在第几号字节 / 哪条 `mov` 碰过它"是**逆向知识**（`AGENTS.md` §6 的知识层）：
 * 它随**这一份反汇编导出**而变，换一份镜像/换一次导出就全变。
 * ⇒ 把它放进模拟器，等于让模拟器**依赖镜像布局**，那是把"实现"和"观察"搅在一起。
 *
 * 本模块把两者分开：**这里只放观察到的事实（带 EA）**；
 * 语义（池有哪几个、编码与否、operand type tag）在 `apps/emulator/src/model/`。
 * 两边由 `tools/test/emulator-model.test.mjs` 回语料对账 —— 也就是说：
 * **偏移可以被替换，语义不必跟着动**；而偏移错了，守卫会红。
 *
 * ## 与台账的关系
 * 本文件里每个常量都对应 `data/ledger/` 里的一条观察（`subject` 形如 `Engine+0x5D880/…`）。
 * 台账是**结论的真源**；本文件是那些结论的**可执行形式**（守卫直接拿它回语料复核）。
 */

/** `Engine` 对象的全局池族槽位（绝对值 = `Engine 基址 + 偏移`） */
export const GLOBAL_SLOTS = {
  base: {
    int: 0x5d800, float: 0x5d808, string: 0x5d810,
    intRef: 0x5d818, floatRef: 0x5d820, stringRef: 0x5d828,
  },
  /** `*_alt` 槽（memflip 的第二份缓冲）。★ 语义**未定** ⇒ 只记存在，不实现行为 */
  alt: { int: 0x5d804, float: 0x5d80c, string: 0x5d814, intRef: 0x5d81c, floatRef: 0x5d824, stringRef: 0x5d82c },
  /** 计数槽（★ `pool_string_count` 在旧仓 `fields.json` 里**同名两条**：0x5D7F0 与 0x5D814 ⇒ 只认前者为"计数"） */
  count: { int: 0x5d7e8, float: 0x5d7ec, string: 0x5d7f0 },
};

/** 帧区的布局：`Engine+0x5D880` 起、40 帧、步长 0x78 */
export const FRAME_LAYOUT = {
  base: 0x5d880,
  stride: 0x78,
  count: 40,
  /**
   * 帧内**偏移 → 语义名**。
   * ★ 这是**观察**：每一个都能在语料里找到出处（见文件末尾 `EVIDENCE`；完整普查见 `FRAME_SLOTS_OBSERVED`）。
   */
  off: {
    cur: 0x0,
    /** ★ 不随 `cur` 变化 ⇒ 其实是 Engine 级寄存器（`exit` 路径写它，见台账 `…frame-caller-exit-path`） */
    callerChain: 0x4,
    savedCur: 0x8,
    effectFlags: 0xc,
    valid: 0x10,
    strTable: 0x14,
    operands: 0x18,
    count0: 0x1c,
    base0: 0x34,
    caller: 0x4c,
    frameArg: 0x50,
    triples: [
      { len: 0x54, ptr: 0x58 },
      { len: 0x5c, ptr: 0x60 },
      { len: 0x64, ptr: 0x68 },
    ],
    state6C: 0x6c,
    state70: 0x70,
    /** ★ `operand_count` = `2*argc + 1`（**不是** `arity`） */
    operandCount: 0x74,
    /** `array_container`（`std::vector`：`operator new(0x10)`，3 dword = begin/end/cap） */
    arrayContainer: 0x84,
    /** 三组 (长度, 指针) 的**基址**（脚本缓冲那块） */
    strBase: 0x14,
  },
};

/**
 * 帧区里**机械普查到**的 slot 全集（0x5D880..0x5D908 ⇒ 偏移 0..0x88）。
 * ★ 用**两种寻址形态**归一后得到：
 *   * 形态 A（字面）：`[reg+reg*8+5D8xxh]` / `[reg+5D8xxh]`
 *   * 形态 B（折叠）：`[reg+reg*8]`，偏移来自 `add/lea …,0C79h`（本例命中 `+0x38`）
 * ★ 只认形态 A 会**漏掉折叠的那些**（踩过：于是得出了"`+0x38` 零出现"的错误全称否定）。
 */
export const FRAME_SLOTS_OBSERVED = [
  0x0, 0x4, 0x8, 0xc, 0x10, 0x14, 0x18,
  0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30,
  0x34, 0x38, 0x3c, 0x40, 0x44, 0x48,
  0x4c, 0x50, 0x54, 0x58, 0x5c, 0x60, 0x64, 0x68, 0x6c, 0x70, 0x74,
  0x78, 0x7c, 0x80, 0x84, 0x88,
];

/**
 * **引擎标量数组**：`Engine[base + 索引] = 值` 这种按索引写的槽族。
 *
 * ★ 与 `ENGINE_SCALAR_WRITES` 的差别：那里的槽下标是**指令里写死的**（每个 opcode 一个固定 dword），
 *   这里下标由**操作数**给出 ⇒ 必须把索引记进键名（`Engine.d551`、`Engine.d552`…），
 *   否则两次不同的写入会互相覆盖（而日志看起来一切正常）。
 *
 * ★ `outOfRange: 'skip'` = 引擎**静默跳过**这次写（逐字 `if (result <= 0x1F) …`，**没有**异常）
 *   —— ⛔ 本层照抄"跳过"，但**必须记一笔**（不可见地丢弃一次写是最难查的一类）。
 */
export const ENGINE_SCALAR_ARRAYS = [
  // `0x107`（`sub_421E50`，argc 2）：`v2 = op2; result = op1; if (result <= 0x1F) this[result + 551] = v2;`
  // ★ 实测用法（SYSTEM4#88..#96）：`copy-local-array` 先把脚本里的块填进 `global:int[43457..]`，
  //   紧接着 8 条 `0x107 (imm 0..7) (global-int 43457+i)` 把**那 8 个值**搬进引擎的这张表。
  { name: 'Engine.d551', base: 551, len: 32, opcode: 0x107, handler: 'sub_421E50', indexOperand: 0, valueOperand: 1, maxIndex: 0x1f, outOfRange: 'skip', note: '键槽表（下标由 op1 给出）' },
  // ★ `0x10b`（`sub_422070`，argc 2）：**同族但角色互换** ——
  //   `v2 = op2（索引）; result = op1（值）; if (result <= 0x1F) this[v2 + 1383] = result;`
  //   ⇒ 索引来自 **op2**、范围检查落在 **op1** 上。两处差别（谁当索引 / 哪张表）都写在字段里，
  //   ⛔ 不要用"看起来差不多"把两条 opcode 合并实现。
  { name: 'Engine.d1383', base: 1383, len: 32, opcode: 0x10b, handler: 'sub_422070', indexOperand: 1, valueOperand: 0, maxIndex: 0x1f, outOfRange: 'skip', note: '另一张按索引写的表（下标由 op2 给出）' },
];

/**
 * 各 local 池在帧内的**计数槽 / 基址槽**（观测；与 `apps/emulator` 里的语义池名对应）。
 *
 * ## ★★ 已订正（2026-10）：基址是 `帧+0x3C/0x40/0x44/0x48/0x50/0x54`，**不是** `+0x34…+0x48`
 * 判据（本仓自己取的证，锚 = EA）：`sub_40ED40`（脚本装载器）里**分配紧跟着写基址** ——
 * `local_string` 28B 池 `new[]`@`0x40F365` → 基址写 `帧+0x3C`@`0x40F3B1`；
 * `local_int`@`0x40F2D7` → `帧+0x40`@`0x40F2E9`；`local_float`@`0x40F316` → `帧+0x44`@`0x40F31B`；
 * `local_ptr`@`0x40F3DE` → `帧+0x48`@`0x40F3F0`；`+0x50`@`0x40F41D`；`+0x54`@`0x40F45C`。
 * 读取侧由 `sub_41BF50` 的 case `0xB/9/0xA/0xC/0xD/0xE` 钉住（`[帧+0x3C]`… `[帧+0x54]`）。
 * 旁证：`set-string` 对 type 11（局部串）的目标 = `[Engine+0x5D8BC] + 28*idx`，而 `0x5D8BC − 0x5D880 = 0x3C` ✓
 * —— **这一格新旧两份说法一致**，其余五格是旧表错。
 *
 * ★ 旧表（`+0x34…+0x48`）的来源是**旧仓 `fields.json`**，本仓没有为它找到判据 ⇒
 * 按用户口径「**以本仓重新发现的内容为准**」，此处采用本仓的取证；旧说法记在台账里，不再使用。
 * ★ `count` 那六格（`帧+0x1C..+0x30`）**不变**：新证据同样给出这六格。
 */
export const LOCAL_POOL_SLOTS = [
  { name: 'int', count: 0x1c, base: 0x40 },
  { name: 'float', count: 0x20, base: 0x44 },
  { name: 'string', count: 0x24, base: 0x3c },
  { name: 'ptr', count: 0x28, base: 0x48 },
  { name: 'floatPtr', count: 0x2c, base: 0x50 },
  { name: 'stringPtr', count: 0x30, base: 0x54 },
];

/**
 * 元素宽度里哪些**逐字核实过**（★ 没核实的别当事实）。
 * ★ `string: true`（2026-10 订正）：28 字节 = MSVC `std::string`（`+0` 数据 / `+16` size / `+20` capacity，
 * `cap < 0x10` ⇒ 内联 16 字节），由两条独立路径核实（`sub_433310` 的 `28*idx` 与 `sub_42A420`/`41B640` 的
 * `cmp [elem+0x14],10h`）；本仓另有一处实证（`INITCONFIG0` 的五条内联串解出字体名）。
 */
export const ELEM_BYTES_VERIFIED = { int: true, float: true, ptr: true, floatPtr: true, stringPtr: true, string: true };

/** 常量携带的语料证据（守卫会拿它回语料复核 → EA 锚的"可执行形式"） */
export const EVIDENCE = {
  frameStride: { at: '.text:0041BF64..0041BF69', insns: ['shl edx,4', 'sub edx,eax', 'mov eax,[ecx+edx*8+5D898h]'], note: '15·cur ×8 = 120·cur（两条取操作数原语同形）' },
  localPoolBases: {
    at: '.text:0040F2E9..0040F471',
    insns: ['mov [esi+ecx*8+5D8FCh],eax', 'mov edx,1Ch', 'mul edx', 'operator new[]'],
    note: '装载器 `sub_40ED40` 把 **基址**写进 帧+0x3C/0x40/0x44/0x48/0x50/0x54（分配紧随其后）；★ 旧注写的 "+0x34/…/+0x48" 已订正',
  },
  intPoolBase: { at: '.text:0042AF2F..0042AF35', insns: ['mov edx,[esi+5D800h]', 'lea eax,[edx+ecx*4]'], note: 'operand type 3 的取址：base + idx*4（下标不过编码）' },
  /** ★ int 族的 `key` 在 `Engine+0x5EC8C`（运行期赋值，从不出现在立即数里） */
  codecKey: { at: '.text:00417359', insns: ['mov [esi+5EC8Ch], edx'], note: '全清单唯一的写点' },
  /** ★ `enc_zero` 在 `Engine+0x5EC90` = `ENC(key,0)` ⇒ **非 0** */
  encZero: { at: '.text:00415990', insns: ['mov [esi+5EC90h], eax'], note: 'eax = rol(key,21) = ENC(key,0)' },
};

/**
 * ★★ **引擎标量槽的写入观察** —— 只登记"**哪个 opcode 的 handler 写了哪个 dword 槽、写成什么形态**"，
 *   **语义一律未定**（⛔ 不给含义、不解释用途、不代表它属于哪个子系统）。
 *
 * ## 为什么需要它
 * 启动链前段有一批薄 handler，形状就是"读操作数 → 写引擎的某个 dword"（例：`0x149` 写 `this[97058]`）。
 * 模拟器要跑过去就得**把值存下来**：否则后面若有分支读同一个槽，我们会**静默走错**（不报错，只是路径不同）。
 * 而 `apps/emulator` 里**不许出现偏移**（本仓硬口径）⇒ 偏移留在这里，模型只按 `name` 引用。
 *
 * ## 判据（都可复跑）
 * * `handler`：`pnpm tools opcodes handlers --opcode <opcode>` **现算**所得
 *   —— 守卫 `tools/test/opcodes-handlers.assets.test.mjs` 逐条对账；
 * * `dword`：handler 体（`pnpm tools disasm-at pseudo --sym <handler>`）里**逐字出现** `this[<dword>]`
 *   （一个 handler 写多个槽 ⇒ 多条记录）；
 * * `form`：值的来源 —— `op1` = 操作数 1 的原值、`bool(op1)` = 非 0 归一成 1、
 *   `bswap24(op1)` = 在**低 24 位内**把字节序倒过来（`b0<<16 | b1<<8 | b2`；像 BGR↔RGB）；
 * * `callsAfter`：**写完标量之后**还有哪次子系统调用 —— ★ 这一列是**必需的诚实**：
 *   少了它，"handler 里那次未建模的调用"就不会进保真欠账，日志会显得比实际干净
 *   （本仓踩过：`0x78`/`0x2db` 的体里都有 `sub_459F40(...)`，第一版没记）。
 *
 * ## ⛔ 它**不是**语义结论
 * `Engine.d97058` 只是一个**稳定身份**（dword 下标），**不是含义**。含义要等有人按 handler 的
 * 调用方 / 读者去核 —— 那才是知识（准入门见 `AGENTS.md` §6）。
 */
export const ENGINE_SCALAR_WRITES = [
  { name: 'Engine.d97058', dword: 97058, opcode: 0x149, handler: 'sub_4229A0', form: 'op1' },
  { name: 'Engine.d166965', dword: 166965, opcode: 0x21b, handler: 'sub_423C20', form: 'bool(op1)' },
  { name: 'Engine.d92323', dword: 92323, opcode: 0x252, handler: 'sub_425AB0', form: 'op1' },
  { name: 'Engine.d1415', dword: 1415, opcode: 0x88, handler: 'sub_41FAB0', form: 'op1' },
  { name: 'Engine.d97050', dword: 97050, opcode: 0x88, handler: 'sub_41FAB0', form: 'op1' },
  { name: 'Engine.d21667', dword: 21667, opcode: 0x78, handler: 'sub_41F450', form: 'op1', callsAfter: ['sub_459F40'] },
  { name: 'Engine.d71744', dword: 71744, opcode: 0x2db, handler: 'sub_426500', form: 'op1', callsAfter: ['sub_459F40'] },
  // ★ `0x76`：写 `Engine+0x15280`（紧邻 `0x78` 的 `+0x1528C`，同一个结构的不同字段）+ 同一次 `sub_459F40`
  { name: 'Engine.d21664', dword: 21664, opcode: 0x76, handler: 'sub_41F390', form: 'bswap24(op1)', callsAfter: ['sub_459F40'] },
  // ★ `0x77` 与 `0x76` **同形**（只差槽：`+0x15284`），体逐字 `this[21665] = BYTE2(v2) + ((BYTE1(v2) + ((u8)v2 << 8)) << 8)`
  { name: 'Engine.d21665', dword: 21665, opcode: 0x77, handler: 'sub_41F3F0', form: 'bswap24(op1)', callsAfter: ['sub_459F40'] },
  // ★ `0x1a4`（argc 2）：**两个槽、两个操作数**（`this[21671] = op2; this[21670] = op1`，无子系统调用）
  { name: 'Engine.d21670', dword: 21670, opcode: 0x1a4, handler: 'sub_41FE60', form: 'op1' },
  { name: 'Engine.d21671', dword: 21671, opcode: 0x1a4, handler: 'sub_41FE60', form: 'op2' },
  // ★ `0x2ee`（argc 1）：`this[80106] = op1` 之后对 `Engine+0xAA554` 那个对象走 **vtable+12**
  //   （实参含静态串 `aMessageMessage_0` 与 op1）——与 `0x1ca` 同形。**返回值不写回操作数**。
  { name: 'Engine.d80106', dword: 80106, opcode: 0x2ee, handler: 'sub_426650', form: 'op1', callsAfter: ['(vtable+12) on Engine+0xAA554（aMessageMessage_0）'] },
  // ★ `0xfe`（argc 1）：**带范围检查**的标量写（`sub_421CA0`）——
  //   `result = op1; if (result > 0x1F) throw (aSetkeytotal, 65541); this[517] = result;`
  //   ⇒ 上界不是"顺手加的守卫"，是**引擎自己抛的异常**（`max` 一字不差照抄 0x1F）。
  { name: 'Engine.d517', dword: 517, opcode: 0xfe, handler: 'sub_421CA0', form: 'op1', max: 0x1f },
  // ★ `0x10f`（argc 1）：普通标量写（`sub_422120`：`this[122369] = op1`）
  { name: 'Engine.d122369', dword: 122369, opcode: 0x10f, handler: 'sub_422120', form: 'op1' },
  // ★★ `0x110` / `0x111` / `0x112`：**派发表里没有登记**它们（实测：`byOpcode.get(0x110)` 为空）。
  //   而表是 `rep stosd` **预填**成默认 handler 的 —— 那个默认 handler（`sub_418E30`）的体是
  //   抛「このコマンドはサポートされていません．」⇒ **引擎明确不支持这三条命令**。
  //   ⇒ 本层把它们登记成"照抄引擎的抛"，于是"走到这里"就变成一个**可见的分歧信号**
  //   （而不是含混的"本批未实现"）：真实启动流程**不该**执行到这些命令。
  { name: 'Engine.unsupported', dword: -1, opcode: 0x110, handler: 'sub_418E30', form: 'unsupported' },
  { name: 'Engine.unsupported', dword: -1, opcode: 0x111, handler: 'sub_418E30', form: 'unsupported' },
  { name: 'Engine.unsupported', dword: -1, opcode: 0x112, handler: 'sub_418E30', form: 'unsupported' },
];
