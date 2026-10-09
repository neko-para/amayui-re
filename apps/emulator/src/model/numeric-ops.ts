/**
 * apps/emulator/src/model/numeric-ops.ts —— **纯数值指令族**的定义（批 R1 迭代点 ⑤）
 *
 * ## 这一族的判据（★ 2026-10 订正：按字面为假，必须带一个 carve-out）
 * 一个 opcode 属于纯数值族，当且仅当它的 handler **除分派器协议字段之外**不碰任何引擎状态：
 * ```
 *   读  Engine+0x5D880              当前帧序号（×15×8 = 120·cur 的索引基数）
 *   写  Engine+0x5D8F4 + 120·cur   = 1 + 2·argc   ← 本条指令占几个 4 字节块
 * ```
 * 这两条**不是副作用**，是**分派器协议**（分派器在 handler 返回后立刻读它并 `ip += 4 × 长度字`）。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `model/numeric-ops-purity-carveout`）。
 * ★ 顺带的机械证据：长度字 = `2·argc+1`（39 条逐条核过全部相符）⇒ `argc` 有了独立于旧表的判据。
 * ★ 这条订正**不改任何常量**（`TOUCHES_ENGINE_STATE` 的例外集合实测正确），只改**判据的措辞**。
 *
 * ## 本文件里的东西都是**机械核过**的
 * * `handler`：从语料构造函数的分派表提取（`mov dword ptr [esi+0A50xxh], offset sub_XXXXXX`，
 *   基址 `0xA509C`，`opcode = (偏移−0xA509C)/4`）——与旧仓 `analysis/opcodes.json` **逐条一致**。
 * * `name` / `argc`：来自本仓 `packages/age-format/src/asm/instruction-set.json`（它由旧表派生，
 *   派生链见 `pnpm tools opcodes`）。
 * * `staticUses`：在 492 个反汇编 .BIN 上按 argc 走一遍数出来的**出现次数**。
 *   ★ 它是**可复算的观察**（不是估计）：`0` 表示"整个语料里一条都没有"。
 *   这批 0 是很有用的判据 —— 未出现的指令**不可能**被语料当作已验证行为，动它们只影响推理不影响产物。
 *
 * ## 边界（★ 不硬塞进来的）
 * * `0x5D/0x60` 之外的"内存/数组/指针"族（`0x2D8 set-array-to` / `0x64 copy-local-array` /
 *   `0x6C fill-zero` / `0x61 lookup-array` / `0x63 lea` / `0x1B0 memcpy` / `0x2C9`）**不做算术**，
 *   只搬运 —— 它们**不属于**本族，属"数据搬运"族（另批）。
 * * 字符串↔数值转换（`0x192 set-string` / `0x193 concat` / `0x2EC atoi` …）输出是串，口径另算。
 * * `semanticsUnknown`：`0x2D7 / 0x2D9 / 0x2DB / 0x2DD / 0x2DE / 0x2DF–0x2E4` 在旧仓是"仅映射"（无语义）
 *   ⇒ 本模型**只登记存在与句柄，不写语义**（凡"应该是浮点比较/取整"之类都是猜测）。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `model/numeric-ops-semantics-unknown`）。
 */

/**
 * 一族里的每条：opcode / 助记符（可空）/ argc / handler / 语料静态出现次数 / 语义已知否
 *
 * ★★ **`staticUses` 的"走法"必须写清 —— 两种走法在族内有 25/39 条对不上**
 * （独立复核抓到，我复算确认）：
 *   * **走法 A（当前口径）**：起点 = v4 的 56 字节 / v5 的 64 字节；遇到未知 opcode 就 `+4` **重同步**；一直扫到文件尾。
 *   * **走法 B（引擎迭代器 `iterate.mjs`）**：起点 = 头部长度（v4 **60**）；按**动态前压的指令区终点**停。
 * ⇒ 两者给出**同一组 6 条零出现**（`0x2D2 0x2D4 0x2D6 0x2DF 0x2E0 0x2E1`），但 B 另外把 6 条
 *   （`0x2D7 0x2D9 0x2DD 0x2E2 0x2E3 0x2E4`）也算成 0 —— 因为**每个文件它只解析到第一个结构异常为止**。
 * ★ **该用哪个是"判断"而不是"测量"**（A 量"文件里有多少条"，B 量"引擎会执行多少条"）：
 *   本模型暂用 A 的数，并把这个选择**显式留在这里**，等迭代系统接入执行时再定。
 */
