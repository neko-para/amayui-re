/**
 * `0x23b`（CG 数字条）的**纯几何**：把一次"按数字条画数值"翻译成要建的绘制项。
 *
 * ★ 为什么需要它：TITLE 的 `Version` 数字（`0.00.0000`）**不是**预烘焙的位图 ——
 *   TITLE.BIN 里 `i2da`（登记数字条记录）+ 三条 `i23b`（分别画 1 / 2 / 4 位）
 *   + 两条 `draw-texture`（画两个小数点）拼出来。`draw-texture 76/77` 的源矩形
 *   `1342,811,12,15` 是**小数点**格子；数字格子从记录里的 `x0 = 0x54c = 1356` 开始、
 *   步长 `cellWidth + innerGap = 12 + 2 = 14`。把 76/77 当成数字会画出两个小数点。
 * ★ 本文件只做纯数据（无 IO / 无 Machine / 无 Host）：
 *   `handlers.ts` 负责读操作数、删旧项、写 `scene.drawItems`；这里只算"该建哪些项"。
 *
 * 证据（锚 = EA；`.lst` 逐字）：
 * - handler：`0x23b → sub_424970`（`pnpm tools opcodes handlers --opcode 0x23b`）。
 * - 记录布局：`0x2da` 把 7 个整数写进 `Engine+388332+28*cgno`；消费方 `sub_424970` 读
 *   `[esi+4]`（x0）/ `[esi+8]`（y0）/ `[esi+0Ch]`（单字宽）/ `[esi+10h]`（字高）/
 *   `[esi+14h]`（字内空隙）/ `[esi+18h]`（字距）、`[esi]`（纹理槽）。
 * - 源矩形：`0x424CC3` 起 —— `srcX = x0 + (cellWidth + innerGap) * digit`，宽 = `cellWidth`，
 *   高 = `cellHeight`。
 * - 目标 x：三分支
 *   - `flags & 2`（居中，`0x424AC9`）：`x + k*advance - trunc((last - digitCount + 1) * advance / 2)`；
 *   - `flags & 4`（左对齐，`0x424BF8`）：`x + (digitCount - 1 - (last - k)) * advance`；
 *   - 否则（右对齐，`0x424CF2`）：`x + k*advance`。
 *   其中 `advance = cellWidth + 字距`，`k` 从 `digits-1`（个位）递减到 0，`last = digits-1`。
 * - 前导零：右对齐分支才看 `flags & 1`（`0x424CCE`）；居中 / 左对齐分支只看
 *   `k == last || 剩余值 != 0`（`0x424AA9` / `0x424BE4`）。个位那一位永远画。
 *
 * ★ 本文件**不**声称覆盖 `sub_424970` 的全部副作用：删 `[id, id+digits)` 的旧项由
 *   `handlers.ts` 做（引擎是 `sub_4ABB60`），删的是**四张同键表**（对象记录 / 网格 / 两张辅助表）。
 *   这里只算"每个被画出的位"的几何；**目标 x / y 由 handler 在每个位上各读一次**。
 */

/** 一条数字条记录（`0x2da` 的七个整数格，顺序与引擎记录一致） */
export interface CgDigitRecord {
  /** `[0]` 纹理槽（0 = 未登记 / 无效） */
  textureId: number;
  /** `[1]` 数字条左上角 x */
  x0: number;
  /** `[2]` 数字条左上角 y */
  y0: number;
  /** `[3]` 单字宽 */
  cellWidth: number;
  /** `[4]` 字高 */
  cellHeight: number;
  /** `[5]` 字内空隙（源矩形的格子步长 = 单字宽 + 它） */
  innerGap: number;
  /** `[6]` 字距（目标步长 = 单字宽 + 它；可为负） */
  advanceGap: number;
}

/**
 * 一个"将要画的位"：键 / 纹理槽 / 源矩形，以及**目标 x 相对 op4 的整数偏移**。
 * ★ 为什么把 x / y 拆出去：引擎在**每个被画出的位**的建项分支里各读一次 op4 / op5
 *   （`0x424ABE` / `0x424ACF`）；位数 ≤ 0 时那些分支一次都不进。
 */
export interface CgDigitSlot {
  /** 绘制键（从 `0x23b` 的 op1 起，按建项顺序逐个 +1） */
  handle: number;
  textureId: number;
  srcLeft: number;
  srcTop: number;
  srcRight: number;
  srcBottom: number;
  /** 目标 x 相对 op4 的偏移（引擎按整数算完再转 float；居中档也 **不会**出现 .5） */
  dstXOffset: number;
}

