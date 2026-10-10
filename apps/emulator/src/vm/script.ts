/**
 * apps/emulator/src/vm/script.ts —— **装载一份脚本**（★ 核心层：零 Node 依赖）
 *
 * ## 它做什么
 * 把一份脚本的字节变成"可以执行的东西"：头 + 切好边界的指令序列 + **字节偏移 → 指令下标**的映射。
 *
 * ★ 它**不解释**任何语义（那是 handler 的事），也**不读文件**（字节由宿主给）。
 *   切边界那件事已经由 `src/model/iterate.ts` 做了 —— 本模块只在它之上补两样执行期需要的东西：
 *
 * 1. **字节偏移 → 指令下标**：控制流的 label 是**绝对字节偏移**（`headerLen + raw_data*4`），
 *    而执行是按"第几条指令"推进的；这条换算与内联字符串**共用同一个算式**、只该有一处。
 *    口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `vm/script-inline-string-offset`）。
 * 2. **装载期的记账**：池初值、脚本名。★ 这些**不是**可选的收尾工作：
 *    引擎在装载时就给 local 池填了初值（`ENC(key,0)`），**不是 0** ——
 *    "未初始化 = 0" 是错的，而错了以后症状是"某个初值相关的分支偶尔走错"。
 *
 * ## ★ 本批的诚实缺口（登记在需求树里，不在这里糊过去）
 * * **local 池的初值数量**：头部那 6 个 `local_*` 数各自喂给哪个池，
 *   是**装载器**的观察（`.text:00405693..004056AB` 那一族写基址的代码附近），本批没有取证。
 *   ⇒ 本模块**不猜**：`initLocalPools` 只按调用方显式给的计数初始化，缺省**一个都不初始化**，
 *     并把"这次没初始化"记进 `notes`（于是"读到 null"能被解释，而不是看起来像数据坏了）。
 * * **字符串操作数**（`type 2`）：要码页解码器（`asm/codec.mts`，不在运行期子集里）。
 *   LOGO.BIN 到 `play-movie` 为止**一个字符串操作数都没有** ⇒ 本批不接，遇到就响亮失败。
 */

import { iterate } from '../model/iterate.ts';
import type { Instr, IterateResult } from '../model/iterate.ts';
import type { ByteSource, Header, OpcodeTable } from '@amayui/age-format/src/asm/runtime.mts';
import { decodeCp932 } from '@amayui/age-format/src/asm/codec.mts';

/** 内联字符串的长度上限（防御：地址算错时不要一路读到脚本末尾） */
const INLINE_STRING_MAX = 4096;

/**
 * `type 2`（引擎的「内联字符串」）→ 文本。
 *
 * ## ★★ 判据（实证 + 与 label 同一算式）
 * 1. **文件偏移 = `headerLen + 4*raw`** —— 与 label **完全同一个算式**（两者都是"文件内的字节偏移 ÷ 4"）。
 *    实证：`INITCONFIG0.BIN` 共 484 B；代码区占 `60 + 356 = 416` B，而该脚本五条 `set-string`
 *    的 raw 是 89/92/96/99/102 ⇒ `60 + 4*89 = 416` **正好是代码区之后第一字节**。
 * 2. 该处起**逐字节 `^0xFF`**，解出来为 0 即终止（原始字节是 `0xFF`）。
 * 3. 结果是 **cp932（Shift-JIS）** 文本。
 *
 * 实证结果（可复跑）：raw = 89/92/96/99/102 ⇒ `メイリオ` / `ＭＳ ゴシック` / `游ゴシック` / `メイリオ` / `ＭＳ ゴシック`
 * —— 正是"首次运行时把默认字体名写进配置"该有的值（`INITCONFIG0.BIN` 的用途）。
 * ★ 这条换算与 label **共用一处口径**（见文件头第 1 条）。
 *
 * ★ **只给"文本"这一路用**：引擎里 `type 2` 还有一条**整型**读法（`atoi` 未解码字节），
 *   那条**没有取证** ⇒ 本函数**不**承担它（`readOperand` 仍然对 `type 2` 响亮失败）。
 */
export function inlineString(script: LoadedScript, raw: number): string {
  const offset = script.headerLen + (raw >>> 0) * 4;
  const bytes = script.bytes;
  if (offset >= bytes.length) {
    throw new Error(`内联字符串的偏移越出脚本：raw=${raw} ⇒ offset=${offset}（脚本 ${bytes.length} B）`);
  }
  const out: number[] = [];
  for (let p = offset; p < bytes.length; p += 1) {
    const c = bytes[p] ^ 0xff;
    if (c === 0) return decodeCp932(Uint8Array.from(out));
    out.push(c);
    if (out.length > INLINE_STRING_MAX) {
      throw new Error(`内联字符串没有终止符（>` + INLINE_STRING_MAX + ` B，raw=${raw}，offset=${offset}）—— 偏移口径错了？`);
    }
  }
  throw new Error(`内联字符串读到脚本末尾都没有终止符（raw=${raw}，offset=${offset}）`);
}