/** 纯数值族里的一条指令 */
export interface NumericOp {
  opcode: number;
  /** 助记符（本仓指令表里的写法；无名者空串） */
  name: string;
  argc: number;
  /** 在反汇编 .BIN 语料里按 argc 走一遍数出来的出现次数（★ 走法见上注） */
  staticUses: number;
  /** 已知语义（未解者**不写** —— 不许猜） */
  semantics?: string;
  /** 旧仓「仅映射」、语义未解 */
  semanticsUnknown?: boolean;
  /** 体内碰到的 dispatcher 出参以外的引擎状态（有则必须写清，见 `TOUCHES_ENGINE_STATE`） */
  touchesEngineState?: string;
}

export const NUMERIC_OPS: NumericOp[] = [
  // —— 0x50..0x5F：整数双目 + mov（旧仓 ARITHMETIC_OPS，逐条机械核实）
  { opcode: 0x50, name: 'add', argc: 3, staticUses: 70439, semantics: 'op1 = op2 + op3' },
  { opcode: 0x51, name: 'sub', argc: 3, staticUses: 48129, semantics: 'op1 = op2 − op3' },
  { opcode: 0x52, name: 'mul', argc: 3, staticUses: 7475, semantics: 'op1 = op2 * op3' },
  { opcode: 0x53, name: 'div', argc: 3, staticUses: 2487, semantics: 'op1 = op2 / op3（除 0 ⇒ 引擎抛）' },
  { opcode: 0x54, name: 'mod', argc: 3, staticUses: 758, semantics: 'op1 = op2 % op3（C 截断；0 ⇒ 抛）' },
  { opcode: 0x55, name: 'mov', argc: 2, staticUses: 448922, semantics: 'op1 = op2（只读 op2、只写 op1）' },
  { opcode: 0x56, name: 'and', argc: 3, staticUses: 57685, semantics: 'op1 = op2 & op3' },
  { opcode: 0x57, name: 'or', argc: 3, staticUses: 52973, semantics: 'op1 = op2 | op3' },
  { opcode: 0x58, name: 'sar', argc: 3, staticUses: 8, semantics: 'op1 = op2 >> op3（算术右移）' },
  { opcode: 0x59, name: 'shl', argc: 3, staticUses: 348, semantics: 'op1 = op2 << op3' },
  { opcode: 0x5a, name: 'eq', argc: 3, staticUses: 211725, semantics: 'op1 = (op2 == op3)' },
  { opcode: 0x5b, name: 'ne', argc: 3, staticUses: 5254, semantics: 'op1 = (op2 != op3)' },
  { opcode: 0x5c, name: 'lt', argc: 3, staticUses: 40661, semantics: 'op1 = (op2 < op3)（有符号）' },
  { opcode: 0x5d, name: 'lte', argc: 3, staticUses: 624, semantics: 'op1 = (op2 <= op3)' },
  { opcode: 0x5e, name: 'gr', argc: 3, staticUses: 3424, semantics: 'op1 = (op2 > op3)' },
  { opcode: 0x5f, name: 'gre', argc: 3, staticUses: 5859, semantics: 'op1 = (op2 >= op3)' },
  // —— 单目 / 位 / 随机
  { opcode: 0x60, name: 'random', argc: 2, staticUses: 22, semantics: 'op1 = rand() % op2（★ 本族**唯一非确定性**；op2==0 ⇒ 先写 op1=0 再抛）', touchesEngineState: '写 `Engine+0x69330h`（`inc` / `cmp …,0Ch` / 归零 ⇒ **上限 12 的计数器**：每条指令最多重掷 12 次）' },
  { opcode: 0x135, name: 'bit-set', argc: 2, staticUses: 1088, semantics: 'op1 |= (1<<op2)（位号 >0x1F ⇒ 打错误串后继续，不写 op1）' },
  { opcode: 0x136, name: 'bit-reset', argc: 2, staticUses: 194, semantics: 'op1 &= ~(1<<op2)（同上越界）' },
  { opcode: 0x13f, name: 'check-bit', argc: 3, staticUses: 93, semantics: 'op1 = ((1<<op3) & op2) != 0（**位号是 op3**）' },
  // —— 浮点族
  { opcode: 0x191, name: '', argc: 2, staticUses: 14, semantics: 'op1 = fabs(op2)（浮点）' },
  { opcode: 0x2d0, name: '', argc: 3, staticUses: 11, semantics: '浮点双目（fadd 位）' },
  { opcode: 0x2d1, name: '', argc: 3, staticUses: 12, semantics: '浮点双目（fsub 位）' },
  { opcode: 0x2d2, name: '', argc: 3, staticUses: 0, semantics: '浮点双目（fmul 位）' },
  { opcode: 0x2d3, name: '', argc: 3, staticUses: 4, semantics: '浮点双目（fdiv 位）' },
  { opcode: 0x2d4, name: '', argc: 3, staticUses: 0, semantics: 'fmod(op2,op3)（符号跟随除数）' },
  { opcode: 0x2d5, name: 'float-mov', argc: 2, staticUses: 13076, semantics: 'op1 = op2（float→float）' },
  { opcode: 0x2d6, name: '', argc: 2, staticUses: 0, semantics: '类型转换：整数 op2 → 浮点 op1' },
  // —— 语义未解（★ 只登记存在，不写语义）
  { opcode: 0x2d7, name: '', argc: 2, staticUses: 1, semanticsUnknown: true },
  { opcode: 0x2d9, name: '', argc: 2, staticUses: 2, semanticsUnknown: true },
  { opcode: 0x2da, name: '', argc: 8, staticUses: 93, semanticsUnknown: true, touchesEngineState: '体内出现 `Engine+0x14D30` / `+0x313D0`（语义未解，不猜）' },
  { opcode: 0x2db, name: '', argc: 1, staticUses: 1, semanticsUnknown: true, touchesEngineState: '写 `Engine+0x313D0`（实测 `mov [esi+313D0h],eax`）' },
  { opcode: 0x2dd, name: '', argc: 2, staticUses: 3, semanticsUnknown: true, touchesEngineState: '读 `Engine+0x460F0` / `+0x460F4`' },
  { opcode: 0x2df, name: '', argc: 3, staticUses: 0, semanticsUnknown: true },
  { opcode: 0x2e0, name: '', argc: 3, staticUses: 0, semanticsUnknown: true },
  { opcode: 0x2e1, name: '', argc: 3, staticUses: 0, semanticsUnknown: true },
  { opcode: 0x2e2, name: '', argc: 3, staticUses: 1, semanticsUnknown: true },
  { opcode: 0x2e3, name: '', argc: 3, staticUses: 1, semanticsUnknown: true },
  { opcode: 0x2e4, name: '', argc: 3, staticUses: 1, semanticsUnknown: true },
];

