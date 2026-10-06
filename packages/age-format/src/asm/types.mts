/**
 * packages/age-format/src/asm/types.mjs —— **操作数类型标签 ↔ `type` 字段**（以及控制流判据）
 *
 * 来源（旧仓只读参照，逐行对照移植）：
 *   · `天結/scripts/asm/age-shared.mjs` —— `getTypeLabel` / `getType` / `isControlFlowOpcode` /
 *     `isArrayOpcode` / `isLabelArgument`
 *
 * ## 为什么要单独一个文件
 * 反汇编器与重汇编器**必须共用同一张表**（一个写标签、一个读标签），否则"写出去读不回来"。
 * 放进 `disassemble.mjs` 会让 `assemble.mjs` 反向依赖反汇编器；放这里两边都是叶子依赖。
 *
 * ## 非显然口径
 * 1. **`type` 字段不是连续枚举**：`0/2` 是"无标签"（标量 / 字符串各按上下文判），`1` 是 float，
 *    `3–0xE` 是变量种类，`0x8003/0x8005/0x8009/0x800B` 是带高位的特殊种类（原样用十六进制当标签）。
 *    落在 `0xF–0x8002`、`0x800C+` 的值一律抛错 —— 那是坏数据，不是可忽略的边角。
 * 2. `getTypeLabel` 对 `2` 返回空串：字符串操作数在文本里写成 `"…"`，不带类型标签。
 * 3. `getType` 额外认 `unknown0x8003` 一族**历史别名**（老文本写法），反汇编不会写出它们。
 * 4. `isLabelArgument` 里每个 opcode 的位序是**格式约定**（`0xFFFFFFFF` = 无 label 哨兵）。
 *    这张表决定了文本里 `label_XXXXXXXX` 出现的位置，改它会让旧文本重汇编后与旧字节码不一致。
 * 5. 头部那 13 个 u32 数值字段的**顺序与相对偏移**也放在这里（`FIELD_OFFSETS` / `FIELD_NAMES` /
 *    `fieldBlockShift`）：反汇编器读、重汇编器写，两边必须共用同一张表，否则"读出来写不回去"。
 */

// ───────────────────────────────────────────────────────── 头部数值字段（两方向共用）

/**
 * 13 个 u32 数值字段**相对签名起点**的偏移。v4 签名 8 字节、v5 签名 16 字节 ⇒ v5 的整块字段
 * 在文件里再整体后移 8 字节（`fieldBlockShift`）。
 *
 * ★ 这条"整体后移 8 字节"是本包对旧仓的一处**有意修正**：旧仓 Node 版对 v5 调用
 *   `parseNumericFields(buf, 16)`（把签名起点当成了已含 8 字节偏移）**又**在函数内用 `+8…+56` 读，
 *   等价于整块再后移 8 字节，于是 13 个字段里从 `unknown_data` 起全部错位
 *   （`sub_header_length` 会被读成别的字段）。真实语料全是 v4，所以旧仓一直没暴露。
 *   本包按"C++ 字段顺序 + 签名长度差"重建：6 个 `local_*` → `sub_header_length` → 三张表各 2 个。
 */
export const FIELD_OFFSETS = [8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56];

/** 与 `FIELD_OFFSETS` 一一对应的字段名（顺序即文件里的顺序） */
export const FIELD_NAMES = [
  'local_integer_1', 'local_floats', 'local_strings_1', 'local_integer_2', 'unknown_data', 'local_strings_2',
  'sub_header_length',
  'table_1_length', 'table_1_offset', 'table_2_length', 'table_2_offset', 'table_3_length', 'table_3_offset',
];

/** 数值字段块相对"签名起点"的位移：v4 = 0、v5 = 8 */
export const fieldBlockShift = (isVer5: boolean): number => (isVer5 ? 8 : 0);


