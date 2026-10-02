/**
 * packages/age-format/test/samples.mjs —— 测试用的**样本解析器**（不是测试本身）
 *
 * 样本是**原始游戏文件**，按用户口径**不入库**（见 `corpus/assets/samples.md`）：
 * 只有 `corpus/assets.json` 里的三条 `assets/samples-*` 条目登记了来源路径与 sha256。
 * 所以测试要这样拿样本：
 *   1. 读清单 roots（`gameInstall` 是每台机器不同的绝对路径）；
 *   2. 把条目里 path 的 basename 解析成绝对路径；
 *   3. **现算 sha256 与清单比对** —— 不只是"文件在不在"，而是"还是那一份吗"；
 *   4. 任一环不成立 ⇒ 返回 `null`，调用方**跳过**（fresh clone / 没装游戏的机器上不该红）。
 *
 * ★ 与 `tools/lib/manifest.mjs` 的关系：那份是**领域模型 + 守卫**（住在 tools/ 层）。
 *   这里刻意只**读 JSON**、不 import 它 —— 包与工具层之间不建立反向依赖
 *   （口径见 `tools/README.md` §0 的分层约定）。
 */
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
/**
 * 仓库根：`HERE` = `<root>/packages/age-format/test` ⇒ 上溯三级
 * （test → age-format → packages → 仓库根）。**这个数字踩过两次坑**：`path.dirname` 已经把最后一级算进去了。
 */
export const REPO_ROOT = path.resolve(HERE, '..', '..', '..');
export const MANIFEST = path.join(REPO_ROOT, 'corpus', 'assets.json');

export function loadManifest() {
  return JSON.parse(fs.readFileSync(MANIFEST, 'utf8'));
}

export function sha256(buf) {
  return createHash('sha256').update(buf).digest('hex');
}

/**
 * 取一个样本条目，并校验它每一项来源都在场且 sha256 与清单一致。
 * @param {string} id 例如 `assets/samples-agf`
 * @returns {null | {id: string, entries: Array<{rel: string, abs: string, buf: Buffer, sha256: string}>}}
 *          `null` = 样本不可用（缺席 / 路径根没配 / 校验和不符）—— 调用方应跳过
 */
export function loadSample(id) {
  const manifest = loadManifest();
  const entry = manifest.entries.find((e) => e.id === id);
  if (!entry) throw new Error(`清单里没有条目 ${id}（测试与清单脱节了，这是真错误，不该跳过）`);
  if (entry.storage !== 'external-only') {
    throw new Error(`${id} 的 storage 应为 external-only（样本不入库），实际 ${entry.storage}`);
  }
  const out = [];
  for (const o of entry.origin ?? []) {
    const base = manifest.roots?.[o.root];
    if (typeof base !== 'string') throw new Error(`${id} 的 origin.root "${o.root}" 不在 roots 里`);
    if (!fs.existsSync(base)) return null; // 这台机器上根本没有游戏安装目录
    const abs = path.join(base, o.path);
    if (!fs.existsSync(abs)) return null;
    const buf = fs.readFileSync(abs);
    if (typeof o.sha256 === 'string' && sha256(buf) !== o.sha256) {
      throw new Error(`${id}: ${o.path} 的 sha256 与清单不符 ⇒ 样本被换过（不是"跳过"，是事实变了）`);
    }
    out.push({ rel: o.path, abs, buf, sha256: o.sha256 });
  }
  return { id, entries: out };
}

/** 按 basename 取条目里的一个文件（ALF 的索引/数据体要分别取，见下） */
export function fileOf(sample, basename) {
  const hit = sample.entries.find((e) => path.basename(e.rel) === basename);
  if (!hit) throw new Error(`${sample.id} 里没有 ${basename}`);
  return hit;
}