/** 一份装载好的脚本（执行期的全部静态信息） */
export interface LoadedScript {
  /** 引擎侧的名字（`LOGO.BIN` 这类） */
  name: string;
  bytes: ByteSource;
  header: Header;
  /** 头部长度（label 换算要用） */
  headerLen: number;
  instructions: Instr[];
  /** **字节偏移 → 指令下标**（label 是绝对字节偏移，执行按下标推进） */
  indexByByteOffset: Map<number, number>;
  /** 装载期的说明（缺口、可疑结构）—— 必须能看见，不许静默 */
  notes: string[];
}

/** 装载的选项 */
export interface LoadScriptOptions {
  /** 指令表（核心用 `asm/runtime.mts` 自带的 `OPCODE_TABLE`） */
  table: OpcodeTable;
  /** 遇到结构问题是否立刻停（缺省 true：宁可响亮失败，也不要在错位的指令流上跑下去） */
  strict?: boolean;
}

/**
 * 装载一份脚本。
 *
 * ★ `strict` 缺省 **true**：切边界一旦错位，后面每条指令都是垃圾，而"在垃圾上继续跑"
 *   会产出**看起来正常**的日志（只是全错）。⇒ 结构问题必须在这里就停。
 */
export function loadScript(name: string, bytes: ByteSource, opts: LoadScriptOptions): LoadedScript {
  const iter: IterateResult = iterate(bytes, { table: opts.table, strict: opts.strict ?? true });
  const notes: string[] = [];
  for (const p of iter.problems) notes.push(`+0x${p.byteOffset.toString(16)} ${p.kind}: ${p.message}`);
  if (iter.problems.length && (opts.strict ?? true)) {
    throw new Error(`脚本 ${name} 的结构问题使指令边界不可信（${iter.problems.length} 条），拒绝装载：${notes[0]}`);
  }
  const indexByByteOffset = new Map<number, number>();
  for (const ins of iter.instructions) indexByByteOffset.set(ins.byteOffset, ins.index);
  return {
    name,
    bytes,
    header: iter.header,
    headerLen: iter.headerLen,
    instructions: iter.instructions,
    indexByByteOffset,
    notes,
  };
}

/** label（绝对字节偏移）→ 指令下标；找不到 ⇒ `null`（**不抛**：调用方要能报"跳到了一条指令中间"） */
export function instructionIndexAt(script: LoadedScript, byteOffset: number): number | null {
  const i = script.indexByByteOffset.get(byteOffset);
  return i === undefined ? null : i;
}

/** label 操作数（`raw_data`）→ 绝对字节偏移（**唯一**一处换算） */
export function labelByteOffsetOf(script: LoadedScript, rawData: number): number {
  return script.headerLen + ((rawData >>> 0) << 2);
}

/** 头的 `local_vars` 六个数（**按头里的字段顺序**，不做"哪个数喂哪个池"的猜测） */
export function localVarCounts(header: Header): {
  localInteger1: number; localFloats: number; localStrings1: number;
  localInteger2: number; unknownData: number; localStrings2: number;
} {
  const f = header.fields;
  return {
    localInteger1: f.local_integer_1, localFloats: f.local_floats, localStrings1: f.local_strings_1,
    localInteger2: f.local_integer_2, unknownData: f.unknown_data, localStrings2: f.local_strings_2,
  };
}

/**
 * 头的 6 个 local 声明 → **按池序**的计数数组（第 `i` 项 = `LOCAL_POOLS[i]` 的计数）。
 *
 * ★★ 凭什么敢按**位置**对应（而不是"猜哪个数喂哪个池"）：装载器里那 6 处 store 的**落点顺序**
 *   就是池序 —— 判据（语料 + 守卫，锚 = EA 在 `layout.mts` 的 `LOCAL_POOL_SLOTS` 头注里）：
 *   第 `i` 处「读计数（读缓冲 `+8+4i`）→ `operator new[]` → 写基址」的计数槽是 `记录+0x08+4i`（绝对 `0x5D89C+4i`），
 *   而 `记录+0x08+4i` ↔ `LOCAL_POOL_SLOTS[i]` ↔ operand type `9+i` 两两对上
 *   （守卫：`tools/test/emulator-model.test.mjs` 的「脚本头 6 个 local 声明 … 位置对应」用例）。
 *   ⇒ 本函数的**位置对应**不是本层的假设，它由那条语料守卫钉住；改了这里的顺序，那条会红。
 */
export function localCountList(header: Header): number[] {
  const c = localVarCounts(header);
  return [c.localInteger1, c.localFloats, c.localStrings1, c.localInteger2, c.unknownData, c.localStrings2];
}