/** `type` 字段 → 文本里的类型标签；空串表示"无标签"（标量 / 字符串 / 数组各有自己的写法） */
export function getTypeLabel(type: number): string {
  switch (type) {
    case 0: return '';
    case 1: return 'float';
    case 2: return '';
    case 3: return 'global-int';
    case 4: return 'global-float';
    case 5: return 'global-string';
    case 6: return 'global-ptr';
    // ★ type 7 = **global 的 float 指针池**（operand type 7）。补它的依据**不是**旧仓文档，而是语料里
    //   取址原语 `sub_42AEA0` 的跳转表 `jpt_42AF16`（12 个 case，覆盖 0..13）——它在 case 7 上
    //   算出"global float 指针池的第 idx 项"。原先这里缺 7 ⇒ `getTypeLabel(7)` **抛**，
    //   而同一包的 `disassemble.mjs` 却允许 7 通过范围检查（两处自相矛盾）。
    //   守卫：`tools/test/engine-operand-types.test.mjs`（红 = 引擎 case 集与类型表不再一致）。
    //   ★ 与 `local-float-ptr`(0xd) 配对读：**global** 一侧是 7、**local** 一侧是 0xd。
    case 7: return 'global-float-ptr';
    case 8: return 'global-string-ptr';
    case 9: return 'local-int';
    case 0xa: return 'local-float';
    case 0xb: return 'local-string';
    case 0xc: return 'local-ptr';
    case 0xd: return 'local-float-ptr';
    case 0xe: return 'local-string-ptr';
    case 0x8003: return '0x8003';
    case 0x8005: return '0x8005';
    case 0x8009: return '0x8009';
    case 0x800b: return '0x800B';
    default:
      throw new Error(`Unknown type value: ${type.toString(16)}`);
  }
}

/** 文本里的类型标签 → `type` 字段（`getTypeLabel` 的逆） */
export function getType(name: string): number {
  const map: Record<string, number> = {
    'local-int': 9, 'local-ptr': 0xc, 'global-int': 3, 'global-float': 4,
    'global-string': 5, 'global-ptr': 6, 'global-float-ptr': 7, 'global-string-ptr': 8,
    'local-float': 0xa, 'local-string': 0xb, 'local-string-ptr': 0xe,
    float: 1, 'local-float-ptr': 0xd,
  };
  if (name in map) return map[name];
  const special: Record<string, number> = {
    '0x8003': 0x8003, '0x8005': 0x8005, '0x8009': 0x8009, '0x800B': 0x800B,
    unknown0x8003: 0x8003, unknown0x8005: 0x8005,
    unknown0x8009: 0x8009, unknown0x800B: 0x800B,
  };
  if (name in special) return special[name];
  throw new Error(`Unknown variable type: ${name}`);
}

/** 控制流指令（操作数可能是 label） */
export const isControlFlowOpcode = (op: number): boolean =>
  [0x8c, 0x8f, 0xa0, 0xcc, 0xfb, 0xd4, 0x90, 0x7b, 0xa2, 0xa3].includes(op);

/** 数组指令（第 2 个操作数是"数组块引用"） */
export const isArrayOpcode = (op: number): boolean => op === 0x64;

/** `isLabelArgument` 要看的那两部分（只声明**本函数用到的**形状，不把整个指令结构搬进来） */
export interface LabelArgProbe {
  /** 指令定义（本函数只读 `opcode`） */
  readonly def: { readonly opcode: number };
  /** 操作数数组（本函数只读第 `x` 个的 `raw_data`） */
  readonly args: readonly { readonly raw_data: number }[];
}

/** 第 `x` 个操作数是不是 label（`0xFFFFFFFF` = 无 label 哨兵） */
export function isLabelArgument(instr: LabelArgProbe, x: number): boolean {
  const { opcode } = instr.def;
  const raw = instr.args[x].raw_data;
  if ((opcode === 0x8c || opcode === 0x8f) && raw !== 0xffffffff) return true;
  if (opcode === 0xa0 && x > 0 && raw !== 0xffffffff) return true;
  if ((opcode === 0xcc || opcode === 0xfb) && x > 0 && raw !== 0xffffffff) return true;
  if (opcode === 0xd4 && x >= 2 && raw !== 0xffffffff) return true;
  if (opcode === 0x90 && x >= 4 && raw !== 0xffffffff) return true;
  if (opcode === 0x7b && raw !== 0xffffffff) return true;
  // menu-bind(op2=目标 label) / menu-dispatch(op2=回退 label) 的第 2 个操作数是 label。
  if ((opcode === 0xa2 || opcode === 0xa3) && x === 1 && raw !== 0xffffffff) return true;
  return false;
}

/** 十六进制（小写、无补零；对应 C++ `std::hex` 默认） */
export const hex = (v: number): string => (v >>> 0).toString(16);
/** 8 位小写十六进制（对应 `std::setw(8) << std::setfill('0') << std::hex`） */
export const labelHex = (v: number): string => (v >>> 0).toString(16).padStart(8, '0');
