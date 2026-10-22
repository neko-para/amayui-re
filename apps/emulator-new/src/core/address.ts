/**
 * apps/emulator-new/src/address.ts —— **合成地址空间**（最小、够用、确定性）
 *
 * ## 为什么需要它
 * 指针族的槽里装的是**地址**（`40-pools.md` 的「全局引用族」「局部池族」），而 `0x61 lookup-array`
 * 做的是**地址算术**：`地址 ← 数组首格地址 + 元素宽度 × 下标`（逐字：`sub_418CC0` 的 switch，见下）。
 * ⇒ 模型里必须存在"地址"这个概念，且同一批输入必须给出同一批地址（否则快照/复算不可比）。
 *
 * ## 与规范的关系（★ 回答"这些地址值是特殊的还是随意的"）
 * * **规范只规定关系，不规定数值**：`40-pools.md` 的「下标不做上界检查」写的是
 *   "地址由'基址 + 下标 × 元素宽'直接算出"；`55-config.md` 的「脚本访问」明写
 *   "地址的数值本身不规定，只用地址差"（配置键换算）。⇒ 基址取什么值**不是**语义。
 * * 因此本文件的基址是**合成的实现选择**（不是引擎真实地址，也不是"特殊值"）：
 *   只要满足 ① 每池"基址 + 元素宽×下标"、② 元素宽 4 / 28、③ 用**地址差**换算出的键与规范一致、
 *   ④ 取址→解引用能回到同一格、⑤ 地址装得进 4 字节槽，就与规范相容。
 * * ⛔ **不是**规范要求、但这里做了选择的部分：具体基址、窗口大小、池的先后顺序（全局在局部之下）、
 *   对齐检查，以及"未映射 / 未对齐 ⇒ 响亮失败"（规范说越界是 **UB**，没说可以复刻）。
 * * ⚠ 若某脚本让**绝对地址**变得可观察（比较两个无关指针 / 把地址当整数比较或打印），
 *   它的行为本来就是**机器相关**的：本文件的选择会给出一组自洽但可能与真机不同的数值。
 * * ★ **配置键不是"地址值"**：`save-int`/`load-int` 的键是**槽的身份** —— 值族（3/5）= 操作数自带的池下标；
 *   引用 / 指针族（6/8/12/14）= 用**地址差**把同一个身份反算回来（逐字 `sub_418A30`：
 *   取指针槽内容、减全局 int 池基址、`sar 2`；字符串版减全局串池基址、除以 28）。
 *   只有指针**指向非全局池**（例如局部池）时，反算结果才不等于池下标 —— 那是规范明写的
 *   "以全局池起点为基准的地址序号"（`55-config.md` 的「脚本访问」），是边界情形，不是常见路径。
 *
 * ## 形状（三条口径，都是旧的「按语义槽建模」做不到而必须做的）
 * 1. **扁平**：`地址 = 池基址 + 元素宽度 × 下标` —— 引擎的 `base + 4*i` / `lea` 原样成立。
 * 2. **合成**：基址是**固定分区**（不是引擎里的真实地址；真实地址不是可观察语义）：
 *    - 全局池：`0x10000000` 起，每池一个 `0x04000000` 窗口，顺序 3/4/5/6/7/8；
 *    - 局部池：`0x40000000` 起，**每帧**一个 `0x00800000` 窗口，帧内六池各占 `0x00100000`。
 *    ⇒ 40 帧 × 8 MiB + 6 × 64 MiB 仍在 u32 内（指针槽只有 4 字节）。
 * 3. **稀疏**：地址只是算术；格子有没有被写过由池（`Map`）回答。**没写过 ≠ 值是 0**
 *    （整型池在装载期被填 `ENC(key,0)` 是另一回事，见 `values.ts`）。
 *
 * ## 元素宽度（这是 `0x61` 的**判据**，不是实现细节）
 * 逐字（`sub_418CC0` 的 switch，case = 落点类型标签）：
 * * 6 / 7 / 12 / 13（整型 / 浮点 的引用 / 指针）⇒ `base + 4 × idx`
 * * 8 / 14（字符串引用 / 字符串指针）⇒ `base + 28 × idx`（28 = 字符串记录宽度）
 * * 其余（含值族 9/10/11）⇒ 抛「操作数类型异常」
 *
 * ★ 越界 / 未对齐 / 落在窗口外 / 指向字符串记录却按整型读 ⇒ **响亮失败**（引擎那里是 UB，不是语义）。
 */

/** 池身份（= 操作数类型标签；名字只是可读别名） */
export type PoolTag = 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14;

/** 指针族：这些标签的**槽里装地址**（6/7/8 全局，12/13/14 局部） */
export const POINTER_TAGS: readonly PoolTag[] = [6, 7, 8, 12, 13, 14];

export const isPointerTag = (tag: number): tag is PoolTag => (POINTER_TAGS as readonly number[]).includes(tag);

