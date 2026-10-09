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
 *   这里下标由**操作数**给出。
 *   ★ 由操作数给出的下标**怎么落地**（键名要不要带索引、越界要不要留痕）是**实现口径**，不在本层 ——
 *   它在 `apps/emulator/src/vm/ops.ts` 的 `scalarArrayHandler`。
 *
 * ★ `outOfRange` = **引擎自己怎么做**：`'skip'` = 引擎**静默跳过**这次写（逐字 `if (result <= 0x1F) …`，
 *   **没有**异常）；`'throw'` = 引擎抛（见 `0x30a`）。⛔ 两者不许混成一条 —— 那会把"引擎会崩"与
 *   "引擎当无事发生"变成同一种表现（照抄与留痕的做法在 `ops.ts` 的 `scalarArrayHandler`）。
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
  // ★ `0x30a`（`sub_426B60`，argc 2）：表 1969，**两条范围检查且越界都抛**
  //   `v2 = op2（索引）; result = op1（值）; if (result > 0x1F || v2 > 7) throw (aSetgeskey); this[v2 + 1969] = result;`
  //   ⇒ `outOfRange: 'throw'`（与上面两条的 `'skip'` **不同**，这正是它们必须分成两条登记的理由）；
  //   另注意索引上界是 **7**（不是 0x1F）：这张表只有 8 格。
  { name: 'Engine.d1969', base: 1969, len: 8, opcode: 0x30a, handler: 'sub_426B60', indexOperand: 1, valueOperand: 0, maxIndex: 0x7, maxValue: 0x1f, outOfRange: 'throw', note: '8 格的表；值与索引都有范围检查，越界抛 aSetgeskey' },
];

/**
 * 各 local 池在帧内的**计数槽 / 基址槽**（观测；与 `apps/emulator` 里的语义池名对应）。
 *
 * ## ★★ 2026-10 二次订正：基址 = `帧+0x34/0x38/0x3C/0x40/0x44/0x48`（**与旧仓 `fields.json` 一致**）
 *
 * 上一版（同一批）把基址写成 `+0x3C…+0x54`，**那是错的**，错在**把装载器的 6 处 store 与池名配错了**：
 * 它引的 store 地址本身没问题，但把 `0x40F2E9` 那条读成了写 `帧+0x40` —— 逐字是
 * `25262 .text:0040F2E9 mov [esi+edx*8+5D8B4h], eax`，而 `0x5D8B4 − 0x5D880 = 0x34`。
 *
 * ### 判据一：装载器 `sub_40ED40` 的 6 处 store（逐字，本轮复核）
 * ```
 * 25262 .text:0040F2E9 mov [esi+edx*8+5D8B4h], eax   ;; 帧+0x34
 * 25282 .text:0040F321 …（折叠形：`0xC79` ⇒ 0xC79*0x78 = 0x5D8B8 = 帧+0x38）
 * 25332 .text:0040F3B1 mov [esi+edx*8+5D8BCh], ecx   ;; 帧+0x3C
 * 25351 .text:0040F3F0 mov [esi+edx*8+5D8C0h], eax   ;; 帧+0x40
 * 25370 .text:0040F42F mov [esi+edx*8+5D8C4h], eax   ;; 帧+0x44
 * 25390 .text:0040F471 mov [esi+edx*8+5D8C8h], eax   ;; 帧+0x48
 * ```
 *
 * ### 判据二（**定配对的那一半**）：取址原语 `sub_42AEA0` 的跳转表按 **operand type** 分派
 * 入口 `sub ecx,3` + `cmp ecx,0Bh` ⇒ case `3..14`（`.lst:66226-66232`，表 `jpt_42AF16` @`0x42B47C`）。
 * IDA 在**每个 case 标签上写了 case 号**，而 case 号 = type ⇒ 池名与偏移是**机械对上**的，不靠顺序猜：
 * ```
 * 66256 loc_42AF5B ; jumptable case 9  → 66262 mov ecx,[esi+edx*8+5D8B4h]   ⇒ int       = 帧+0x34
 * 66288 loc_42AFB7 ; jumptable case 10 → 66295 mov ecx,[esi+edx*8]（0xC79 折叠）= 帧+0x38 ⇒ float
 * 66313 loc_42B000 ; jumptable case 11 → 66319 mov edx,[esi+edx*8+5D8BCh]   ⇒ string    = 帧+0x3C
 * 66246 loc_42AF3D ; jumptable case 12 → 66251 mov ecx,[esi+edx*8+5D8C0h]   ⇒ ptr       = 帧+0x40
 * 66278 loc_42AF99 ; jumptable case 13 → 66283 mov ecx,[esi+edx*8+5D8C4h]   ⇒ floatPtr  = 帧+0x44
 * 66325 loc_42B027 ; jumptable case 14 → 66330 mov ecx,[esi+edx*8+5D8C8h]   ⇒ stringPtr = 帧+0x48
 * ```
 * ★ 折叠形的算术（自证 `0xC79` 这个魔数）：`(帧基址 + 0x38) / 步长 = 0x5D8B8 / 0x78 = 0xC79`，
 *   而 case 10 的体是 `ecx = cur + 0xC79; edx = 15*ecx; [esi + edx*8]` ⇒ 地址 = `帧 + 120*cur + 0x38`。
 *
 * ### 判据三（旁证）：`ENC(key,0)` 的初值填在 `帧+0x34` 那一池
 * `.lst:25404 mov eax,[eax+5D8B4h]` + `.lst:25407 mov [eax+ecx*4],edx`，而 `edx = [esi+5EC90h] = ENC(key,0)`
 * ⇒ 那个池是 **int 池**（int 族初值口径），与判据二的 case 9 相互印证。
 *
 * ★ `count` 那六格（`帧+0x1C..+0x30`）**两次订正都没动**：两种读法给出同一组。
 * ★ 教训（写给下一个复核者）：这次翻案的根因不是"旧仓对/本仓错"，而是**把 store 地址抄成了别的偏移**；
 *   所以本条把**逐字行号**留在上面 —— 判据要能当场复算，别只留结论。
 */
