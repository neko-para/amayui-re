/**
 * apps/emulator/src/vm/operand.ts —— **操作数的读写**（★ 核心层：零 Node 依赖）
 *
 * 一条指令的每个操作数都是 `{ type, raw_data }`：`type` 说"这是什么种类"（立即数 / 某个池的第几号
 * 槽 / 字符串…），`raw_data` 说"具体是哪一个"；把这一对变成"内存里的那个位置"（并读写它）
 * 就是本模块的全部职责。
 *
 * ★★ 值的口径：**int 族一律按 u32 看**（`>>> 0`），**符号由用它的那条指令决定** ——
 *   引擎的池槽是 dword，`sub (local-int 0) 0 1` 的结果在盘上/内存里就是 `0xFFFFFFFF`；
 *   例如 `set-texture` 的颜色操作数是"`< 0` 就取 0"，那一步才做 `| 0`。
 *   ⇒ 同一个位模式只有一种表示（把 `-1` 与 `0xFFFFFFFF` 都放进模型 ⇒ 比较与快照都会分叉）。
 *
 * ★ 未初始化 ⇒ `null`（不是 0）。口径与理由见知识台账：`data/ledger/`
 *   （域 `Emulator`，subject `model/pools-capacity-unknown-sparse`）。
 * ★ 本批**没有取证**的那些 type ⇒ **响亮失败**，不许猜。口径与理由见知识台账：`data/ledger/`
 *   （域 `Emulator`，subject `vm/operand-unverified-types-loud`）。
 *   ★ 名单 = `UNVERIFIED_OPERAND_TYPES`（`type 2` + 高位种类 `0x8003/0x8005/0x8009/0x800B`）。
 *   ★ 判据：LOGO.BIN 从第 1 条到 `play-movie` 用到的 type 只有 `0`、`3`、`4`、`9`
 *     （立即数 + global-int + global-float + local-int），**上列缺口一条都不碰**。
 */

import type { InstrArg } from '../model/iterate.ts';
import type { GlobalPools, LocalPools, SlotValue } from '../model/pools.ts';
import { LOCAL_POOLS, localPoolByTypeTag, localPoolTypeTags } from '../model/pools.ts';
import type { LoadedScript } from './script.ts';
import { inlineString } from './script.ts';
import type { AddressSpace } from '../model/address-space.ts';
import { decInt } from '@amayui/age-format/src/asm/value-codec.mts';

/** 一个操作数读出来的值（int 族是 u32，字符串是 `string`） */
export type OperandValue = number | string;

/** 读的结果：值 + 诊断信息（"这是哪种东西""落在哪个槽"） */
export interface OperandRead {
  /** `null` = 这个槽还没被写过（稀疏模型里"没有"，不是"值是 0"） */
  value: OperandValue | null;
  /** 语义类别（日志与诊断用；**不是**宿主地址） */
  kind: string;
  /** 落点的语义描述，如 `local-int[0]` / `immediate` —— **只有语义名，没有偏移** */
  where: string;
}

/** 读操作数要知道的三件事（都是注入的；本模块自己不持状态） */
export interface OperandContext {
  locals: LocalPools;
  globals: GlobalPools;
  script: LoadedScript;
  /**
   * ★ **地址空间**（ADR 第 ② 步）：指针族要"取址 / 解引用"，两者都只能在这个空间里做。
   * ⛔ 不要求它是可选的：少了它，指针族只能"抛"或"猜" —— 显式要求它，编译期就把话说清。
   */
  space: AddressSpace;
}

/**
 * `type` → global 池的**语义名**。
 *
 * ★ 这两边（`3..8` 与 `GLOBAL_POOL_NAMES` 的下标）必须一一对上，
 *   由守卫 `tools/test/emulator-operand.test.mjs` 核 —— 对不上的症状是
 *   "读到了**另一个池**的值"（类型对、值错，且不报错）。
 */
export const GLOBAL_POOL_BY_TYPE_TAG: Record<number, string> = {
  3: 'int',
  4: 'float',
  5: 'string',
};

/**
 * ★★ **需要地址空间**才能实现的 type：池里那一格装的是**指针**，值在 `[ptr]` 里。
 * `6` global-ptr · `7` global-float-ptr · `8` global-string-ptr ·
 * `c` local-ptr · `d` local-float-ptr · `e` local-string-ptr
 * ★ 把它们当普通池读，**类型没错、值全错，而且不会报错** —— 所以这里必须抛。
 */
