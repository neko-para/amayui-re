/** @env pure @kind product @why 发行物不满足不变量 */
/**
 * tools/test/release.test.mjs — 发行打包（`tools/lib/release.mjs` + `tools/release.mjs`）的**基建契约**测试
 *
 * 测什么（按 `AGENTS.md` §10：只测基建契约，不测业务结论）：
 *   ① **条目表只由真源算出来** —— BIN ← patch.json 的键集、AGF ← 配方集合、字体 ← 目录里的 `Amayui-CN_cnjp*.ttf`、
 *      版本节 ← `CHANGELOG.md`。**没有第四处同步清单**（旧仓 `patch.config.json` 就是这么漂掉的）；
 *   ② **形状不变量** —— 测试树里 `*.ALF` 是**硬链接**（同 inode）、其余是复制；覆盖件是真文件；
 *      排除项（`AGE-EXTEND.TTF` / 天结.exe / 本机分析产物）必须**显式报出来**，不许静默丢；
 *   ③ **守卫能不能红** —— 缺 AGF / 版本节不合规格 ⇒ 退出码 1 且**一个文件都不落**；
 *   ④ **写路径的幂等与确定性** —— 再跑一次 0 写入；同输入 ⇒ 同字节；上一轮的过时覆盖件被清掉。
 *
 * 不测什么：真实 453 支脚本的重建（那是 `pnpm tools release pack` 的活，需要游戏安装目录在场）、
 * AGF 的像素正确性（那是 `pnpm tools ui-bake verify` 的活，需要 headless Chrome）。
 * 夹具全部是**合成的**：一支能真汇编/反汇编的最小 AGE 脚本 + 哑字节的 AGF/字体/文本。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { assemble } from '../../packages/age-format/src/asm/index.mjs';
import { extractEntry, loadPatch, mapperContext, savePatch } from '../lib/patch.mjs';
import { listRecipes } from '../lib/ui-bake/recipe.mjs';
import {
  DOMAIN,
  changelogVersion,
  collectChanged,
  describe,
  installPlan,
  loadContext,
  lowIntegrityLabel,
  lowLabelAdvice,
  packPlan,
  relabelMedium,
  writeInstall,
  writePack,
} from '../lib/release.mjs';
import { readZip } from '../lib/zip.mjs';
import { sha256buf } from '../lib/fsx.mjs';
import { main as releaseMain } from '../release.mjs';

// ─────────────────────────────────────────────────────────── 夹具

const HEAD = ['==Binary Information - do not edit==', 'signature = SYS4450 ', 'local_vars = { f 1 1 6 1 2 }', '====', ''];

/** 合成一支**真能汇编**的最小 AGE 脚本（指令挑了无参数/纯数字的，与引擎无关） */
const mkScript = (rows) => assemble([...HEAD, ...rows, ''].join('\n'));

const CHANGELOG = [
  '# 汉化补丁更新记录（CHANGELOG）',
  '',
  '## v9.9（开发中）',
  '',
  '- [新翻译][LOOSE] 合成夹具：用来测「条目表由真源算出来」。',
  '',
].join('\n');

/**
 * 造一份**自足夹具**：基线树 / patch / AGF 产物 / 字体 / 随包文本 / AGERC 全在临时目录里。
 * @returns {object} 一份能直接喂 `loadContext` 的路径集合
 */
function makeFixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'release-test-'));
  const f = {
    root,
    base: path.join(root, 'gameInstall'),
    baked: path.join(root, 'baked'),
    fonts: path.join(root, 'fonts'),
    release: path.join(root, 'release'),
    agerc: path.join(root, 'agerc', 'AGERC.DLL'),
    patch: path.join(root, 'patch.json'),
    out: path.join(root, 'install'),
    manifest: path.join(root, 'install-manifest.json'),
    zipDir: path.join(root, 'patch-out'),
    zip: path.join(root, 'patch-out', 'v9.9-dev.zip'),
  };
  for (const d of [f.base, f.baked, f.fonts, f.release, path.dirname(f.agerc), f.zipDir]) fs.mkdirSync(d, { recursive: true });

  // ── 基线树：一支散装脚本 + 一个"ALF"大件 + 一个普通件 + 该排除的四类 ──
  f.baseBin = mkScript(['i1f4', 'comment "hello"', 'i258 3 1']);
  f.tgtBin = mkScript(['i1f4', 'comment "world"', 'i258 4 1', 'i258 5 1']);
  fs.writeFileSync(path.join(f.base, 'LOOSE.BIN'), f.baseBin);
  fs.writeFileSync(path.join(f.base, 'DATA1.ALF'), Buffer.alloc(4096, 7));
  fs.writeFileSync(path.join(f.base, 'note.txt'), '原版普通件\n');
  fs.writeFileSync(path.join(f.base, 'AGE-EXTEND.TTF'), 'font');
  fs.writeFileSync(path.join(f.base, '天结.exe'), 'shell');
  fs.writeFileSync(path.join(f.base, 'AGE.EXE__info.txt'), 'analysis');
  fs.writeFileSync(path.join(f.base, 'junk.dmp'), 'crash');
  fs.mkdirSync(path.join(f.base, '_extracted'));
  fs.writeFileSync(path.join(f.base, '_extracted', 'junk.bin'), 'x');

  // ── patch：用**真提取器**从"基线 → 目标"造条目（不手写 ops，免得测的是自己的手艺） ──
  const { mapper, subsSha } = mapperContext();
  const entry = extractEntry(f.baseBin, f.tgtBin, mapper);
  const res = savePatch(
    { schemaVersion: 1, subsSha, scripts: { 'LOOSE.BIN': { baseSha: entry.baseSha, resultSha: entry.resultSha, ...(entry.header ? { header: entry.header } : {}), ops: entry.ops } } },
    f.patch,
  );
  assert.equal(res.ok, true, `夹具的 patch 必须能落盘：${res.reason}`);

  // ── AGF 产物 / 字体（含一张**不该进包**的 Sarasa 基底）/ 随包文本 / AGERC ──
  //    产物按**真仓的配方集合**铺满（缺省 blocks 就是它）；`f.blocks` 是给模型级用例的紧凑子集
  f.allBlocks = listRecipes();
  f.blocks = ['SO001', 'SO002'];
  for (const b of f.allBlocks) fs.writeFileSync(path.join(f.baked, `${b}.AGF`), Buffer.from(`AGF:${b}`));
  fs.writeFileSync(path.join(f.fonts, 'Amayui-CN_cnjp.ttf'), 'regular');
  fs.writeFileSync(path.join(f.fonts, 'Amayui-CN_cnjp-Bold.ttf'), 'bold');
  fs.writeFileSync(path.join(f.fonts, 'SarasaGothicSC-Regular.ttf'), '上游基底，不进包');
  fs.writeFileSync(path.join(f.release, 'CHANGELOG.md'), CHANGELOG);
  fs.writeFileSync(path.join(f.release, '安装说明.md'), '# 安装说明\n\n1. 复制一份游戏\n');
  fs.writeFileSync(f.agerc, Buffer.from('AGERC'));

  return f;
}

/** 夹具 → 上下文（可覆盖任意字段：测试要"改一处看它红不红"） */
const ctxOf = (f, over = {}) =>
  loadContext({
    base: f.base,
    patch: f.patch,
    baked: f.baked,
    agerc: f.agerc,
    fonts: f.fonts,
    releaseDir: f.release,
    blocks: f.blocks,
    out: f.out,
    installManifest: f.manifest,
    zipDir: f.zipDir,
    ...over,
  });

/** 静音跑一段（CLI 会打印计划；判据是退出码与盘上结果） */
function run(fn) {
  const out = process.stdout.write.bind(process.stdout);
  const err = process.stderr.write.bind(process.stderr);
  process.stdout.write = () => true;
  process.stderr.write = () => true;
  try {
    return fn();
  } finally {
    process.stdout.write = out;
    process.stderr.write = err;
  }
}

const withFixture = (fn) => {
  const f = makeFixture();
  try {
    return fn(f);
  } finally {
    fs.rmSync(f.root, { recursive: true, force: true });
  }
};

const entriesOf = (plan) => plan.rows.map((r) => r.entry);

// ─────────────────────────────────────────────────────────── ① 条目表只由真源算出来

