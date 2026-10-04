/**
 * tools/lib/ui-bake/age-format.mjs —— **纯工具**：把 `packages/age-format` 的入口收敛到一处
 *
 * 为什么要这一层：`tools/` 的分层规则（`tools/README.md` §0，由 `tools/test/layering.test.mjs` 守）
 * 要求**每个 CLI 只 import `./lib/*`** —— CLI 不直接伸手到 `packages/`。
 * 于是"格式包有哪些函数可用"在这里一次性声明，CLI 与配方模型都从这里取。
 *
 * 方向：`tools/lib/**` → `packages/age-format`（包是更底层的格式层，不是 CLI）。
 */
export { readAlf, indexEntries } from '../../../packages/age-format/src/alf.mjs';
export {
  readAgfBuffer,
  decodeRgba,
  writeAgf,
  encodeBody,
  alphaFromRgba,
  packSection,
  parseWhBpp,
  extractPaletteRgb,
} from '../../../packages/age-format/src/agf.mjs';
