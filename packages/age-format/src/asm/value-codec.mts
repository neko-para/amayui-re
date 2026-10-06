/**
 * packages/age-format/src/asm/value-codec.mjs —— AGE 脚本运行时的**整数值编解码**（DEC / ENC）
 *
 * ## 这是什么
 * 引擎的**整型族**槽（全局/局部的 int 池、指针目标、数组元素）在内存里存的是
 * **编码后的位模式**，不是原值；读要过 DEC、写要过 ENC。**float 族不过编解码**（裸 `float32`）。
 * ⇒ 所以"池模型"不可能是一个纯数组：**没有 key 就一个 int 槽都读不出来**。
 *
 * ## 公式（★ 机械取自语料，不是从旧仓文档抄的）
 * ```
 * DEC:  x → rol32(x, 11) ^ key → ror32(…, 25)      出处 .text:0040D500..0040D521
 *       语料逐字： rol eax,0Bh / xor eax,[ebx+5EC8Ch] / ror eax,19h
 * ENC:  v → ror32(v,  7) ^ key → rol32(…, 21)      出处 .text:004103B2..004103D5
 *       语料逐字： ror eax,7   / xor eax,[ebx+5EC8Ch] / rol eax,15h
 * ```
 * ★ 互逆的算术理由一眼可见：`7 + 25 = 32`、`11 + 21 = 32`（两个方向各自"转出去、转回来"）。
 *   守卫 `tools/test/engine-value-codec.test.mjs` 除了验公式，还**回到语料里验这两条指令序列在场** ——
 *   公式写在代码里、证据留在语料里，两者不许各说各的。
 *
 * ## key 是运行期状态（★ 因此本模块**没有默认 key**）
 * `key` = `Engine+0x5EC8C`（dword 索引 97059），由引擎初始化时写入：
 * 语料 `.text:00417359` = `mov [esi+5EC8Ch], edx`（`edx` 来自 `[ebp+var_14]`，**非立即数**）⇒
 * 每个进程一份、随启动而变。`enc_zero` = `Engine+0x5EC90` = `ENC(key, 0)`。
 * ⇒ 本模块**不提供默认值**：静默用 0 会把"我不知道 key"伪装成"key 是 0"。
 *
 * ## 索引**不过**编解码（只有"值"过）
 * 取址原语 `sub_42AEA0` 的 case 3 逐字是 `mov ecx,[eax]`（取 idx）→ `mov edx,[esi+5D800h]`（取池基址）
 * → `lea eax,[edx+ecx*4]`（`base + idx*4`）—— **下标直接乘 4，没有任何位运算**。
 */

/** 32 位循环左移（`n` 取模 32） */
export const rol32 = (x: number, n: number): number => {
  const s = ((n % 32) + 32) % 32;
  return ((x << s) | (x >>> (32 - s))) >>> 0;
};

/** 32 位循环右移（`n` 取模 32） */
export const ror32 = (x: number, n: number): number => {
  const s = ((n % 32) + 32) % 32;
  return ((x >>> s) | (x << (32 - s))) >>> 0;
};

/**
 * DEC —— 内存里的**编码位模式** → 原值。
 * @param x 从池里读出来的 dword（按 uint32 解释）
 * @param key `Engine+0x5EC8C`
 */
export const decInt = (x: number, key: number): number => ror32(rol32(x >>> 0, 11) ^ (key >>> 0), 25);

/**
 * ENC —— 原值 → 内存里的**编码位模式**（`decInt` 的逆）。
 * @param v 原值（按 uint32 解释）
 * @param key `Engine+0x5EC8C`
 */
export const encInt = (v: number, key: number): number => rol32(ror32(v >>> 0, 7) ^ (key >>> 0), 21);

/** `Engine+0x5EC90` 那个槽的语义：`ENC(key, 0)`（局部 int 池的初值就是它） */
export const encZero = (key: number): number => encInt(0, key);

/** 池/槽的索引换算：`base + idx*4`（★ 下标**不过**编解码） */
export const intSlotOffset = (idx: number): number => (idx >>> 0) * 4;

/** 本模块的公式与证据（供守卫/文档引用，避免两处各写一份） */
export const CODEC = {
  keyField: { name: 'Engine+0x5EC8C', dword: 97059 },
  encZeroField: { name: 'Engine+0x5EC90', dword: 97060 },
  dec: {
    shifts: [11, 25],
    xorBetween: true,
    sample: '.text:0040D500..0040D521',
    // ★ 逐字取自语料：key 先被搬到栈变量（`var_44`），所以 xor 的源是那个变量、不是内存操作数
    insns: ['rol eax,0Bh', 'xor eax,[ebp+var_44]', 'ror eax,19h'],
    keyLoad: 'mov eax,[ebx+5EC8Ch]',
  },
  enc: {
    shifts: [7, 21],
    xorBetween: true,
    sample: '.text:004103B2..004103D5',
    insns: ['ror eax,7', 'xor eax,dword ptr [ebp+ArgList]', 'rol eax,15h'],
    keyLoad: 'mov eax,[ebx+5EC8Ch]',
  },
  /** 取址侧不过编解码的逐字证据 */
  indexPath: { sample: '.text:0042AF2D', insns: ['mov ecx,[eax]', 'mov edx,[esi+5D800h]', 'lea eax,[edx+ecx*4]'] },
  /** int 池基址槽（operand type 3）；float 是 0x5D808 / 0x5D80C */
  intPoolBase: 'Engine+0x5D800',
  floatPoolBase: 'Engine+0x5D808',
};