test('★ pack：条目表只由真源算出来（键集 / AGF 集合 / 版本节）', () => {
  withFixture((f) => {
    const ctx = ctxOf(f);
    const changed = collectChanged(ctx);
    assert.deepEqual(changed.problems, [], `变更集不许有问题：${changed.problems.join('; ')}`);
    const plan = packPlan(ctx, changed);

    assert.deepEqual(
      entriesOf(plan),
      ['BIN/LOOSE.BIN', 'AGF/SO001.AGF', 'AGF/SO002.AGF', 'AGERC.DLL', 'Amayui-CN_cnjp-Bold.ttf', 'Amayui-CN_cnjp.ttf', 'CHANGELOG.md', '安装说明.md'],
      '包里有且只有：BIN 键集 + AGF 配方集 + AGERC + 发行字体 + 两份文本（顺序固定）',
    );
    assert.ok(!entriesOf(plan).some((n) => n.includes('Sarasa')), '上游 Sarasa 基底**不随包发**');
    assert.ok(!entriesOf(plan).some((n) => n.includes('note.txt')), '基础树的件不进发行包');

    // 字节也对：BIN 是「基线 + patch」重建出来的那一份
    const bin = plan.rows.find((r) => r.entry === 'BIN/LOOSE.BIN');
    assert.ok(bin.buf.equals(f.tgtBin), '包里的 BIN 必须是重建结果（= 提取时的产物）');
    assert.equal(bin.source.startsWith('patch.json'), true, '每一件都要能指回真源');

    assert.equal(plan.version.version, 'v9.9');
    assert.equal(plan.version.dev, true);
    assert.equal(plan.zipName, 'v9.9-dev.zip');
    assert.deepEqual(plan.problems, []);
  });
});

test('AGF 集合缺省 = **有配方的块**（不是某份手写名单）', () => {
  withFixture((f) => {
    const ctx = ctxOf(f, { blocks: null });
    const changed = collectChanged(ctx);
    assert.deepEqual(changed.problems, [], changed.problems.join('; '));
    assert.deepEqual(
      changed.agfs.map((a) => a.block),
      f.allBlocks,
      'AGF 进包集合 = 有配方的块（tools/ui-bake/recipes/ 就是"我们改过哪些图"的真源）',
    );

    // 缺一张 ⇒ 必须报"先跑哪条命令"，而不是自己凑一张图
    fs.rmSync(path.join(f.baked, 'SO001.AGF'));
    const bad = collectChanged(ctx);
    assert.ok(bad.problems.some((p) => /SO001\.AGF/.test(p) && /ui-bake build/.test(p)), bad.problems.join('; '));
    assert.ok(!bad.agfs.some((a) => a.block === 'SO001'));
  });
});

test('changelogVersion：开发中 / 已发布 / 不成形三种形态', () => {
  assert.deepEqual(
    (({ version, dev, zipName }) => ({ version, dev, zipName }))(changelogVersion('## v1.14（开发中）\n')),
    { version: 'v1.14', dev: true, zipName: 'v1.14-dev.zip' },
  );
  // 与旧仓 zip 命名一脉相承：v1.13-260901.zip
  assert.equal(changelogVersion('# x\n\n## v1.13（2026-09-01）\n').zipName, 'v1.13-260901.zip');
  assert.equal(changelogVersion('## v1.14（待定）\n').zipName, null, '标注既不是日期也不是"开发中" ⇒ 算不出 zip 名');
  assert.equal(changelogVersion('# 没有版本节\n'), null);
});

test('pack：没有当前版本节 ⇒ 红（不许出个没版本节的包）', () => {
  withFixture((f) => {
    fs.writeFileSync(path.join(f.release, 'CHANGELOG.md'), '# 只有标题，没有版本节\n');
    const ctx = ctxOf(f);
    const plan = packPlan(ctx, collectChanged(ctx));
    assert.ok(plan.problems.some((p) => /找不到版本节/.test(p)), `必须因为版本节报红：${plan.problems.join('; ')}`);
  });
});

// ─────────────────────────────────────────────────────────── ② 形状不变量

