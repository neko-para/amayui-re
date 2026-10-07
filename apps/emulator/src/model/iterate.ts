/**
 * apps/emulator/src/model/iterate.ts —— **迭代系统**：`字节流 → 指令`（批 R1 迭代点 ⑥）
 *
 * ## 它是什么 / 不是什么
 * ✅ 它把脚本字节流按 **opcode + argc** 机械地切成一条条指令，并算出每条指令的**字节长度**。
 * ❌ 它**不做执行**：不解释 operand type、不读写池、不改任何状态（用户口径：不含整体执行流程）。
 *    ⇒ 它的职责只有"边界算得对"这一件事 —— 而这恰恰是**一切后续工作的前提**：
 *      argc 错 1，整条流都错位（后面每条指令都变成垃圾）。
 *
 * ## 布局（三条口径，都有语料证据）
 * ```
 * 指令 = opcode(u32) + argc × { type(u32), raw_data(u32) }        ⇒ 字节长度 = 4 + 8*argc
 * dword 长度 = 2*argc + 1                                         ⇒ 与字节长度恒等（4*(2*argc+1) = 4+8*argc）
 * 下一条指令的字节偏移 = 上一条 + 4 + 8*argc
 * ```
 * ★ 第二条是**引擎自己在写的**：每条 handler 体内都 `mov dword ptr [esi+ecx*8+5D8F4h], N`（`帧+0x74`），
 *   `N = 2*argc+1`（实测 `add` ⇒ 7、`mov` ⇒ 5）；主循环则 `ip += 4 * 该槽`。
 *   守卫会**对整个指令表**核对这条恒等式 —— 它把"指令表"与"引擎的推进口径"绑在一起。
 *
 * ## 指令区边界
 * * 起点 = 头部长度（v4 = 60 字节；v5 = 68）
 * * 终点 = `头部长度 + min(三张表的 offset) * 4`（三张表紧跟在指令区之后）
 * * `opcode == 0` ⇒ 引擎直接报 "bad opcode : 0"（语料里不该出现）
 */
/**
 * ★ `ByteSource` / `Header` / `OpcodeDef` / `OpcodeTable` 都是**格式层的类型** ——
 * 由 `packages/age-format/src/asm/header.mts` / `opcodes.mts` 定义，这里只 `import type`。
 * （类型属于拥有它的包；消费方不再自己声明、也不再用 `as` 把边界糊过去。）
 * 它们都刻意用**结构化类型**（`Uint8Array` / 最小接口）而不是 `NodeJS.Buffer`。
 *
 * ★★ **入口是 `asm/runtime.mts`，不是 `asm/index.mts`**：后者是**工具侧总入口**，会把反汇编器 /
 * 重汇编器（工具侧语义 + 体积）一起拖进前端 bundle。★ 这条分界**不是平台** —— `age-format` 的
 * `src/asm/**` 现在**整个目录零 Node 依赖**（指令表随模块自带，走 ESM JSON import），
 * 所以核心要表时直接 `import { OPCODE_TABLE }` 即可。★ 边界仍由 `apps/emulator/tsconfig.json` 的
 * **`"types": []`** 兜底：谁把 `node:*` / `Buffer` 拉进可达闭包，`pnpm typecheck` 当场红
 * （不需要"扫源码"的守卫 —— 见 `decisions.md` 里那条"用类型系统而不是测试当守卫"）。
 */
import { ByteReader, readHeader } from '@amayui/age-format/src/asm/runtime.mts';
import type { ByteSource, Header, OpcodeDef, OpcodeTable } from '@amayui/age-format/src/asm/runtime.mts';

export type { ByteSource, Header, OpcodeDef, OpcodeTable };

/** 一条指令的**操作数**（只切边界，不解释 `type` 的含义） */
export interface InstrArg {
  type: number;
  rawData: number;
}

/** 切好边界的一条指令 */
export interface Instr {
  index: number;
  byteOffset: number;
  opcode: number;
  name: string;
  argc: number;
  byteLength: number;
  dwords: number;
  args: InstrArg[];
}

/** 迭代过程中记下的结构问题（**不猜语义**，只报"这里对不上"） */
export interface IterProblem {
  byteOffset: number;
  kind: string;
  message: string;
}

export interface IterateResult {
  header: Header;
  headerLen: number;
  instructions: Instr[];
  endOffset: number;
  problems: IterProblem[];
}

/** 一条指令的**字节长度**（唯一真源：`4 + 8*argc`） */
export const instrByteLength = (argc: number): number => 4 + 8 * (argc >>> 0);