export const LOCAL_POOL_SLOTS = [
  { name: 'int', count: 0x1c, base: 0x34 },
  { name: 'float', count: 0x20, base: 0x38 },
  { name: 'string', count: 0x24, base: 0x3c },
  { name: 'ptr', count: 0x28, base: 0x40 },
  { name: 'floatPtr', count: 0x2c, base: 0x44 },
  { name: 'stringPtr', count: 0x30, base: 0x48 },
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
    insns: ['mov [esi+edx*8+5D8B4h],eax', 'mov [esi+edx*8+5D8BCh],ecx', 'mov [esi+edx*8+5D8C0h],eax', 'mov [esi+edx*8+5D8C8h],eax'],
    note:
      '装载器 `sub_40ED40` 把 6 个 local 池的**基址**写进 帧+0x34/0x38/0x3C/0x40/0x44/0x48；' +
      '★ **配对**由取址原语 `sub_42AEA0` 的跳转表 case 号（= operand type）钉住（case 9→+0x34 … case 14→+0x48），' +
      '不是靠"分配顺序"猜。逐字行号见 `LOCAL_POOL_SLOTS` 的头注（两次订正的经过也写在那里）',
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
 * * `form`：值的来源 —— `op1` = 操作数 1 的原值、`op2` = 操作数 2、`const` = **写死的常量**（`value` 给出）、
 *   `bool(op1)` = 非 0 归一成 1、
 *   `bswap24(op1)` = 在**低 24 位内**把字节序倒过来（`b0<<16 | b1<<8 | b2`；像 BGR↔RGB）；
 * * `max`：引擎**自己**的范围检查（越界它抛 C++ 异常）—— 值的形态与上界是**事实**；
 *   照抄成抛（⛔ 不许 clamp）是**实现口径**，在 `apps/emulator/src/vm/ops.ts` 的 `scalarHandler`；
 * * ★ **名字前缀区分两种存储**（同一个存储按名字寻址，但语义不同，别混）：
 *   `Engine.dNNN` = `this[NNN]`（引擎对象里的字段）；`Global.dNNNNNN` = **进程全局**（不在 `Engine` 里，
 *   例如 `0x248` 写的 `dword_55052C`）。
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
  // ★★ `callsAfter` 的**参数口径**（2026-10 取证）：`sub_459F40` 的 `this` **不是 Engine**，
  //   而是**字体管理器子对象 `Engine+0x14D30`** —— 5 个调用方在调用点前逐字 `lea ecx, [..+14D30h]`
  //   （`.lst:46607/46647/46670/57911/78916`）。★ 所以 `+0x15280/84/8C` 的宿主是**那个子对象**，
  //   写这些槽的是"往子对象里写"，不是"往 Engine 顶层的这三个偏移写"。
  // ★ 另一条形态差别：`0x78`/`0x2db` 是**尾跳** `jmp sub_459F40`（无返回），`0x76`/`0x77` 是 `call`。
  { name: 'Engine.d21667', dword: 21667, opcode: 0x78, handler: 'sub_41F450', form: 'op1', callsAfter: ['sub_459F40'] },
  { name: 'Engine.d71744', dword: 71744, opcode: 0x2db, handler: 'sub_426500', form: 'op1', callsAfter: ['sub_459F40'] },
  // ★ `0x76`：写 `Engine+0x15280`（紧邻 `0x78` 的 `+0x1528C`，同一个结构的不同字段）+ 同一次 `sub_459F40`
  { name: 'Engine.d21664', dword: 21664, opcode: 0x76, handler: 'sub_41F390', form: 'bswap24(op1)', callsAfter: ['sub_459F40'] },
  // ★ `0x77` 与 `0x76` **同形**（只差槽：`+0x15284`），体逐字 `this[21665] = BYTE2(v2) + ((BYTE1(v2) + ((u8)v2 << 8)) << 8)`
  { name: 'Engine.d21665', dword: 21665, opcode: 0x77, handler: 'sub_41F3F0', form: 'bswap24(op1)', callsAfter: ['sub_459F40'] },
  // ★ `0x1a4`（argc 2）：**两个槽、两个操作数**（`this[21671] = op2; this[21670] = op1`，无子系统调用）
  { name: 'Engine.d21670', dword: 21670, opcode: 0x1a4, handler: 'sub_41FE60', form: 'op1' },
  { name: 'Engine.d21671', dword: 21671, opcode: 0x1a4, handler: 'sub_41FE60', form: 'op2' },
  // ★ `0x2ee`（argc 1）：`this[80106] = op1` 之后对 `Engine+0xAA514` 那个对象走 **vtable+12**
  //   （实参含静态串 `aMessageMessage_0` 与 op1）——与 `0x1ca` 同形。**返回值不写回操作数**。
  //   ★ **偏移订正**（逐字）：`sub_426650` 的 `this[174405]` ⇒ `174405*4 = 0xAA514`；同族 `0x1ca`
  //     （`sub_420240`）逐字也是 `lea esi, [ecx+0AA514h]` ⇒ 原先的 `0xAA554` / `ops.ts` 的 `0xAA614`
  //     **都是抄错的数字**（514 被写成 554/614）。
  { name: 'Engine.d80106', dword: 80106, opcode: 0x2ee, handler: 'sub_426650', form: 'op1', callsAfter: ['(vtable+12) on Engine+0xAA514（aMessageMessage_0）'] },
  // ★ `0xfe`（argc 1）：**带范围检查**的标量写（`sub_421CA0`）——
  //   `result = op1; if (result > 0x1F) throw (aSetkeytotal, 65541); this[517] = result;`
  //   ⇒ 上界不是"顺手加的守卫"，是**引擎自己抛的异常**（`max` 一字不差照抄 0x1F）。
  { name: 'Engine.d517', dword: 517, opcode: 0xfe, handler: 'sub_421CA0', form: 'op1', max: 0x1f },
  // ★ `0x10f`（argc 1）：普通标量写（`sub_422120`：`this[122369] = op1`）
  { name: 'Engine.d122369', dword: 122369, opcode: 0x10f, handler: 'sub_422120', form: 'op1' },
  // ★ `0x25b`（`sub_425E20`，argc 1）：
  //   `result = op1; this[92379] = 2; this[92381] = result;
  //    if (!this[167990]) { v3 = op1; sub_408440(this, v3); }`
  //   ⇒ 两个固定槽（一个写**常量 2**、一个写 op1）+ 一次**条件**子系统调用
  //   （条件读的是 `Engine.d167990` —— 那个槽谁写的**还没取证**，见 handler 里的说明）。
  { name: 'Engine.d92379', dword: 92379, opcode: 0x25b, handler: 'sub_425E20', form: 'const', value: 2 },
  { name: 'Engine.d92381', dword: 92381, opcode: 0x25b, handler: 'sub_425E20', form: 'op1' },
  // ★ `0x248`（`sub_4252E0`，argc 1）：`result = op1; dword_55052C = result;`
  //   ⇒ 写的是**进程全局**（不在 `Engine` 里）⇒ 名字前缀用 `Global.`，别让"Engine 字段"这个说法变成谎
  { name: 'Global.d55052C', dword: 55052, opcode: 0x248, handler: 'sub_4252E0', form: 'op1' },
  // ★★ `0x110` / `0x111` / `0x112`：**派发表里没有登记**它们（实测：`byOpcode.get(0x110)` 为空）。
  //   而表是 `rep stosd` **预填**成默认 handler 的 —— 那个默认 handler（`sub_418E30`）的体是
  //   抛「このコマンドはサポートされていません．」⇒ **引擎明确不支持这三条命令**。
  //   ⇒ 本层把它们登记成"照抄引擎的抛"，于是"走到这里"就变成一个**可见的分歧信号**
  //   （而不是含混的"本批未实现"）：真实启动流程**不该**执行到这些命令。
  { name: 'Engine.unsupported', dword: -1, opcode: 0x110, handler: 'sub_418E30', form: 'unsupported' },
  { name: 'Engine.unsupported', dword: -1, opcode: 0x111, handler: 'sub_418E30', form: 'unsupported' },
  { name: 'Engine.unsupported', dword: -1, opcode: 0x112, handler: 'sub_418E30', form: 'unsupported' },
];