test('★ install：ALF 硬链接、其余复制、覆盖件是真文件', () => {
  withFixture((f) => {
    const ctx = ctxOf(f);
    const plan = installPlan(ctx, collectChanged(ctx));
    assert.deepEqual(plan.problems, []);

    // 基础树的三种去向
    const byPath = new Map(plan.base.map((r) => [r.path, r]));
    assert.equal(byPath.get('DATA1.ALF').action, 'hardlink');
    assert.equal(byPath.get('note.txt').action, 'copy');

    // 排除项与跳过项都要**显式**报出来
    const names = plan.excluded.map((e) => e.name);
    for (const n of ['AGE-EXTEND.TTF', '天结.exe', 'AGE.EXE__info.txt', 'junk.dmp']) {
      assert.ok(names.includes(n), `${n} 必须被显式排除（而不是静默丢掉）`);
    }
    assert.ok(plan.excluded.every((e) => e.why && e.why.length > 5), '每条排除都要有理由');
    assert.deepEqual(plan.dirs, ['_extracted'], '子目录不复制，但要报出来');

    // 覆盖件 = 变更集，且 BIN 那一支会**顶掉**基础树里的同名复制件
    const overlay = new Map(plan.overlays.map((o) => [o.path, o]));
    assert.ok(overlay.has('LOOSE.BIN') && overlay.get('LOOSE.BIN').replaces === 'LOOSE.BIN', 'LOOSE.BIN 在基础树里也有 ⇒ 覆盖件顶掉它');
    assert.ok(overlay.has('SO001.AGF'));

    const res = writeInstall(ctx, plan);
    assert.deepEqual(res.problems, []);
    assert.equal(res.ok, true);

    const srcAlf = fs.statSync(path.join(f.base, 'DATA1.ALF'));
    const dstAlf = fs.statSync(path.join(f.out, 'DATA1.ALF'));
    assert.equal(dstAlf.ino, srcAlf.ino, 'ALF 必须是**同一个 inode**（硬链接，而不是复制）');
    assert.ok(dstAlf.nlink >= 2, `硬链接的链数必须 ≥ 2（实际 ${dstAlf.nlink}）`);

    const srcNote = fs.statSync(path.join(f.base, 'note.txt'));
    const dstNote = fs.statSync(path.join(f.out, 'note.txt'));
    assert.notEqual(dstNote.ino, srcNote.ino, '非 ALF 的件必须是复制（不同 inode）');
    assert.equal(fs.readFileSync(path.join(f.out, 'note.txt'), 'utf8'), '原版普通件\n');
    assert.ok(Math.abs(dstNote.mtimeMs - srcNote.mtimeMs) < 2, '复制要保住来源 mtime（也让"跳过"判据成立；±2 ms 是文件系统取整）');

    // 覆盖件：内容 = 重建结果，且是**真文件**（不能是链到基线的硬链接）
    const outBin = fs.readFileSync(path.join(f.out, 'LOOSE.BIN'));
    assert.ok(outBin.equals(f.tgtBin), '覆盖件必须是重建后的 BIN');
    assert.notEqual(fs.statSync(path.join(f.out, 'LOOSE.BIN')).ino, fs.statSync(path.join(f.base, 'LOOSE.BIN')).ino);
    assert.ok(fs.readFileSync(path.join(f.base, 'LOOSE.BIN')).equals(f.baseBin), '基线树必须一个字节都没被动过');

    // 被排除的件不许出现在树里
    for (const n of ['AGE-EXTEND.TTF', '天结.exe', 'AGE.EXE__info.txt', 'junk.dmp', '_extracted']) {
      assert.equal(fs.existsSync(path.join(f.out, n)), false, `${n} 不该出现在测试树里`);
    }
  });
});

test('★ install：再跑一次 0 写入（幂等）', () => {
  withFixture((f) => {
    const ctx = ctxOf(f);
    const plan = installPlan(ctx, collectChanged(ctx));
    assert.equal(writeInstall(ctx, plan).ok, true);
    const second = writeInstall(ctx, plan);
    assert.equal(second.ok, true);
    assert.equal(second.stats.write, 0, '覆盖件不该重写');
    assert.equal(second.stats.copy, 0, '基础件不该重拷');
    assert.equal(second.stats.hardlink, 0, 'ALF 不该重链');
    assert.equal(second.stats.prune, 0);
  });
});