export const POINTER_OPERAND_TYPES: readonly number[] = [0x6, 0x7, 0x8, 0xc, 0xd, 0xe];

/** 本批**已实现**的操作数 type：立即数（0/1）+ 值就是值的池（3/4/5 + 9..14 里的非指针） */
export const IMPLEMENTED_OPERAND_TYPES: readonly number[] = [
  0, 1, 3, 4, 5,
  ...localPoolTypeTags().filter((t) => !POINTER_OPERAND_TYPES.includes(t)),
];

/** 本批**未取证/未建模**的 type（不是"坏数据"，是"我们还没读它"） */
export const UNVERIFIED_OPERAND_TYPES: readonly number[] = [2, 0x8003, 0x8005, 0x8009, 0x800b];

/** 一个 4 字节暂存：dword **位模式** ↔ float32（type 1 的浮点立即数、浮点池落槽） */
// ★ 位模式换算的**一份真源**在 `model/float-bits.ts`（池层也要用，而 `model/` 不许反向依赖 `vm/`）
//   ⚠ `export … from` 只再导出、**不进本地作用域** ⇒ 本文件自己用还得 import 一次。
import { asFloat32, bitsFromFloat, floatFromBits } from '../model/float-bits.ts';
export { asFloat32, bitsFromFloat, floatFromBits };

/** 未取证的 type ⇒ 抛（错误消息必须说清"这是没取证，不是坏数据"） */
function unverified(type: number): never {
  throw new Error(
    `操作数 type ${type === 2 ? '2（镜像内联字符串：数值读与文本读不自洽，本批未钉死）' : `0x${type.toString(16)}（高位种类：池里装的是容器句柄，带懒分配 + 登记）`} ` +
    `本批**未取证/未建模** —— 这不是坏数据，是我们还没读它。见 apps/emulator/src/vm/operand.ts 的文件头与需求树`,
  );
}

/** 需要地址空间的 type ⇒ 抛（**不许静默降级**：当普通池读会"类型对、值错"且不报错） */
function unimplementedPointerType(type: number): never {
  const what: Record<number, string> = {
    0x6: 'global-ptr（全局指针池 → 解引用）',
    0x7: 'global-float-ptr（全局浮点指针池 → 解引用）',
    0x8: 'global-string-ptr（全局字符串指针池 → 解引用）',
    0xc: 'local-ptr（局部指针池 → 解引用）',
    0xd: 'local-float-ptr（局部浮点指针池 → 解引用）',
    0xe: 'local-string-ptr（局部字符串指针池 → 解引用）',
  };
  throw new Error(
    `操作数 type 0x${type.toString(16)} = ${what[type]} 需要**地址空间**才能实现（值在 [ptr] 里，不在池格内）。` +
    `本仓的池模型是语义槽（number|string）、没有地址空间 ⇒ 拒绝按普通池读（那会"类型对、值错"且不报错）`,
  );
}

/**
 * 读一个操作数。
 *
 * @param ctx 池与脚本（都是注入的）
 * @param arg 操作数（`{ type, raw_data }`）
 * @param index 它是第几个（0 起；诊断用）
 */