/** ★ 语料里**一条都没有**的那些（静态出现次数 0）—— 有意义的判据，不是"没见过"的含糊说法 */
export const UNUSED_IN_CORPUS = NUMERIC_OPS.filter((o) => o.staticUses === 0).map((o) => o.opcode);

/**
 * ★★ **本族里"并不纯"的例外**（独立复核抓到；原先那条全称断言**不成立**）：
 * * `0x60 random` —— 写 `Engine+0x69330h`（`inc` / `cmp …,0Ch` / 归零）：**上限 12 的计数器**。
 * * `0x2DA` / `0x2DB` / `0x2DD` —— 分别碰 `+0x14D30` `+0x313D0` / `+0x313D0` / `+0x460F0`/`+0x460F4`。
 * ⇒ 正确的说法是"**这一族是数值搬运/算术，其中 4 条还碰引擎状态**"。逐条列在这里，由守卫核对
 *   （谁改动它、或新增例外，都会红）。口径与理由见知识台账：`data/ledger/`
 *   （域 `Emulator`，subject `model/numeric-ops-purity-carveout`）。
 */
export const TOUCHES_ENGINE_STATE = NUMERIC_OPS.filter((o) => o.touchesEngineState).map((o) => o.opcode);

/** 语义未解的那些（旧仓"仅映射"）—— 模型只登记存在 */
export const SEMANTICS_UNKNOWN = NUMERIC_OPS.filter((o) => o.semanticsUnknown).map((o) => o.opcode);

/** 本族的边界说明（哪些**不**属于它，免得下次有人顺手塞进来） */
export const NOT_IN_FAMILY = {
  reason: '只搬运、不做算术（或输出是字符串）⇒ 不属于纯数值族',
  opcodes: [0x64, 0x6c, 0x61, 0x63, 0x12c, 0x1b0, 0x2c9, 0x2d8, 0x12f, 0x192, 0x193, 0x194, 0x195, 0x1a6, 0x1b2, 0x2c5, 0x2c6, 0x2c7, 0x2c8, 0x2de, 0x2ec],
};

/** 按 opcode 找一条（不在族里 ⇒ `null`） */
export const byOpcode = (op: number): NumericOp | null => NUMERIC_OPS.find((o) => o.opcode === op) ?? null;
/** 这条的语义是否已知（"未解"的**不许**被当成已知用） */
export const hasSemantics = (op: number): boolean => {
  const e = byOpcode(op);
  return Boolean(e && !e.semanticsUnknown);
};