test('★ install：上一轮的过时覆盖件被清掉（含"变回基础件"的那种）', () => {
  withFixture((f) => {
    const ctx = ctxOf(f);
    assert.equal(writeInstall(ctx, installPlan(ctx, collectChanged(ctx))).ok, true);
    assert.ok(fs.readFileSync(path.join(f.out, 'LOOSE.BIN')).equals(f.tgtBin));
    assert.ok(fs.existsSync(path.join(f.out, 'SO002.AGF')));

    // 换一份**空 patch** + 只留一个配方 ⇒ 上一轮的 LOOSE.BIN 覆盖件过时、SO002.AGF 覆盖件过时
    const { subsSha } = mapperContext();
    savePatch({ schemaVersion: 1, subsSha, scripts: {} }, f.patch);
    const ctx2 = ctxOf(f, { blocks: ['SO001'] });
    const plan2 = installPlan(ctx2, collectChanged(ctx2));
    assert.deepEqual(plan2.problems, []);
    const res2 = writeInstall(ctx2, plan2);
    assert.equal(res2.ok, true);
    assert.equal(res2.stats.prune, 1, 'SO002.AGF 是纯覆盖件 ⇒ 清掉');

    assert.equal(fs.existsSync(path.join(f.out, 'SO002.AGF')), false, '过时的纯覆盖件必须清掉');
    assert.ok(fs.readFileSync(path.join(f.out, 'LOOSE.BIN')).equals(f.baseBin), '过时覆盖件的名字若回到基础树里，必须重铺成基础件（不留旧译文）');
  });
});

test('install：落点已存在且**不是本工具建的** ⇒ 拒绝（除非 --force），且不动里面的东西', () => {
  withFixture((f) => {
    fs.mkdirSync(f.out, { recursive: true });
    fs.writeFileSync(path.join(f.out, '别人的文件.txt'), '别碰我');
    const ctx = ctxOf(f);
    const plan = installPlan(ctx, collectChanged(ctx));
    const res = writeInstall(ctx, plan);
    assert.equal(res.ok, false);
    assert.ok(res.problems.some((p) => /--force/.test(p)), `必须告诉用户怎么办：${res.problems.join('; ')}`);
    assert.equal(fs.readFileSync(path.join(f.out, '别人的文件.txt'), 'utf8'), '别碰我');
    assert.equal(fs.existsSync(path.join(f.out, 'DATA1.ALF')), false, '拒绝了就一个文件都不许落');
  });
});

test('install：`--alf copy` 的退路真的走通（不同卷 / 拿不到建链权限的机器）', () => {
  withFixture((f) => {
    const ctx = ctxOf(f, { alfMode: 'copy' });
    const plan = installPlan(ctx, collectChanged(ctx));
    assert.equal(plan.base.find((r) => r.path === 'DATA1.ALF').action, 'copy', 'ALF 也要落成 copy');
    assert.deepEqual(plan.problems, []);
    const res = writeInstall(ctx, plan);
    assert.equal(res.ok, true);
    const srcAlf = fs.statSync(path.join(f.base, 'DATA1.ALF'));
    const dstAlf = fs.statSync(path.join(f.out, 'DATA1.ALF'));
    assert.notEqual(dstAlf.ino, srcAlf.ino, 'copy 模式下 ALF 是**另一份**（不再是同一个 inode）');
    assert.ok(fs.readFileSync(path.join(f.out, 'DATA1.ALF')).equals(fs.readFileSync(path.join(f.base, 'DATA1.ALF'))));
  });
});

test('install：硬链接拿不到时**不留半棵树**（新建的那棵整棵撤掉）', () => {
  withFixture((f) => {
    const ctx = ctxOf(f);
    const plan = installPlan(ctx, collectChanged(ctx));
    // 让预探失败：源文件在落盘前消失
    fs.rmSync(path.join(f.base, 'DATA1.ALF'));
    const res = writeInstall(ctx, plan);
    assert.equal(res.ok, false);
    assert.ok(res.problems.some((p) => /硬链接不可用|ENOENT/.test(p)), res.problems.join('; '));
    assert.equal(fs.existsSync(f.out), false, '一次都没成 ⇒ 不许留下"有一半基础件、又没有清单"的假树');
  });
});