export function readOperand(ctx: OperandContext, arg: InstrArg, index: number): OperandRead {
  const type = arg.type;
  const raw = arg.rawData >>> 0;

  if (type === 0) {
    // 立即数：**按 u32 归一化**（符号由用它的指令决定，见文件头）
    return { value: raw, kind: 'immediate', where: `immediate#${index}` };
  }
  if (type === 1) {
    // 浮点立即数：raw 那 4 字节**就是 IEEE754 float32 的位模式**（取证：`fld dword ptr [p+8k]`）
    return { value: floatFromBits(raw), kind: 'immediate.float', where: `immediate-float#${index}` };
  }

  // ★★ 指针族（本批只实现了存 4 字节整数的两支：0x6 全局 / 0xc 局部）。
  //   语义（逐字，锚 = EA）：`sub_41BF50` 的 case 12（局部 ptr，`0x41C049`）
  //   ```
  //     mov eax,[ecx+esi*8+5D8C0h]   ; ptr 池基址
  //     mov edx,[eax+edx*4]          ; 取第 idx 格 —— ★ **不 DEC**：格内容就是**地址**
  //     mov eax,[edx]                ; ★ 解引用
  //     rol 0Bh / xor [5EC8Ch] / ror 19h   ; ★ DEC 作用于**解引用出来的** dword
  //   ```
  //   ⇒ 读 = 取地址 → 解引用 → DEC。（对照 case 9 的 int：取格后**直接** DEC ⇒ 见 `LOCAL_POOLS` 的 `encoded` 订正。）
  // ★★ 字符串指针族（`0x8` 全局 / `0xe` 局部）：取格（地址）→ **定位字符串元素** → 返回那个 JS 字符串。
  //   实测：`0xe` 在 104 份脚本里出现 **1446** 次（`0x8` 一次都没有 ⇒ 一起实现，代价为零）。
  //   逐字对照（**`sub_41B640` 的 case 14**，`.lst:40896-40903`，`0x41B92D`；同形副本 `sub_41B9B0` `0x41BCFD`）：
  //   取格 → 解引用 → 按 **28 字节**格取文本。
  //   ★ 订正出处：`sub_41BF50` **没有 case 14** —— `.lst:41465 cmp edx,0Dh` / `ja def_41BF99`
  //   ⇒ type `0xe` 走 default 抛（`.lst:41726`）。行为不变，只是原先的出处写错了。
  if (type === 0x8 || type === 0xe) {
    const isLocal = type === 0xe;
    const addr = (isLocal ? ctx.locals.read(type, raw) : ctx.globals.read('stringRef', raw)) as number | null;
    const where = `字符串指针 type 0x${type.toString(16)} 第 ${raw} 格`;
    return { value: stringElementAt(ctx, (addr ?? 0) >>> 0, where), kind: 'pointer.string', where };
  }

  if (type === 0x6 || type === 0xc) {
    const isLocal = type === 0xc;
    const addr = (isLocal ? ctx.locals.read(type, raw) : ctx.globals.read('intRef', raw)) as number | null;
    // 未写过 ⇒ 引擎那边是 `initZero` 的 0 ⇒ 解引用地址 0（**会响亮失败**：0 不在任何区域里）
    const target = (addr ?? 0) >>> 0;
    // ★★ 解引用前**按需增长**（决策 `REQ-01M4B969TBWVERFCB1MXS2Q2E1`）：
    //   地址可能落在区域的**窗口内**却超出它**当前**的 `byteLength`（实测：`global:int[1353969]`
    //   的地址 `0x1052a3c4` —— 大下标数组还没被写过）⇒ 不增长就会抛"不落在任何区域里"。
    //   增长由 `Machine` 的钩子逐条留痕（`system.region.grow`），所以这**不是**静默扩容。
    ctx.space.ensureAddress(target, `指针 type 0x${type.toString(16)} 第 ${raw} 格 → 解引用前的增长`);
    const cell = ctx.space.readU32(target, `指针 type 0x${type.toString(16)} 第 ${raw} 格 → 解引用`);
    if (cell === null) return { value: null, kind: 'pointer.unwritten', where: `ptr[${raw}]→0x${target.toString(16)}` };
    return { value: decInt(cell, isLocal ? ctx.locals.key : ctx.globals.key), kind: 'pointer.deref', where: `ptr[${raw}]→0x${target.toString(16)}` };
  }

  if (POINTER_OPERAND_TYPES.includes(type)) unimplementedPointerType(type);

  const globalName = GLOBAL_POOL_BY_TYPE_TAG[type];
  if (globalName) {
    const v = ctx.globals.read(globalName, raw);
    return { value: v, kind: `global.${globalName}`, where: `global-${globalName}[${raw}]` };
  }

  const def = localPoolByTypeTag(type);
  if (def) {
    const v = ctx.locals.read(type, raw);
    return { value: v, kind: `local.${def.name}`, where: `local-${def.name}[${raw}]` };
  }

  unverified(type);
}

/**
 * 写一个操作数。**没有返回值**：写下去的位模式可以事后从池里读回来
 * （多一条"写回显"等于把同一件事写两遍）。
 *
 * ★ 立即数、字符串、高位种类**不可写**：引擎的写路径要一个真实落点，
 *   而这几种没有（至少本批没取证）。⇒ 抛，不静默丢弃 ——
 *   "脚本以为写进去了、实际丢了"是最难查的一类静默错误。
 */
