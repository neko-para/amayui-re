/**
 * apps/emulator/src/vm/operand.ts —— **操作数的读写**（★ 核心层：零 Node 依赖）
 *
 * ## 这一层是什么
 * 一条指令的每个操作数都是一个 `{ type, raw_data }` 对。`type` 说"这是什么种类的东西"
 * （立即数 / 某个池的第几号槽 / 字符串…），`raw_data` 说"具体是哪一个"。
 * 把 `(type, raw_data)` 变成"内存里的那个位置"（并且读写它）就是本模块的全部职责。
 *
 * ## ★★ 值的口径：**int 族一律按 u32 看，符号由用它的那条指令决定**
 * 引擎的池槽是 dword。`sub (local-int 0) 0 1` 的结果是 `0xFFFFFFFF`（不是 `-1`）——
 * 因为它在盘上/内存里就是那 32 个位。而**用到它的指令**才决定符号：例如 `set-texture` 的
 * 颜色操作数是"`< 0` 就取 0"，那一步才做 `| 0`。
 * ⇒ 本层**归一化到 u32**（`>>> 0`），于是"同一个位模式只有一种表示"。
 *   把 `-1` 和 `0xFFFFFFFF` 都放进模型里 = 同一件事两种写法 ⇒ 比较与快照都会分叉。
 *
 * ## ★ 未初始化 ⇒ `null`（不是 0）
 * 池模型是稀疏的（容量未知，不许编一个数出来）。读到一个**没有的槽**返回 `null`，
 * 由调用方决定怎么办（记一笔 `oob`，再按引擎初值语义处理）。★ 不许在这里补 0：
 * int 族的初值是 `ENC(key,0)`（**非 0 的位模式**），"补 0"会把
 * "我不知道这里有什么"伪装成"这里就是 0"。
 *
 * ## ★ 本批**没有**取证的那些 type ⇒ **响亮失败**，不许猜
 * `0x8003 / 0x8005 / 0x8009 / 0x800B`（高位种类）、`type 1`（float 立即数）、
 * `type 2`（字符串，要码页解码器）都在本批之外。遇到它们**抛**，
 * 错误消息里说清"这是未取证，不是坏数据" —— 静默当成某个已知种类是本仓最贵的一类错。
 * ★ 判据：LOGO.BIN 从第 1 条到 `play-movie` 用到的 type 只有 `0`、`3`、`4`、`9`
 *   （立即数 + global-int + global-float + local-int），**上列缺口一条都不碰**。
 */

import type { InstrArg } from '../model/iterate.ts';
import type { GlobalPools, LocalPools, SlotValue } from '../model/pools.ts';
import { LOCAL_POOLS, localPoolByTypeTag, localPoolTypeTags } from '../model/pools.ts';
import type { LoadedScript } from './script.ts';

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
const F32 = new DataView(new ArrayBuffer(4));

/** dword 位模式 → float32（写成读回同一端序 ⇒ 与宿主端序无关） */
export const floatFromBits = (bits: number): number => {
  F32.setUint32(0, bits >>> 0, true);
  return F32.getFloat32(0, true);
};

/** float32 → dword 位模式 */
export const bitsFromFloat = (v: number): number => {
  F32.setFloat32(0, v, true);
  return F32.getUint32(0, true);
};

/** 收敛成 float32（浮点池落的是 **4 字节单精度** —— 取证：`fstp dword ptr`） */
export const asFloat32 = (v: number): number => floatFromBits(bitsFromFloat(v));

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
