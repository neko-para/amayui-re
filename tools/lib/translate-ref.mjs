/**
 * tools/lib/translate-ref.mjs —— **翻译参考快照**的领域模型
 *
 * 把旧仓 `docs/translate/**` + `docs/translation/README.md`（在某个提交上的那一版）
 * 映射到 `docs/01-translation/ref/`，并**按还能不能用**二分存放：
 *
 * ```
 * ref/assets/**            落笔要查的**语言资产**（术语 / 语气 / 联动规范）
 *                          ⇒ **散文件**：技能要能直接 rg（`keywords-角色语气.md` 就靠这个用）
 * ref/archive.zip          **历史工作单**（逐脚本待办 / 旧管线说明 / 计划）
 *                          ⇒ **打包**：不预期被阅读，只要内容还在（照 `corpus/disasm` 的 zip 路子）
 * ```
 *
 * ★ 为什么 archive 用 zip 而不是散文件：
 *   ① 那 253 个文件没有阅读价值（写的就是"当时的状态"），散在树里只是噪声；
 *   ② zip 里的字节**不过 git 的 eol/text 规范化** ⇒ 作为"历史原件"比散文件更忠实；
 *   ③ `lib/zip.mjs` 的写入是**确定性**的（固定时间戳、无 extra 字段）⇒ 同输入同字节，可复核。
 *
 * ★ 映射是**规则**不是清单：`keywords-*` / `glossary-draft.md` / `SG与SC分节对应.md` ⇒ assets，其余 ⇒ archive。
 *   所以"漏搬了哪一份 / 某一篇被放错边 / zip 里多了什么"都是机械可查的。
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';

import { runCapture } from './exec.mjs';
import { buildZip, readZip } from './zip.mjs';
import { REPO_ROOT } from './paths.mjs';

/** 快照区（仓相对 / 绝对） */
export const REF_REL = 'docs/01-translation/ref';
export const REF_DIR = path.join(REPO_ROOT, REF_REL);
export const ARCHIVE_ZIP_REL = `${REF_REL}/archive.zip`;
export const ARCHIVE_ZIP = path.join(REPO_ROOT, ARCHIVE_ZIP_REL);

/**
 * ★ 迁移来源：**唯一一份**（README 只指向本文件，不复述这个 id —— 两处写必然漂）。
 * `6cf11a79`（"chore: remove obsolete files"）是**删掉整个 `docs/`** 的那次提交；
 * 取它的**前一版**（`^`）就是"被误删之前"的那棵树。
 */
export const SOURCE_COMMIT = '6cf11a79^';
export const SOURCE_PATHS = ['docs/translate', 'docs/translation/README.md'];

/** 旧仓路径 → **ref 相对路径**。`null` = 有意不搬。 */
export function mapRef(oldRel) {
  const p = oldRel.replace(/\\/g, '/');
  if (p === 'docs/translation/README.md') return 'archive/legacy-translation-README.md';
  const m = /^docs\/translate\/(.+)$/.exec(p);
  if (!m) return null;
  const name = m[1];
  if (name.includes('/')) return null;
  // —— 资产侧：只有这三类 ——
  if (name.startsWith('keywords-')) return `assets/keywords/${name}`;
  if (name === 'glossary-draft.md') return `assets/${name}`;
  if (name === 'SG与SC分节对应.md') return `assets/${name}`;
  // —— 其余一律历史存档（进 zip）——
  if (name.startsWith('prob-')) return `archive/prob/${name}`;
  return `archive/${name}`;
}

/** ref 相对路径 → 绝对路径 */
export const refAbs = (refRel) => path.join(REF_DIR, refRel);

/** 进 zip 的一律以 `archive/` 开头 */
export const isPacked = (refRel) => refRel.startsWith('archive/');

/** 旧仓根（`corpus/assets.json` 的 `roots.oldRepo`；**只读来源**） */
export function oldRepoRoot() {
  const m = JSON.parse(fs.readFileSync(path.join(REPO_ROOT, 'corpus', 'assets.json'), 'utf8'));
  return m.roots?.oldRepo ?? null;
}