test('install：`--le-cmd` 写的启动器钉住当前目录（`cd /d "%~dp0"`），路径错了就红', () => {
  withFixture((f) => {
    const le = path.join(f.root, 'LEProc.exe');
    fs.writeFileSync(le, 'not really LE');
    const ctx = ctxOf(f, { leCmd: le, leProfile: '1bad53a5-5774-46ee-bdcd-0afe948cf006' });
    const plan = installPlan(ctx, collectChanged(ctx));
    assert.deepEqual(plan.problems, []);
    const l = plan.overlays.find((o) => o.kind === 'launcher');
    assert.ok(l, '给了 --le-cmd 就必须产出启动器');
    assert.equal(l.path, '启动游戏-LE.cmd');
    const text = l.buf.toString('latin1');
    assert.match(text, /cd \/d "%~dp0"/, '必须先把当前目录切到树自己（引擎按当前目录找件）');
    assert.match(text, /-runas 1bad53a5-/, '要带上 --le-profile');
    assert.match(text, /"%~dp0AGE\.EXE"/, '启动对象是同一棵树里的 AGE.EXE（不写死绝对路径）');
    assert.ok(!/[^\x00-\x7f]/.test(text), 'cmd 按 ANSI 解码 ⇒ 内容必须全 ASCII（中文会变乱码）');

    // 路径不对 ⇒ 红（而不是写一份起不来的启动器）
    const bad = ctxOf(f, { leCmd: path.join(f.root, '不存在', 'LEProc.exe') });
    const badPlan = installPlan(bad, collectChanged(bad));
    assert.ok(badPlan.problems.some((p) => /LEProc 不在/.test(p)), badPlan.problems.join('; '));
  });
});

test('完整性标签：能认出来、能算出改标签的命令（Low 标签会让 LE 起不了窗口）', () => {
  const lowOut = 'E:\\tmp\\x\\AGE.EXE Mandatory Label\\Low Mandatory Level:(I)(NW)\nSuccessfully processed 1 files; Failed processing 0 files\n';
  const fake = (out, code = 0) => () => ({ code, out, error: undefined });
  if (process.platform !== 'win32') return;
  assert.equal(lowIntegrityLabel('C:\\t\\AGE.EXE', fake('E:\\tmp\\x\\AGE.EXE Mandatory Label\\Medium Mandatory Level:(I)(NW)\n')).low, false);
  const low = lowIntegrityLabel('C:\\t\\AGE.EXE', fake(lowOut));
  assert.equal(low.low, true);
  assert.match(low.detail, /Low Mandatory Level/);
  // 认不出来（icacls 跑不起来）不许当成"没问题"
  assert.equal(lowIntegrityLabel('C:\\t\\AGE.EXE', () => ({ code: 127, out: '', error: 'EPERM' })).error, 'EPERM');

  const calls = [];
  const rec = (cwd, cmd, args) => { calls.push([cmd, ...args]); return { code: 0, out: 'ok' }; };
  assert.equal(relabelMedium('C:\\t', rec).ok, true);
  assert.deepEqual(calls[0], ['icacls', 'C:\\t', '/setintegritylevel', 'Medium', '/T', '/C']);
  assert.match(lowLabelAdvice('C:\\t').join('\n'), /setintegritylevel Medium/);
});

test('install：落盘途中出错也**不留半棵树**（外层薄壳兜住意外抛出）', () => {
  withFixture((f) => {
    const ctx = ctxOf(f);
    const good = installPlan(ctx, collectChanged(ctx));
    const buf = Buffer.from('x');
    // 造一个"写不下去"的覆盖件（Windows 文件名里 `?` 非法）
    const bad = { ...good, overlays: [...good.overlays, { path: 'x?.BIN', kind: 'bin', buf, sha256: sha256buf(buf), source: '测试' }] };
    const res = writeInstall(ctx, bad);
    assert.equal(res.ok, false, '写不下去必须报失败');
    assert.ok(res.problems.some((p) => /落盘过程中出错/.test(p)), res.problems.join('; '));
    assert.equal(fs.existsSync(f.out), false, '这一轮新建的树必须整棵撤掉，不许留残件');
    assert.equal(fs.existsSync(path.join(f.out, 'DATA1.ALF')), false);
  });
});

// ─────────────────────────────────────────────────────────── ③ 守卫能红（CLI 层）

const cliBase = (f) => [
  '--base', f.base, '--patch', f.patch, '--baked', f.baked, '--agerc', f.agerc,
  '--fonts', f.fonts, '--release-dir', f.release, '--write',
];
const packArgs = (f, out) => [...cliBase(f), '--pack', '--out', out];
const installArgs = (f, out) => [...cliBase(f), '--install', '--out', out];
const planArgs = (f, target, out) => [...cliBase(f).filter((a) => a !== '--write'), '--plan', target, ...(out ? ['--out', out] : [])];