export function writeOperand(ctx: OperandContext, arg: InstrArg, index: number, value: OperandValue): void {
  const type = arg.type;
  const raw = arg.rawData >>> 0;

  if (type === 0 || type === 1) {
    throw new Error(
      `操作数 #${index} 是立即数（type ${type}），**不是 lvalue**（引擎的取址原语只覆盖 3..14，遇到立即数抛类型异常）—— ` +
      `值 ${JSON.stringify(value)} 无处可写`,
    );
  }

  if (type === 0x8 || type === 0xe) {
    // ★ 字符串指针格也存**地址本身**（`stringPtr` 池 `encoded: false`）
    if (typeof value !== 'number') throw new Error(`操作数 #${index} 是字符串指针格，只能写地址（收了 ${JSON.stringify(value)}）`);
    if (type === 0xe) ctx.locals.write(type, raw, value >>> 0);
    else ctx.globals.write('stringRef', raw, value >>> 0);
    return;
  }

  if (type === 0x6 || type === 0xc) {
    // ★ 写指针格 = 存**地址本身**（`encoded: false` ⇒ 不过 ENC）—— 见读那一支的逐字。
    if (typeof value !== 'number') throw new Error(`操作数 #${index} 是指针格，只能写数字（收了 ${JSON.stringify(value)}）`);
    if (type === 0xc) ctx.locals.write(type, raw, value >>> 0);
    else ctx.globals.write('intRef', raw, value >>> 0);
    return;
  }

  if (POINTER_OPERAND_TYPES.includes(type)) unimplementedPointerType(type);

  const globalName = GLOBAL_POOL_BY_TYPE_TAG[type];
  if (globalName) {
    ctx.globals.write(globalName, raw, coerceToPool(globalName, value));
    return;
  }

  const def = localPoolByTypeTag(type);
  if (def) {
    ctx.locals.write(type, raw, coerceToPool(def.name, value));
    return;
  }

  unverified(type);
}

/**
 * 落池前按**目标池的类型**归一化。
 * ★ 判据（取证）：写原语按**目标槽位的声明类型**分派 —— 目标是 int 族就走 `__ftol2_sse`
 *   **截断**再 ENC；目标是浮点族就 `fstp dword`（**4 字节单精度**）。
 * ⇒ 所以 `float-mov` 之类把浮点写进 int 槽时**静默截断**，"float→float"只在 op1 属浮点族时成立。
 *   本函数把这件事显式化：**落池类型决定值的形状**，不是"谁算出来的"。
 */
function coerceToPool(poolName: string, value: OperandValue): SlotValue {
  if (typeof value === 'string') return value;
  // 浮点族的池名（global 侧 float/floatRef，local 侧 float/floatPtr）——
  // ★ "哪个池装浮点"从池定义派生，不另立一份
  const kind = LOCAL_POOLS.find((p) => p.name === poolName)?.kind
    ?? (poolName.toLowerCase().startsWith('float') ? 'float' : 'int');
  // ★ int 族：截断由 `encInt` 的 `>>> 0` 完成（ToUint32 本身就向零截断小数）
  return kind === 'float' ? asFloat32(value) : value;
}

/**
 * 把读出来的值当 **i32** 用（`0xFFFFFFFF → -1`）。
 * ★ 这是"用值的那条指令"的职责，不是操作数层的 —— 所以它在这里是一个**显式函数**，
 *   调用点一眼能看出"这里我确实要符号"。
 */
export const asInt32 = (v: OperandValue): number => (typeof v === 'string' ? Number(v) : v | 0);

/** 把读出来的值当 **u32** 用（`-1 → 0xFFFFFFFF`） */
export const asUint32 = (v: OperandValue): number => (typeof v === 'string' ? Number(v) >>> 0 : v >>> 0);

/** 把读出来的值当**浮点**用（int 立即数 → 浮点值；LOGO 里 `float-mov (global-float 9) 500` 就是这条） */
export const asFloat = (v: OperandValue): number => (typeof v === 'string' ? Number(v) : v >>> 0);

/**
 * **取址**（对应引擎的取址原语 `sub_42AEA0`，只覆盖 type 3..14）：操作数 → 它那一格的**地址**。
 * ★ 需要池已经迁到区域（ADR 第 ② 步）—— 没迁的池会**抛**（见 `LocalPools.regionOf`）。
 */
export function addressOfOperand(ctx: OperandContext, arg: InstrArg, index: number): number {
  const type = arg.type;
  const raw = arg.rawData >>> 0;
  const globalName = GLOBAL_POOL_BY_TYPE_TAG[type];
  if (globalName) return ctx.globals.regionOf(globalName).addressOf(raw);
  const def = localPoolByTypeTag(type);
  if (def) return ctx.locals.regionOf(type).addressOf(raw);
  throw new Error(`操作数 #${index} 的 type 0x${type.toString(16)} 不能取址（引擎的取址原语只覆盖 3..14）`);
}

