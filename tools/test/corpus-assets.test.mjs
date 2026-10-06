/** @env pure @kind gate @why LFS 规则/忽略规则/说明书约定被破坏 */
/**
 * tools/test/corpus-assets.test.mjs — `corpus/assets/`（M1b 的 UI 图片 + 字体）的**基建契约**
 *
 * 只测三件"会坏且坏得有意义"的事：
 *   ① **7z 解压产物不入库**：入库的是 `corpus/assets/fonts/*.7z`，解压目录必须被 .gitignore 命中
 *      —— 这条规则在两个地方（.gitignore 与测试）都出现是**刻意的**：只留一处时，规则被删掉不会有任何反应。
 *   ② **入库载荷确实走 LFS**：按扩展名规则（不锚具体文件名，避免"清单第二份"）。
 *   ③ **新 JSON 有同名说明书**，且说明书指向"怎么查 / 怎么改"（`json-docs.test.mjs` 把这条泛化了；
 *      这里多一条**指名**的用例，让它在 diff 里看得见）。
 *
 * ❌ 不测：图片/字体的具体取值、版本号、sha256 —— 那些是状态，交给 `pnpm tools corpus validate` 当哨兵。
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { gitAttrFilter, gitCheckIgnore, gitLsFiles } from '../lib/exec.mjs';
import { REPO_ROOT } from '../lib/paths.mjs';

const FONTS_DIR = 'corpus/assets/fonts';
const UI_DIR = 'corpus/assets/ui-images';
const ARCHIVES = ['SarasaGothicSC-TTF-1.0.40.7z', 'SarasaGothicJ-TTF-1.0.40.7z'];
const EXTRACTED = ['SarasaGothicSC', 'SarasaGothicJ'];

test('① 7z 的**解压产物**不得入库：.gitignore 逐目录命中，且 7z 本身不被忽略', () => {
  const ignoreText = fs.readFileSync(path.join(REPO_ROOT, '.gitignore'), 'utf8');
  for (const d of EXTRACTED) {
    assert.ok(
      ignoreText.includes(`/${FONTS_DIR}/${d}/`),
      `.gitignore 必须显式忽略解压目录 /${FONTS_DIR}/${d}/（入库的只有 7z）`,
    );
  }
  for (const a of ARCHIVES) {
    const r = gitCheckIgnore(REPO_ROOT, `${FONTS_DIR}/${a}`);
    assert.equal(r.error, undefined, `git check-ignore 跑不起来：${r.error}`);
    assert.equal(r.ignored, false, `入库的 7z 不得被忽略：${FONTS_DIR}/${a}`);
  }
});

test('② 入库载荷按扩展名走 LFS（.7z / .ttf / .png），且本测试不写仓（只读磁盘）', () => {
  // ★ 不捕获子进程管道（受限沙箱里要开命名管道 ⇒ EPERM）：用 lib/exec 的 gitLsFiles（文件描述符重定向）
  const tracked = () => gitLsFiles(REPO_ROOT, FONTS_DIR).files.concat(gitLsFiles(REPO_ROOT, UI_DIR).files);
  const before = tracked();
  const check = (rel) => gitAttrFilter(REPO_ROOT, rel).value;

  // 只查**盘上真有**的载荷：文件缺席时该条不成立（存在性由 corpus validate #3 管）
  for (const a of ARCHIVES) {
    const rel = `${FONTS_DIR}/${a}`;
    if (fs.existsSync(path.join(REPO_ROOT, rel))) assert.equal(check(rel), 'lfs', `${rel} 必须走 LFS`);
  }
  const samples = { ttf: 'Amayui-CN_cnjp.ttf', png: null };
  if (fs.existsSync(path.join(REPO_ROOT, FONTS_DIR, samples.ttf))) {
    assert.equal(check(`${FONTS_DIR}/${samples.ttf}`), 'lfs', '派生 ttf 必须走 LFS');
  }
  const pngs = fs.existsSync(path.join(REPO_ROOT, UI_DIR))
    ? fs.readdirSync(path.join(REPO_ROOT, UI_DIR)).filter((f) => f.endsWith('.png'))
    : [];
  assert.ok(pngs.length > 0, `${UI_DIR}/ 里必须有 UI 图（否则这条用例失去意义）`);
  for (const f of pngs) assert.equal(check(`${UI_DIR}/${f}`), 'lfs', `${UI_DIR}/${f} 必须走 LFS`);

  const after = tracked();
  assert.deepEqual(after, before, '本用例必须只读：不得改变 git index');
});

test('③ 新增的 JSON 都有同名说明书，且都指向「怎么查 / 怎么改」', () => {
  for (const rel of [`${UI_DIR}/versions.json`, 'data/translations/subs-cn-jp.json']) {
    const abs = path.join(REPO_ROOT, rel);
    assert.ok(fs.existsSync(abs), `缺文件 ${rel}`);
    const docAbs = abs.replace(/\.json$/, '.md');
    assert.ok(fs.existsSync(docAbs), `${rel} 旁边缺同名说明书 ${path.basename(docAbs)}`);
    const doc = fs.readFileSync(docAbs, 'utf8');
    assert.ok(doc.includes(path.basename(abs)), `${path.basename(docAbs)} 里没提到 ${path.basename(abs)}`);
    assert.ok(doc.includes('怎么查') && doc.includes('怎么改'), `${path.basename(docAbs)} 缺「怎么查 / 怎么改」`);
  }
});
