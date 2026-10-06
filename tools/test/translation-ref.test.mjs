/** @env external @kind gate @why 翻译参考快照不再与旧仓逐字节相同 */
/**
 * tools/test/translation-ref.test.mjs —— **翻译参考快照的保真守卫**
 *
 * 守的是什么（一句话）：`docs/01-translation/ref/` 是旧仓 `docs/translate/**` 与
 * `docs/translation/README.md` 在某个提交上的**逐字节快照**，按**还能不能用**二分存放 ——
 * `ref/assets/**` 是散文件（落笔要查的语言资产，要能 `rg`），`ref/archive.zip` 是打包的历史工作单。
 * 它是**只读**的：不许就地改内容、不许加文件、不许把 zip 摊回散件。
 *
 * ★ 为什么这条守卫红得有意义：
 *   ① 快照一旦被就地修改，"旧文档当时怎么说"这件事就**永久不可考**了，而这正是它唯一的价值；
 *   ② 落点是**规则**不是清单（`mapPath`），所以"漏搬了哪一份 / 某一篇被放错边 / zip 里多了什么"
 *      都是机械可查的 —— 不靠人眼比对 303 个文件。
 * ★ 与 `pnpm tools disasm verify` 同构：都是"把发布物接回它的来源、要求逐字节相同"。
 *
 * 旧仓不在本机时**如实 skip**（旧仓是仓库外的只读来源）；zip 是 LFS 件，未 smudge 时也如实 skip。
 * 重建入口：`pnpm tools old-repo translate-ref --write`。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';

import {
  ARCHIVE_ZIP,
  ARCHIVE_ZIP_REL,
  blobSha1,
  mapRef,
  oldRepoRoot,
  plan,
  readArchiveZip,
  refAbs,
  snapshotFiles,
} from '../lib/translate-ref.mjs';

/** LFS 未 smudge 时盘上是这个文本指针 —— 那不是 zip，如实 skip 而不是误报红。 */
const isLfsPointer = (buf) => buf.subarray(0, 40).toString('utf8').startsWith('version https://git-lfs.github.com/spec');

const repo = oldRepoRoot();

test('快照区只含「映射规则说得出的」东西（不许就地加文件、也不许把 zip 摊回散件）', () => {
  const files = snapshotFiles();
  assert.ok(files.length > 0, '快照区应有内容');
  assert.ok(files.includes('archive.zip') || files.includes('README.md'), '至少要有一份说明或存档');
  for (const rel of files) {
    const ok = rel === 'README.md' || rel === 'archive.zip' || /^assets\/(keywords\/keywords-.+|glossary-draft|SG与SC分节对应)\.md$/.test(rel);
    assert.ok(ok, `${rel} 不在允许的形态里 —— assets 只放语言资产散件，历史工作单一律进 archive.zip`);
  }
});

test('★ assets/ 与旧仓来源**逐字节**相同（旧仓不在本机时如实 skip）', (t) => {
  if (!repo || !fs.existsSync(repo)) {
    t.skip(`旧仓不在 ${repo ?? '(未登记)'}`);
    return;
  }
  const assets = plan(repo).filter((i) => !i.packed);
  assert.ok(assets.length > 0, '旧仓该提交下应能列出资产侧文件');

  const missing = [];
  const differing = [];
  for (const it of assets) {
    const abs = refAbs(it.refRel);
    if (!fs.existsSync(abs)) {
      missing.push(it.refRel);
      continue;
    }
    if (blobSha1(fs.readFileSync(abs)) !== it.sha) differing.push(it.refRel);
  }
  assert.deepEqual(missing, [], '漏搬的资产侧文件');
  assert.deepEqual(differing, [], '资产侧被就地改过（与旧仓来源不再逐字节相同）');
});

test('★ archive.zip 与旧仓来源**逐字节**相同，且条目集合精确相等', (t) => {
  if (!repo || !fs.existsSync(repo)) {
    t.skip(`旧仓不在 ${repo ?? '(未登记)'}`);
    return;
  }
  if (!fs.existsSync(ARCHIVE_ZIP)) {
    t.skip(`没有 ${ARCHIVE_ZIP_REL}（先跑 pnpm tools old-repo translate-ref --write）`);
    return;
  }
  if (isLfsPointer(fs.readFileSync(ARCHIVE_ZIP).subarray(0, 64))) {
    t.skip(`${ARCHIVE_ZIP_REL} 是 LFS 指针（未 smudge）；先 git lfs pull`);
    return;
  }

  const packed = plan(repo).filter((i) => i.packed);
  assert.ok(packed.length > 0, '旧仓该提交下应能列出存档侧文件');
  const zip = readArchiveZip();
  const want = new Map(packed.map((p) => [p.refRel, p.sha]));

  const missing = packed.filter((p) => !zip.has(p.refRel)).map((p) => p.refRel);
  const extra = [...zip.keys()].filter((n) => !want.has(n));
  const differing = [...zip.entries()].filter(([n, d]) => want.has(n) && blobSha1(d) !== want.get(n)).map(([n]) => n);

  assert.deepEqual(missing, [], 'zip 里缺条目');
  assert.deepEqual(extra, [], 'zip 里多出来源清单之外的条目');
  assert.deepEqual(differing, [], 'zip 条目与旧仓来源不逐字节相同');
});

test('映射是规则：资产侧只认三类名字，其余一律进存档', () => {
  assert.equal(mapRef('docs/translate/keywords-角色语气.md'), 'assets/keywords/keywords-角色语气.md');
  assert.equal(mapRef('docs/translate/glossary-draft.md'), 'assets/glossary-draft.md');
  assert.equal(mapRef('docs/translate/SG与SC分节对应.md'), 'assets/SG与SC分节对应.md');
  assert.equal(mapRef('docs/translate/prob-SC0000.md'), 'archive/prob/prob-SC0000.md');
  assert.equal(mapRef('docs/translate/问题梳理与整理流程.md'), 'archive/问题梳理与整理流程.md');
  assert.equal(mapRef('docs/translation/README.md'), 'archive/legacy-translation-README.md');
  assert.equal(mapRef('docs/other/x.md'), null);
});