/** 一个要建的绘制项（字段与 `DrawItem` 的几何格一一对应） */
export interface CgDigitItem {
  /** 绘制键（从 `0x23b` 的 op1 起，按建项顺序逐个 +1） */
  handle: number;
  textureId: number;
  srcLeft: number;
  srcTop: number;
  srcRight: number;
  srcBottom: number;
  /** 目标位置（引擎按整数算完再转 float；居中档也 **不会**出现 .5） */
  dstX: number;
  dstY: number;
}

/** 把 `0x2da` 的原始七格转成有名字的记录（不校验，校验在 handler 里） */
export function cgDigitRecordOf(raw: readonly number[]): CgDigitRecord {
  return {
    textureId: raw[0] ?? 0,
    x0: raw[1] ?? 0,
    y0: raw[2] ?? 0,
    cellWidth: raw[3] ?? 0,
    cellHeight: raw[4] ?? 0,
    innerGap: raw[5] ?? 0,
    advanceGap: raw[6] ?? 0,
  };
}

/** 十进制位数（至少 1；引擎 `0x424A25` 起的除法循环，截断除法） */
function decimalDigits(value: number): number {
  let count = 1;
  for (let t = Math.trunc(value / 10); t !== 0; t = Math.trunc(t / 10)) count += 1;
  return count;
}

/**
 * `0x23b` 的几何：返回**每个被画出的位**（顺序 = 引擎建项顺序：个位 → 高位）。
 * ★ handler 拿这份计划逐位读 op4 / op5：计划为空（位数 ≤ 0）⇒ 一次都不读。
 *
 * @param record `0x2da` 的七格（调用方保证 `record[0] != 0`）
 * @param startId op1（起始绘制键）
 * @param value op3（要画的数值）
 * @param digits op6（位数；≤ 0 ⇒ 不建项，引擎在 `0x424D97` 直接返回）
 * @param flags op7（bit0 补零 / bit1 居中 / bit2 左对齐；都不设 = 右对齐）
 */
export function cgDigitSlots(
  record: readonly number[],
  startId: number,
  value: number,
  digits: number,
  flags: number,
): CgDigitSlot[] {
  const rec = cgDigitRecordOf(record);
  const last = digits - 1;
  if (last < 0) return [];
  const advance = rec.cellWidth + rec.advanceGap;
  const srcStride = rec.cellWidth + rec.innerGap;
  const digitCount = decimalDigits(value);
  const centered = (flags & 2) !== 0;
  const leftAligned = !centered && (flags & 4) !== 0;
  const zeroFill = (flags & 1) !== 0;
  const slots: CgDigitSlot[] = [];
  let remaining = value | 0;
  for (let k = last; k >= 0; k -= 1) {
    const digit = remaining % 10;
    const draw = centered || leftAligned
      ? k === last || remaining !== 0
      : zeroFill || k === last || remaining !== 0;
    if (draw) {
      const srcLeft = rec.x0 + srcStride * digit;
      let dstXOffset: number;
      if (centered) {
        dstXOffset = k * advance - Math.trunc(((last - digitCount + 1) * advance) / 2);
      } else if (leftAligned) {
        dstXOffset = (digitCount - 1 - (last - k)) * advance;
      } else {
        dstXOffset = k * advance;
      }
      slots.push({
        handle: startId + slots.length,
        textureId: rec.textureId,
        srcLeft,
        srcTop: rec.y0,
        srcRight: srcLeft + rec.cellWidth,
        srcBottom: rec.y0 + rec.cellHeight,
        dstXOffset,
      });
    }
    remaining = Math.trunc(remaining / 10);
  }
  return slots;
}

/**
 * `cgDigitSlots` + 固定的目标位置：纯函数单测用（handler 走 `cgDigitSlots` 逐位读 op4 / op5）。
 *
 * @param x op4 / @param y op5（目标位置）
 */
export function cgDigitItems(
  record: readonly number[],
  startId: number,
  value: number,
  digits: number,
  flags: number,
  x: number,
  y: number,
): CgDigitItem[] {
  return cgDigitSlots(record, startId, value, digits, flags).map(({ dstXOffset, ...slot }) => ({
    ...slot,
    dstX: dstXOffset + x,
    dstY: y,
  }));
}
