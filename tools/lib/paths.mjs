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

/**
 * **本机私有的清单覆盖**（`corpus/assets.local.json`；`.gitignore` 的 `*.local.json` 命中 ⇒ **不入库**）。
 *
 * 为什么需要它：清单里的 `roots`（`oldRepo` / `gameInstall` / `gameSaves`）是**绝对路径**，
 * 而它们**平台相关** —— 同一份清单要同时服务 Windows 与 macOS。把平台路径写进入库的清单 ⇒ 换台机器就红；
 * 每台机器各改一次清单 ⇒ 两台机器互相打架。⇒ 入库的清单只留"已知的那一份"，本机用这份覆盖**逐键覆盖 `roots`**。
 *
 * ★ 它**只覆盖 `roots`**：条目（来源与去向）是仓库事实，不许被本机私有文件改写。
 */
export const DEFAULT_LOCAL_MANIFEST = path.join(REPO_ROOT, 'corpus', 'assets.local.json');

/** 存档样本 */
export const FIXTURES_DIR = path.join(REPO_ROOT, 'corpus', 'fixtures');
export const DEFAULT_SAMPLES = path.join(FIXTURES_DIR, 'samples.json');

/** 反汇编语料 */
export const DEFAULT_STAGING = path.join(REPO_ROOT, '.staging');
export const DEFAULT_OUT_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
export const DEFAULT_ZIP = path.join(REPO_ROOT, 'corpus', 'disasm', 'disasm-20260930.zip');

/** 需求台账（一个节点一个文件；文件名 = ULID = 身份） */
export const DEFAULT_REQUIREMENTS_DIR = path.join(REPO_ROOT, 'data', 'requirements');

/**
 * 旧仓盘点。
 * ★ **旧仓目录不在这里**：它是平台相关路径，真源 = 清单 `roots.oldRepo`
 *   （+ 本机私有覆盖 `corpus/assets.local.json`）⇒ 用 `translate-ref.mjs` 的 `oldRepoRoot()`，
 *   或 `manifest.mjs` 的 `loadManifest()`。本模块**不许**再长出平台路径常量
 *   （`manifest.mjs` import 本模块 ⇒ 在这里 import 它会成环）。
 */
export const DEFAULT_INVENTORY_OUT = path.join(REPO_ROOT, 'docs', '00-origin', 'old-repo-inventory.md');
