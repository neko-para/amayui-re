/**
 * opcode → 实现。★ 当前**故意为空**：`CORE-OPS` 还没有任何一条 opcode 语义成篇。
 * 未实现即抛错（不 no-op）——这是"spec 缺口"的机器表示，和 `data/spec` 的残差是同一套 id。
 */
export interface Machine {
  ip: number;
  operandCount: number;
}

export const HANDLERS = new Map<number, (m: Machine) => void>();

export function unimplemented(opcode: number): never {
  throw new Error(`unimplemented opcode 0x${opcode.toString(16)}`);
}