/**
 * **同族但进不了上面那两张表的槽写**（形态不同 ⇒ 表里放不下）—— 观察，仍是引擎事实。
 *
 * 登记它的理由与 `ENGINE_SCALAR_WRITES` 相同：**"哪个 handler 碰了引擎的哪个 dword、写成什么"**
 * 是引擎事实，而 `apps/emulator` 里不许出现偏移 ⇒ 偏移留在本层，模型只按名字引用。
 * ⛔ 这里同样**不给含义**（只有"谁写它、写成什么形态"）。
 *
 * * `0x88`（handler `sub_41FAB0`，argc 1）的两个**条件**副作用：
 *   `if (op1) this[122368] = 1; else this[174801] &= ~0x8000000;`
 *   ⇒ 名字 `Engine.d122368`（**写常量 1**）/ `Engine.d174801`（**位清除 `0x8000000`**）。
 *   ★ 不进 `ENGINE_SCALAR_WRITES` 的理由是**形态**：那张表登记的是"写成 op1 原值 / 常量 / bswap24"
 *     这几族，而这两条是"条件二选一 + 位清除" ⇒ 本批**只记录、不建模**（欠账见需求树）。
 * * `0x212` / `0x25d` / `0x213` 的**对象表**：表基址 = `Engine + 4*21585` = **`Engine+0x15144`**，
 *   按 `op1`（槽号）取指针取对象；**表项为空 ⇒ 什么都不做**（不抛、不报错 —— 又一处"静默"）；
 *   取到对象后写"对象 + 字段偏移"（字段偏移见 `apps/emulator/src/vm/ops.ts` 的 `OBJECT_FIELD_WRITES`
 *   数据行 —— 那几行**暂未**进本层，别在这里重抄一遍）。
 *   ★ "这一族有多大 / 还有几个待核"是**进度**，在需求树里，不在本文件。
 */
