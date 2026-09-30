/**
 * tools/lib/paths.mjs — **路径常量**（纯数据，不依赖任何其它模块）
 *
 * 为什么单独一个文件：路径常量原先散在 4 个工具里，谁 import 谁就得连带 import 整个工具。
 * 现在它们是**纯事实**，任何一层（纯工具 / 领域模型 / CLI / 测试）都能直接引。
 */
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');

/** 素材清单 */
export const DEFAULT_MANIFEST = path.join(REPO_ROOT, 'corpus', 'assets.json');

/** 存档样本 */
export const FIXTURES_DIR = path.join(REPO_ROOT, 'corpus', 'fixtures');
export const DEFAULT_SAMPLES = path.join(FIXTURES_DIR, 'samples.json');

/** 反汇编语料 */
export const DEFAULT_STAGING = path.join(REPO_ROOT, '.staging');
export const DEFAULT_OUT_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
export const DEFAULT_ZIP = path.join(REPO_ROOT, 'corpus', 'disasm', 'disasm-20260930.zip');

/** 旧仓盘点 */
export const DEFAULT_OLD_REPO = 'E:\\Games\\Eushully\\天結';
export const DEFAULT_INVENTORY_OUT = path.join(REPO_ROOT, 'docs', '00-origin', 'old-repo-inventory.md');