/** 全局池窗口起点与跨度（按 tag 顺序排：3/4/5/6/7/8） */
const GLOBAL_ORIGIN = 0x10000000;
const GLOBAL_SPAN = 0x04000000;
const GLOBAL_ORDER: readonly PoolTag[] = [3, 4, 5, 6, 7, 8];

/** 局部池：每帧一个窗口，帧内六池各占 `LOCAL_POOL_STRIDE` */
const LOCAL_ORIGIN = 0x40000000;
const LOCAL_FRAME_SPAN = 0x00800000;
const LOCAL_POOL_STRIDE = 0x00100000;
const LOCAL_ORDER: readonly PoolTag[] = [9, 10, 11, 12, 13, 14];

/** 帧区容量（与 `machine.ts` 的 `FRAME_SLOT_COUNT` 一致；这里只用于地址换算的界） */
const MAX_FRAMES = 40;

/** 一格的字节数：字符串值池的记录是 28，其余（数值 / 指针槽）是 4 */
export function slotBytes(tag: PoolTag): number {
  return tag === 5 || tag === 11 ? 28 : 4;
}

/**
 * `0x61` / `0x12c` 的**落点**给出的数组元素宽度。
 * ★ 与"落点槽自己的宽度"无关：字符串**指针**的槽是 4 字节，但它指向的记录是 28 字节。
 */
export function arrayStrideFor(tag: number): number {
  switch (tag) {
    case 6: case 7: case 12: case 13: return 4;
    case 8: case 14: return 28;
    default:
      throw new Error(
        `lookup-array 的落点必须是引用 / 指针族（6/7/8/12/13/14），实际 type 0x${tag.toString(16)} —— ` +
        '引擎在这些 case 上抛「操作数类型异常」（`sub_418CC0` 的 default 分支）',
      );
  }
}

/** 池基址（`frameSlot` 只对局部池有意义） */
export function poolBase(tag: PoolTag, frameSlot = 0): number {
  const gi = GLOBAL_ORDER.indexOf(tag);
  if (gi >= 0) return (GLOBAL_ORIGIN + gi * GLOBAL_SPAN) >>> 0;
  const li = LOCAL_ORDER.indexOf(tag);
  if (li < 0) throw new Error(`不是合法的池标签：0x${tag.toString(16)}`);
  if (!Number.isInteger(frameSlot) || frameSlot < 0 || frameSlot >= MAX_FRAMES) {
    throw new Error(`局部池的帧号越界：${frameSlot}（帧区只有 ${MAX_FRAMES} 槽）`);
  }
  return (LOCAL_ORIGIN + frameSlot * LOCAL_FRAME_SPAN + li * LOCAL_POOL_STRIDE) >>> 0;
}

/** 某一格的地址 */
export function slotAddress(tag: PoolTag, index: number, frameSlot = 0): number {
  if (!Number.isInteger(index) || index < 0) throw new Error(`池下标必须是非负整数：${index}`);
  return (poolBase(tag, frameSlot) + slotBytes(tag) * index) >>> 0;
}

/** 一个地址解析出来的落点 */
export interface Resolved {
  tag: PoolTag;
  /** 池内下标 */
  index: number;
  frameSlot: number;
}

/**
 * 地址 → (池, 下标)。
 * ★ 三类失败都**不许**补 0：落在任何窗口外 / 没对齐到元素边界 / 帧号越界 ⇒ 抛。
 */
export function resolveAddress(addr: number): Resolved {
  const a = addr >>> 0;
  if (a >= LOCAL_ORIGIN) {
    const off = a - LOCAL_ORIGIN;
    const frameSlot = Math.floor(off / LOCAL_FRAME_SPAN);
    if (frameSlot >= MAX_FRAMES) throw new Error(`地址 0x${a.toString(16)} 落在帧区之外（帧号 ${frameSlot}）`);
    const rel = off - frameSlot * LOCAL_FRAME_SPAN;
    const li = Math.floor(rel / LOCAL_POOL_STRIDE);
    const tag = LOCAL_ORDER[li];
    const within = rel - li * LOCAL_POOL_STRIDE;
    return withinPool(tag, within, a, frameSlot);
  }
  if (a >= GLOBAL_ORIGIN) {
    const off = a - GLOBAL_ORIGIN;
    const gi = Math.floor(off / GLOBAL_SPAN);
    if (gi >= GLOBAL_ORDER.length) throw new Error(`地址 0x${a.toString(16)} 落在全局池窗口之外`);
    const tag = GLOBAL_ORDER[gi];
    return withinPool(tag, off - gi * GLOBAL_SPAN, a, 0);
  }
  throw new Error(`地址 0x${a.toString(16)} 不落在任何池窗口里（引擎那里是 UB，不是 0）`);
}

function withinPool(tag: PoolTag, within: number, addr: number, frameSlot: number): Resolved {
  const eb = slotBytes(tag);
  if (within % eb !== 0) {
    throw new Error(`地址 0x${addr.toString(16)} 没对齐到元素边界（池 0x${tag.toString(16)} 的元素宽度 ${eb}）`);
  }
  return { tag, index: within / eb, frameSlot };
}
