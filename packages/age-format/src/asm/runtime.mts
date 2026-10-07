/**
 * packages/age-format/src/asm/runtime.mts —— **运行期子集入口**
 *
 * ## 谁该 import 这个
 * **模拟器的核心**（将来要跑在浏览器里的那一半）。它要的是"认头 + 按 argc 切边界 + 查指令表 +
 * 比字节"，**不需要**反汇编器 / 重汇编器（那是工具侧的事，体积也白搭）。
 *
 * ## ★ 这里的分界线是**范围**，不是平台
 * 早先这条线画在"谁碰 `node:fs`"上：`opcodes.mts` 曾在模块顶层 `import fs` 读 `instruction-set.json`，
 * 于是 `import { readHeader } from '…/asm/index.mts'` 会把 `node:fs` 拖进 bundle。
 * ★ **那条根因已经拔掉了**：指令表改成 **ESM JSON 模块 import**（`opcodes.mts` 的 `OPCODE_TABLE`）
 * ⇒ 现在 `src/asm/**` **一个 `node:` 依赖都没有**（判据：`rg 'node:' packages/age-format/src/asm/` 为空）。
 * ⇒ 所以本文件不再是"前端唯一能 import 的那半"，而是"**核心只需要这些**" —— 少拉三个模块进 bundle。
 *
 * ## ★ 本入口仍然**不含** `codec.mts`（理由：范围）
 * 核心目前只切边界、不解字符串（`iterate.ts` 不解释 operand 的文本）。★ 类型上**没有障碍了**：
 * `apps/emulator/tsconfig.json` 的 `lib` 里带了宿主库 **`WebWorker`**，`TextDecoder` / `URL` /
 * `AbortController` / `structuredClone` 这批判 WHATWG 通用类型都拿得到（实测：把 `codec.mts` 拉进本工程闭包 ⇒ exit 0）。
 * 等文本层真要用它，在下面加一行 re-export 即可。
 */
export {
  bytesEqual, concatBytes, copyBytes, decodeLatin1, encodeLatin1, toBytes, ByteReader, ByteWriter,
} from './bytes.mts';

export { readHeader, S5_SIG4, HEADER_LEN_V4, HEADER_LEN_V5 } from './header.mts';
export type { ByteSource, ByteView, Header, HeaderFields } from './header.mts';

export {
  OPCODE_TABLE, buildOpcodeTable, instructionForOpCode, instructionForLabel, instructionForToken,
  opcodeLabel, normalizedOpcode, instructionByteLength,
} from './opcodes.mts';
export type { OpcodeDef, OpcodeTable } from './opcodes.mts';

export {
  getTypeLabel, getType, isControlFlowOpcode, isArrayOpcode, isLabelArgument,
  FIELD_OFFSETS, FIELD_NAMES, fieldBlockShift, hex, labelHex,
} from './types.mts';
