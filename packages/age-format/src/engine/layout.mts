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

/** 各 local 池在帧内的**计数槽 / 基址槽**（观测；与 `apps/emulator` 里的语义池名对应） */
export const LOCAL_POOL_SLOTS = [
  { name: 'int', count: 0x1c, base: 0x34 },
  { name: 'float', count: 0x20, base: 0x38 },
  { name: 'string', count: 0x24, base: 0x3c },
  { name: 'ptr', count: 0x28, base: 0x40 },
  { name: 'floatPtr', count: 0x2c, base: 0x44 },
  { name: 'stringPtr', count: 0x30, base: 0x48 },
];

/** 元素宽度里哪些**逐字核实过**（★ 没核实的别当事实） */
export const ELEM_BYTES_VERIFIED = { int: true, float: true, ptr: true, floatPtr: true, stringPtr: true, string: false };

/** 常量携带的语料证据（守卫会拿它回语料复核 → EA 锚的"可执行形式"） */
export const EVIDENCE = {
  frameStride: { at: '.text:0041BF64..0041BF69', insns: ['shl edx,4', 'sub edx,eax', 'mov eax,[ecx+edx*8+5D898h]'], note: '15·cur ×8 = 120·cur（两条取操作数原语同形）' },
  localPoolBases: { at: '.text:00405693..004056AB', note: '装载器把池基址写进 帧+0x34/…/+0x48' },
  intPoolBase: { at: '.text:0042AF2F..0042AF35', insns: ['mov edx,[esi+5D800h]', 'lea eax,[edx+ecx*4]'], note: 'operand type 3 的取址：base + idx*4（下标不过编码）' },
  /** ★ int 族的 `key` 在 `Engine+0x5EC8C`（运行期赋值，从不出现在立即数里） */
  codecKey: { at: '.text:00417359', insns: ['mov [esi+5EC8Ch], edx'], note: '全清单唯一的写点' },
  /** ★ `enc_zero` 在 `Engine+0x5EC90` = `ENC(key,0)` ⇒ **非 0** */
  encZero: { at: '.text:00415990', insns: ['mov [esi+5EC90h], eax'], note: 'eax = rol(key,21) = ENC(key,0)' },
};