/**
 * 旧仓在该提交上的**文件清单 + blob sha1**。拿不到事实就抛，**不"读不到就当空"**。
 * 一次 `ls-tree` 拿全部（比逐文件 `git show` 快两个数量级）。
 */
export function sourceBlobs(repo) {
  const r = runCapture(repo, 'git', ['ls-tree', '-r', '-z', SOURCE_COMMIT, '--', ...SOURCE_PATHS]);
  if (r.error) throw new Error(`旧仓 git 跑不起来：${r.error}`);
  if (r.code !== 0) throw new Error(`git ls-tree ${SOURCE_COMMIT} 退出 ${r.code} ⇒ 事实拿不到`);
  const map = new Map();
  for (const rec of r.out.split('\0')) {
    if (!rec) continue;
    const m = /^\d+ \w+ ([0-9a-f]{40})\t(.*)$/.exec(rec);
    if (!m) throw new Error(`ls-tree 记录解析不了：${JSON.stringify(rec)}`);
    map.set(m[2], m[1]);
  }
  return map;
}

/** git blob 哈希：与 `git hash-object` 同形（**不过任何 filter**，因为比的是"取出来的字节"）。 */
export function blobSha1(buf) {
  return createHash('sha1').update(`blob ${buf.length}\0`).update(buf).digest('hex');
}

/** 迁移计划：`[{oldRel, refRel, sha, packed}]`，按 refRel 排序（zip 才可能同输入同字节）。 */
export function plan(repo) {
  const out = [];
  for (const [oldRel, sha] of sourceBlobs(repo)) {
    const refRel = mapRef(oldRel);
    if (!refRel) continue;
    out.push({ oldRel, refRel, sha, packed: isPacked(refRel) });
  }
  out.sort((a, b) => a.refRel.localeCompare(b.refRel));
  return out;
}

/** 取一个 blob 的**原始字节**（`git show` 的 stdout 走文件描述符重定向，不开管道）。 */
export function blobContent(repo, oldRel) {
  const tmp = path.join(os.tmpdir(), `ref-${process.pid}-${Math.random().toString(36).slice(2)}.bin`);
  const fd = fs.openSync(tmp, 'w');
  let r;
  try {
    r = spawnSync('git', ['show', `${SOURCE_COMMIT}:${oldRel}`], { cwd: repo, stdio: ['ignore', fd, 'inherit'] });
  } finally {
    fs.closeSync(fd);
  }
  if (r.error) throw new Error(`旧仓 git 跑不起来：${r.error.message}`);
  if (r.status !== 0) throw new Error(`git show ${SOURCE_COMMIT}:${oldRel} 退出 ${r.status}`);
  const buf = fs.readFileSync(tmp);
  fs.unlinkSync(tmp);
  return buf;
}

/** 读出 `ref/archive.zip`（不在 ⇒ 抛，调用方自己判） */
export function readArchiveZip() {
  if (!fs.existsSync(ARCHIVE_ZIP)) throw new Error(`没有 ${ARCHIVE_ZIP_REL}（先跑 pnpm tools old-repo translate-ref --write）`);
  return readZip(fs.readFileSync(ARCHIVE_ZIP));
}

/** 把 archive 侧的全部条目打成一个**确定性** zip（条目名 = ref 相对路径）。 */
export function buildArchiveZip(repo, items = plan(repo).filter((p) => p.packed)) {
  const entries = items.map((it) => ({ name: it.refRel, data: blobContent(repo, it.oldRel) }));
  return buildZip(entries);
}

/** 快照区里**实际**存在的文件（ref 相对路径，排序）。 */
export function snapshotFiles() {
  const out = [];
  const walk = (dir) => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const abs = path.join(dir, e.name);
      if (e.isDirectory()) walk(abs);
      else out.push(path.relative(REF_DIR, abs).replace(/\\/g, '/'));
    }
  };
  walk(REF_DIR);
  return out.sort();
}