test('CLI pack：绿 ⇒ 退出 0 且 zip 能反解出同样多条目；红 ⇒ 退出 1 且**一个文件都不落**', () => {
  withFixture((f) => {
    assert.equal(run(() => releaseMain(packArgs(f, f.zip))), 0, '绿路径必须退出 0');
    assert.ok(fs.existsSync(f.zip));
    const want = 1 + f.allBlocks.length + 1 + 2 + 2; // BIN + AGF 配方集 + AGERC + 字体 2 + 文本 2
    assert.equal(readZip(fs.readFileSync(f.zip)).size, want, `包里应当有 ${want} 个条目（条目名见 pnpm tools release pack 的输出）`);

    // 红：抽掉一张 AGF 产物（模拟"没跑 ui-bake build"）
    fs.rmSync(path.join(f.baked, 'SO001.AGF'));
    const bad = path.join(f.zipDir, 'bad.zip');
    assert.equal(run(() => releaseMain(packArgs(f, bad))), 1);
    assert.equal(fs.existsSync(bad), false, '红灯时不许留下残包');
  });
});

test('CLI pack：同输入 ⇒ 同字节（确定性；连带清单也是）', () => {
  withFixture((f) => {
    assert.equal(run(() => releaseMain(packArgs(f, f.zip))), 0);
    const once = fs.readFileSync(f.zip);
    const man = f.zip.replace(/\.zip$/, '.manifest.json');
    const onceMan = fs.readFileSync(man);
    assert.equal(run(() => releaseMain(packArgs(f, f.zip))), 0);
    assert.ok(once.equals(fs.readFileSync(f.zip)), 'zip 必须同输入同字节（固定时间戳 + 固定条目序）');
    assert.ok(onceMan.equals(fs.readFileSync(man)), '清单也不许带 generatedAt 这类每次都变的东西');
  });
});

test('CLI install：plan 不落盘、install 落盘（活/死两条路都给退出码）', () => {
  withFixture((f) => {
    assert.equal(run(() => releaseMain(planArgs(f, 'install', f.out))), 0);
    assert.equal(fs.existsSync(f.out), false, 'plan（dry-run）不许落盘');
    assert.equal(run(() => releaseMain(installArgs(f, f.out))), 0);
    assert.ok(fs.existsSync(path.join(f.out, 'DATA1.ALF')), 'install --write 必须真的铺出树');
    assert.equal(run(() => releaseMain(planArgs(f, 'pack', f.zip))), 0);
    assert.equal(fs.existsSync(f.zip), false, 'pack 的 plan 也不许落盘');
  });
});

// ─────────────────────────────────────────────────────────── ④ 自描述与注册

test('自描述可用：变更集 / 形状 / 不变量（含谁在守）/ 操作都在', () => {
  const d = describe();
  assert.equal(DOMAIN.id, 'release');
  assert.ok(DOMAIN.data.some((x) => x.includes('patch.json')) && DOMAIN.data.some((x) => x.includes('dist/')));
  assert.ok(d.changedSet.length >= 4, '变更集的每一处真源都要自描述出来');
  assert.ok(d.packShape.length >= 5 && d.installShape.length >= 4);
  assert.ok(d.invariants.length >= 8);
  for (const inv of d.invariants) assert.ok(inv.enforcedBy && inv.enforcedBy.length > 0, `不变量 ${inv.id} 没说"谁在守它"`);
  assert.match(d.writePath, /唯一写入口/);
  assert.ok(d.operations.some((o) => o.name === 'install' && o.mutates) && d.operations.some((o) => o.name === 'pack' && o.mutates));
});

test('patch 的结构不变量：坏的 patch ⇒ 变更集直接报红（不静默跳过）', () => {
  withFixture((f) => {
    const doc = loadPatch(f.patch);
    doc.scripts['LOOSE.BIN'].ops[0].sha8 = 'deadbeef'; // 基线行摘要对不上
    fs.writeFileSync(f.patch, JSON.stringify(doc, null, 1));
    const changed = collectChanged(ctxOf(f));
    assert.ok(changed.problems.length > 0, '坏 patch 必须报红');
  });
});

test('writePack：算不出 zip 名时不许瞎写', () => {
  withFixture((f) => {
    const ctx = ctxOf(f);
    const res = writePack(ctx, { rows: [], zipPath: null, version: null });
    assert.equal(res.ok, false);
    assert.match(res.problems.join(';'), /不知道 zip 叫什么/);
  });
});