/** 一条指令占的 **dword 数**（引擎 `帧+0x74` 那个槽写的值） */
export const instrDwords = (argc: number): number => 2 * (argc >>> 0) + 1;

/** 恒等式：字节长度 == dword 数 × 4（★ 由它把"指令表"与"引擎推进口径"绑起来） */
export const lengthInvariantHolds = (argc: number): boolean =>
  instrByteLength(argc) === instrDwords(argc) * 4;

/**
 * 把一段脚本字节流迭代成指令序列（**只切边界，不解释**）。
 *
 * @param bin 脚本字节码
 * @param opts.table 指令表（本包自带的 `OPCODE_TABLE`，见 `asm/runtime.mts`）；`opts.strict` = 遇到结构问题是否立刻停（缺省 true）
 */
export function iterate(bin: ByteSource, { table, strict = true }: { table: OpcodeTable; strict?: boolean }): IterateResult {
  const header = readHeader(bin);
  // ★ 一次构造、循环里复用（`ByteReader` 内部是 `DataView(b.buffer, b.byteOffset, b.byteLength)`）
  const rd = new ByteReader(bin);
  const headerLen = header.length;
  const minTableOffset = Math.min(header.fields.table_1_offset, header.fields.table_2_offset, header.fields.table_3_offset);
  let endOffset = headerLen + (minTableOffset << 2);
  const problems: IterProblem[] = [];
  if (endOffset > bin.length) {
    problems.push({
      byteOffset: endOffset - 1,
      kind: 'tables-past-eof',
      message: `三张表的 offset 推出的指令区终点 ${endOffset} 超过文件长度 ${bin.length}`,    });
    endOffset = bin.length;
  }

  const instructions: Instr[] = [];
  let pos = headerLen;
  let index = 0;
  while (pos < endOffset) {
    if (pos + 4 > bin.length) {
      problems.push({ byteOffset: pos, kind: 'opcode-past-eof', message: 'opcode 越界' });
      break;
    }
    const opcode = rd.u32(pos);
    const byteOffset = pos;
    if (opcode === 0) {
      // 引擎在这一步是**直接报错**（"bad opcode : 0"）—— 模拟器不许悄悄跳过
      problems.push({ byteOffset, kind: 'bad-opcode-zero', message: 'opcode = 0（引擎会报 "bad opcode : 0"）' });
      if (strict) break;
      pos += 4;
      continue;
    }
    const def = table.byOpcode.get(opcode);
    if (!def) {
      problems.push({ byteOffset, kind: 'unknown-opcode', message: `opcode 0x${opcode.toString(16)} 不在指令表里` });
      if (strict) break;
      pos += 4;
      continue;
    }
    const argc = Number(def.argc);
    const byteLength = instrByteLength(argc);
    if (byteOffset + byteLength > bin.length) {
      problems.push({ byteOffset, kind: 'instr-past-eof', message: `指令 0x${opcode.toString(16)}（argc=${argc}）越过文件尾` });
      break;
    }
    const args: InstrArg[] = [];
    for (let k = 0; k < argc; k += 1) {
      const type = rd.u32(pos + 4 + 8 * k);
      const rawData = rd.u32(pos + 8 + 8 * k);
      args.push({ type, rawData });
      // ★ **动态前压指令区终点**（与反汇编器同口径）：type-2（字符串）的数据块偏移，
      //   以及 `0x64` 第 2 操作数（数组块）的偏移 —— 它们才是指令区真正的末尾。
      //   不这么做会走过头、撞上数据区里的字节（实测表现：一堆 `opcode=0`）。
      if (type === 2 || (opcode === 0x64 && k === 1)) {
        endOffset = Math.min(endOffset, headerLen + (rawData << 2));
      }
    }
    instructions.push({
      index,
      byteOffset,
      opcode,
      name: def.name ?? '',
      argc,
      byteLength,
      dwords: instrDwords(argc),
      args,
    });
    pos += byteLength;
    index += 1;
  }
  return { header, headerLen, instructions, endOffset, problems };
}

/** 供诊断：把一条指令渲染成一行（不含 operand 解释） */
export const instrLine = (ins: Instr): string =>
  `+0x${ins.byteOffset.toString(16)} 0x${ins.opcode.toString(16).padStart(3, '0')} ${(ins.name || '(无名)').padEnd(14)} argc=${ins.argc} bytes=${ins.byteLength}`;

export { readHeader };