/**
 * 地址 → **字符串元素**（字符串指针的解引用）。
 *
 * ★ 地址在这个池上只用来**定位元素**：`idx = (addr − 区域基址) / 28`
 *   （28 = 引擎的元素步长，不是随便取的）。口径与理由（字符串元素是**不透明的 JS 字符串**、
 *   不模拟 28 字节 `std::string`；决策 `REQ-01M4E07ZQ9S7EBA1SK0PREPY4E`）见知识台账：
 *   `data/ledger/`（域 `Emulator`，subject `model/pools-string-opaque`）。
 * ⛔ 指针指向的若不是**字符串池**的区域 ⇒ 抛（不许"顺手当成一个字符串"）。
 * ⛔ 没对齐 ⇒ 抛（`sub_41B640` 的 case 14 也是按 28 字节格取的；出处订正见 `readOperand` 里的同名注）。
 */
function stringElementAt(ctx: OperandContext, addr: number, where: string): SlotValue | null {
  const hit = ctx.space.regionByWindow(addr);
  if (!hit) throw new Error(`${where}：地址 0x${addr.toString(16)} 不落在任何区域窗口里`);
  if (hit.tag !== 'local:string' && hit.tag !== 'global:string') {
    throw new Error(
      `${where}：地址 0x${addr.toString(16)} 指向区域 \`${hit.tag}\` —— **不是字符串元素**` +
      `（字符串指针只能指向字符串池；字符串按裁决是不透明元素）`,
    );
  }
  const off = addr - hit.base;
  if (off % hit.elemBytes !== 0) {
    throw new Error(`${where}：地址 0x${addr.toString(16)} 没对齐到字符串元素边界（步长 ${hit.elemBytes}）`);
  }
  const idx = off / hit.elemBytes;
  return hit.tag === 'local:string' ? ctx.locals.read(11, idx) : ctx.globals.read('string', idx);
}

/** 浮点族的操作数 type（"浮点 → 文本"的格式**未取证** ⇒ 文本路遇到它们要抛） */
export const FLOAT_OPERAND_TYPES: readonly number[] = [1, 4, 0xa];

/**
 * **字符串池的两个 type**（全局 5 / 局部 11）。
 * ★ 它与 `GLOBAL_POOL_BY_TYPE_TAG`（`5: 'string'`）和局部池表里的 string 项**是同一件事的两处写法**；
 *   改了一边就要改另一边（`tools/test/emulator-host.test.mjs` 钉着那张 type→池 的表）。
 */
export const STRING_POOL_TYPES: readonly number[] = [5, 0xb];

/**
 * ★ **按"文本"语义读一个操作数** —— 对应引擎的**另一个**取值原语（`sub_42A420`），
 * 与 `readOperand`（`sub_41BF50` 那一族，整型语义）**不是同一件事**。
 *
 * 分流：
 * * `type 2`（内联字符串）⇒ `inlineString`（判据见 `vm/script.ts` 的头注：
 *   文件偏移 = `headerLen + 4*raw`、字节 `^0xFF`、cp932、到 0 止）；
 * * 字符串池（type 5 / 11 …）⇒ 取格子里那个字符串；
 * * 整型族 ⇒ **十进制文本**（引擎在这一路用 `_itoa_s` 的 `%d`）。
 *
 * ⛔ **浮点族不在这里**：引擎对浮点源转文本用的格式串（`%f` 还是别的）**没有取证**
 * ⇒ 遇到就抛，不猜（猜出来的格式会让字符串内容静默错，而日志看着一切正常）。
 */
export function readOperandAsText(ctx: OperandContext, arg: InstrArg, index: number): string {
  if (arg.type === 2) return inlineString(ctx.script, arg.rawData >>> 0);
  if (FLOAT_OPERAND_TYPES.includes(arg.type)) {
    throw new Error(
      `操作数 #${index} 是浮点（type ${arg.type}），而"浮点 → 文本"的格式**未取证**` +
      `（引擎这一路的格式串没有逐字确认）⇒ 拒绝猜一个格式`,
    );
  }
  const r = readOperand(ctx, arg, index);
  if (r.value === null) {
    // 未写过的格子：**字符串池 ⇒ 空串**（引擎那边 28 字节元素的 size = 0）；
    // **整型池 ⇒ "0"**（装载期被填了 `encZero`，DEC 之后就是 0）。两者都不是"不知道"。
    return STRING_POOL_TYPES.includes(arg.type) ? '' : '0';
  }
  if (typeof r.value === 'string') return r.value;
  return String(asInt32(r.value));
}
