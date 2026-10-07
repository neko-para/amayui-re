/**
 * `model/float-bits.ts` —— **float32 位模式 ↔ JS 数值**（一份真源）
 *
 * ## 为什么它单独成一个文件
 * 它被**两层**同时需要：
 * * `vm/operand.ts`（操作数层：立即数 type 1 的 raw 就是 float32 位模式；浮点池落盘要收敛成单精度）；
 * * `model/pools.ts`（池层：float 族的格内容就是 **float32 位模式**，迁移到区域之后要在边界换算）。
 *
 * ⛔ 不能让 `model/` 去 import `vm/`（分层不许反向），所以这一对函数放在 `model/`，
 * 由 `vm/operand.ts` 引用并**再导出**（原来它就定义在那边 —— 那会逼着池层反向依赖）。
 *
 * ★ 用 `DataView` 显式小端：写进去再读出来走**同一端序** ⇒ 结果与宿主字节序无关
 * （`Float32Array` 直接取 `buffer` 会带上宿主端序，在大端机上就错了）。
 * ★ 收敛口径：浮点池落的是 **4 字节单精度**（取证：`fstp dword ptr`）—— `asFloat32` 就是这条。
 */

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
