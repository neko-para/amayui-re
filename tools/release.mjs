#!/usr/bin/env node
/**
 * tools/release.mjs —— **CLI**：发行打包的两条命令（测试安装树 / 发给玩家的 zip）
 *
 * 经派发器：
 *   pnpm tools release plan install | plan pack      （只算不写）
 *   pnpm tools release install --write               （→ dist/install/）
 *   pnpm tools release pack    --write               （→ dist/patch/<版本>.zip）
 * 也可独立跑：`node tools/release.mjs --install --write`
 *
 * 模型在 `lib/release.mjs`（变更集从哪来、形状、不变量都写在那边）；本文件只做"参数 → 模型 → 打印 + 落盘"。
 * ★ 缺省 **dry-run**（与 corpus / fixtures / requirements / patch / ui-bake 同一口径）：`--write` 才落盘。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import {
  DEFAULT_AGF_DIR,
  DEFAULT_BAKED_DIR,
  DEFAULT_INSTALL_DIR,
  DEFAULT_INSTALL_MANIFEST,
  DEFAULT_PATCH_DIR,
  DOMAIN as DOMAIN_DECL,
  LE_LAUNCHER_NAME,
  OPERATIONS as OPERATIONS_DECL,
  changelogVersion,
  collectChanged,
  describe,
  describeText,
  installPlan,
  loadContext,
  packPlan,
  readInstallManifest,
  relToRepo,
  writeInstall,
  writePack,
} from './lib/release.mjs';

/**
 * 落点的清单：缺省 `dist/install-manifest.json`（沿用旧仓那个名字）；`--out X` ⇒ `X.manifest.json`。
 * ★ 显式 `--out` 恰好**就是缺省落点**时仍按缺省清单算 —— 否则同一棵树会因为"这次带没带 --out"
 *   而认不出自己上一轮写的清单（实测：dry-run 误报"目标已存在且不是本工具建的"）。
 */
const manifestForOut = (out) =>
  out && path.resolve(out) !== path.resolve(DEFAULT_INSTALL_DIR) ? `${path.resolve(out)}.manifest.json` : DEFAULT_INSTALL_MANIFEST;

export const DOMAIN = DOMAIN_DECL;
export const OPERATIONS = OPERATIONS_DECL;
export { describe, describeText };

const HELP = `tools/release.mjs —— 发行打包：测试安装树（旧仓 install/）与发行包（旧仓 patch/）

  node tools/release.mjs --describe                自描述：变更集从哪来 / 包里有什么 / 不变量
  node tools/release.mjs --plan install            测试安装树的执行计划（不落盘）
  node tools/release.mjs --plan pack               发行包的执行计划（不落盘）
  node tools/release.mjs --install [--write]       同步出测试安装树（ALF 硬链接；其余复制）
  node tools/release.mjs --pack    [--write]       打出 zip（含自检：缺件 / 键集 / 版本节）

公共选项
  --write              真的落盘（缺省 dry-run：只报会写什么）
  --json               机器可读（计划 / 结果）
  --base   <目录>      基线根（缺省清单 roots.gameInstall）
  --patch  <文件>      换一份 patch（诊断用；缺省 data/translations/patch.json）
  --baked  <目录>      AGF 来源**覆盖**（缺省 = 入库件 ${relToRepo(DEFAULT_AGF_DIR)}；
                       要用刚烧出来的那份就写 --baked ${relToRepo(DEFAULT_BAKED_DIR)}）
  --agerc  <文件>      AGERC.DLL 来源（缺省 corpus/assets/agerc/AGERC.DLL）
  --fonts  <目录>      字体目录（缺省 corpus/assets/fonts）
  --release-dir <目录> 随包文本目录（缺省 release/）
  --out    <路径>      install：落点目录（缺省 ${relToRepo(DEFAULT_INSTALL_DIR)}）
                       pack：zip 落点（缺省 ${relToRepo(DEFAULT_PATCH_DIR)}/<版本>.zip）
  --force              install：落点已存在且**不是本工具建的**时仍然继续（危险；缺省拒绝）
  --alf    <方式>      install：ALF 的铺法 hardlink（缺省）| copy（不同卷 / 拿不到建链权限时用）
  --le-cmd <路径>      install：写一份 Locale Emulator 启动器 → 树里的 启动游戏-LE.cmd
                       （内容是 cd /d "%~dp0" 再调 LEProc；旧仓 install/ 里就有这么一份）
  --le-profile <guid>  上一条的 LE 配置档（缺省不带 -runas，用 LE 的缺省档）
  --relabel-medium     install：落盘后把整棵树的完整性标签改成 Medium
                       （只有在**受限沙箱工作区**里建树才需要：那里的文件继承 Low 标签，
                        而"从 Low 标签的 EXE 起的进程"会让 Locale Emulator 起不了窗口）
  --exclude <名字>     install：额外排除的基础树文件（可重复；理由见 --describe）

★ 变更集**算出来**，不存同步清单：BIN ← patch.json 的键 · AGF ← **入库件**（缺省不重烧）· AGERC ← 入库可信产物。
★ 测试安装树里的 *.ALF 与游戏安装根**是同一个 inode** ⇒ 别往那棵树里写东西（本工具只写自己的产物）。
`;

function parseArgs(argv) {
  const out = {
    action: null,
    target: null,
    write: false,
    json: false,
    force: false,
    out: null,
    base: null,
    patch: null,
    baked: null,
    agerc: null,
    fonts: null,
    releaseDir: null,
    alf: null,
    leCmd: null,
    leProfile: null,
    relabel: false,
    exclude: [],
  };
  const takesValue = new Set(['out', 'base', 'patch', 'baked', 'agerc', 'fonts', 'release-dir', 'alf', 'le-cmd', 'le-profile', 'exclude']);
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (['--describe', '--plan', '--install', '--pack', '--help', '-h'].includes(a)) out.action = a.replace(/^--?/, '');
    else if (a === '--write') out.write = true;
    else if (a === '--json') out.json = true;
    else if (a === '--force') out.force = true;
    else if (a === '--relabel-medium') out.relabel = true;
    else if (a.startsWith('--')) {
      const k = a.slice(2);
      if (!takesValue.has(k)) throw new Error(`不认识的选项：${a}`);
      const v = argv[++i];
      if (v === undefined) throw new Error(`${a} 需要值`);
      if (k === 'release-dir') out.releaseDir = v;
      else if (k === 'le-cmd') out.leCmd = path.resolve(v);
      else if (k === 'le-profile') out.leProfile = v;
      else if (k === 'exclude') out.exclude.push(v);
      else out[k] = v;
    } else if (out.action === 'plan') {
      out.target = a;
    } else throw new Error(`多余的位置参数：${a}`);
  }
  if (!out.action) out.action = 'help';
  if (out.alf !== null && !['hardlink', 'copy'].includes(out.alf)) throw new Error('--alf 只能是 hardlink / copy');
  if (out.action === 'plan' && !['install', 'pack'].includes(out.target ?? '')) {
    throw new Error('用法：`pnpm tools release plan install` 或 `pnpm tools release plan pack`');
  }
  return out;
}

const mb = (n) => `${(n / 1048576).toFixed(1)} MB`;
const pad = (s, n) => String(s).padEnd(n);

/** 打印变更集摘要（三个动作共用） */
function printChanged(changed, ctx) {
  const L = [];
  L.push(`基线根     ${ctx.baseDir}`);
  L.push(`patch      ${relToRepo(ctx.patchPath)}`);
  L.push(`变更集     BIN ${changed.bins.length} 支（操作 ${changed.stats.ops}）· AGF ${changed.agfs.length} 张 · AGERC ${changed.stats.agerc} 个`);
  if (changed.agfs.length) {
    const fromBaked = path.resolve(ctx.agfDir) === path.resolve(DEFAULT_BAKED_DIR);
    L.push(`AGF 来源   ${relToRepo(ctx.agfDir)}（${fromBaked ? '★ 就地烧出来的那份（显式覆盖）' : '入库件，默认不重烧'}）`);
  }
  return L;
}

function reportProblems(title, problems) {
  process.stderr.write(`\n✖ ${title}（${problems.length} 条）—— **一个字都不写**：\n`);
  for (const p of problems.slice(0, 40)) process.stderr.write(`   - ${p}\n`);
  if (problems.length > 40) process.stderr.write(`   … 还有 ${problems.length - 40} 条\n`);
}

// ─────────────────────────────────────────────────────────── plan / install

function runInstall(args, { dryRun }) {
  const ctx = loadContext({
    base: args.base ?? undefined,
    patch: args.patch ?? undefined,
    baked: args.baked ?? undefined,
    agerc: args.agerc ?? undefined,
    out: args.out ? path.resolve(args.out) : DEFAULT_INSTALL_DIR,
    installManifest: manifestForOut(args.out),
    alfMode: args.alf ?? undefined,
    leCmd: args.leCmd ?? undefined,
    leProfile: args.leProfile ?? undefined,
    exclude: args.exclude,
  });
  const changed = collectChanged(ctx, {
    onProgress: (d, t) => { if (d % 100 === 0) process.stdout.write(`… 重建 ${d}/${t}\n`); },
  });
  const plan = installPlan(ctx, changed);
  const problems = [...changed.problems, ...plan.problems];

  if (args.json) {
    process.stdout.write(`${JSON.stringify({
      action: 'install', dryRun, ctx: { baseDir: ctx.baseDir, outDir: ctx.outDir, manifest: ctx.installManifest },
      changed: { bins: changed.bins.length, agfs: changed.agfs.map((a) => a.block), agerc: Boolean(changed.agerc) },
      base: plan.base, overlays: plan.overlays.map((o) => ({ path: o.path, kind: o.kind, bytes: o.buf.length, sha256: o.sha256, source: o.source })),
      excluded: plan.excluded, dirs: plan.dirs, problems, warnings: changed.warnings,
    }, null, 1)}\n`);
    return problems.length ? 1 : 0;
  }

  const L = printChanged(changed, ctx);
  const alfs = plan.base.filter((r) => r.action === 'hardlink');
  const others = plan.base.filter((r) => r.action === 'copy');
  L.push('');
  L.push(`基础树     ${plan.base.length} 个顶层文件：ALF 硬链接 ${alfs.length} 个（${mb(alfs.reduce((a, r) => a + r.bytes, 0))}，不占额外空间）· 复制 ${others.length} 个（${mb(others.reduce((a, r) => a + r.bytes, 0))}）`);
  L.push(`          子目录不复制 ${plan.dirs.length} 个：${plan.dirs.join(' · ') || '（无）'}`);
  L.push(`排除       ${plan.excluded.length} 个：${plan.excluded.map((e) => `${e.name}（${e.why}）`).join(' · ') || '（无）'}`);
  const kindCount = (k) => plan.overlays.filter((o) => o.kind === k).length;
  L.push(
    `覆盖件     ${plan.overlays.length} 个：BIN ${kindCount('bin')} · AGF ${kindCount('agf')} · AGERC ${kindCount('agerc')}` +
      `${kindCount('launcher') ? ` · 启动器 ${kindCount('launcher')}（${LE_LAUNCHER_NAME}）` : ''}` +
      `（共 ${mb(plan.overlays.reduce((a, o) => a + o.buf.length, 0))}）`,
  );
  L.push(`落点       ${ctx.outDir}`);
  L.push(`清单       ${relToRepo(ctx.installManifest)}（生成物：这一棵树是怎么来的）`);
  const prev = readInstallManifest(ctx.installManifest);
  L.push(`同步模式   ${fs.existsSync(ctx.outDir) && fs.readdirSync(ctx.outDir).length ? (prev ? '就地同步（会清掉上一轮的过时覆盖件）' : '**目标已存在且不是本工具建的** ⇒ 缺 --force 就拒绝') : '新建'}`);

  process.stdout.write(`${L.join('\n')}\n`);
  if (problems.length) { reportProblems('测试安装树过不去', problems); return 1; }
  if (changed.warnings.length) for (const w of changed.warnings) process.stderr.write(`⚠ ${w}\n`);
  if (dryRun) {
    process.stdout.write('\n（dry-run）加 --write 落盘。\n');
    return 0;
  }
  const res = writeInstall(ctx, plan, { force: args.force, relabel: args.relabel, log: (m) => process.stdout.write(`${m}\n`) });
  if (!res.ok) { reportProblems('落盘失败（已回滚本次写入）', res.problems); return 1; }
  const s = res.stats;
  process.stdout.write(
    `\n✔ 已同步     ${res.outDir}\n` +
      `             硬链接 ${s.hardlink} · 复制 ${s.copy} · 覆盖 ${s.write} · 跳过 ${s.skip} · 清过时 ${s.prune}（新增字节 ${mb(s.bytes)}）\n` +
      `             回读复验：覆盖件 ${s.verified}/${plan.overlays.length} 逐字节一致 · ALF ${alfs.length} 个 ino 一致\n` +
      `             清单 ${relToRepo(res.manifestPath)}\n`,
  );
  if (res.relabeled) process.stdout.write(`             完整性标签：已改成 Medium（icacls ${res.relabeled === 'ok' ? '成功' : `失败：${res.relabeled}`}）\n`);
  for (const w of res.warnings ?? []) process.stderr.write(`⚠ ${w}\n`);
  return 0;
}

// ─────────────────────────────────────────────────────────── plan / pack

function runPack(args, { dryRun }) {
  const ctx = loadContext({
    base: args.base ?? undefined,
    patch: args.patch ?? undefined,
    baked: args.baked ?? undefined,
    agerc: args.agerc ?? undefined,
    fonts: args.fonts ?? undefined,
    releaseDir: args.releaseDir ?? undefined,
    zip: args.out ? path.resolve(args.out) : null,
    zipDir: args.out ? path.dirname(path.resolve(args.out)) : DEFAULT_PATCH_DIR,
    packManifest: args.out ? path.resolve(args.out).replace(/\.zip$/i, '.manifest.json') : null,
  });
  const changed = collectChanged(ctx, {
    onProgress: (d, t) => { if (d % 100 === 0) process.stdout.write(`… 重建 ${d}/${t}\n`); },
  });
  const plan = packPlan(ctx, changed, { zipPath: ctx.zipPath });
  const problems = [...changed.problems, ...plan.problems];
  const byKind = (k) => plan.rows.filter((r) => r.kind === k);
  const total = plan.rows.reduce((a, r) => a + r.buf.length, 0);

  if (args.json) {
    process.stdout.write(`${JSON.stringify({
      action: 'pack', dryRun, version: plan.version, zipPath: plan.zipPath,
      entries: plan.rows.map((r) => ({ entry: r.entry, kind: r.kind, bytes: r.buf.length, sha256: r.sha256, source: r.source })),
      problems, warnings: changed.warnings,
    }, null, 1)}\n`);
    return problems.length ? 1 : 0;
  }

  const L = printChanged(changed, ctx);
  L.push('');
  L.push(`版本       ${plan.version ? `${plan.version.version}（${plan.version.mark}）` : '**缺版本节**'} ⇒ ${plan.zipName ?? '（算不出 zip 名）'}`);
  L.push(`包含       BIN/ ${byKind('bin').length} · AGF/ ${byKind('agf').length} · AGERC ${byKind('agerc').length}` +
    ` · 字体 ${byKind('font').length}（${byKind('font').map((r) => r.entry).join('、') || '—'}）` +
    ` · 文本 ${byKind('text').length}（${byKind('text').map((r) => r.entry).join('、') || '—'}）`);
  L.push(`           共 ${plan.rows.length} 件 / ${mb(total)}（压缩前）`);
  L.push(`落点       ${plan.zipPath ?? '（未定）'}（+ 同名 .manifest.json）`);
  L.push('判据       ① BIN 集 == patch.json 的键集　② 每支 BIN 与 resultSha 逐字节相同　③ 条目集合与上面两行一致（缺件/多件都红）');

  process.stdout.write(`${L.join('\n')}\n`);
  if (problems.length) { reportProblems('发行包过不去', problems); return 1; }
  if (changed.warnings.length) for (const w of changed.warnings) process.stderr.write(`⚠ ${w}\n`);
  if (dryRun) {
    process.stdout.write('\n（dry-run）加 --write 落盘。\n');
    return 0;
  }
  const res = writePack(ctx, plan);
  if (!res.ok) { reportProblems('落盘失败（zip 已回滚）', res.problems); return 1; }
  process.stdout.write(
    `\n✔ 已打包     ${res.zipPath}（${mb(res.bytes)} · ${res.entries} 件 · sha256 ${res.sha256.slice(0, 16)}…）\n` +
      `             回读复验：${res.entries} 个条目逐字节一致\n` +
      `             清单 ${relToRepo(res.manifestPath)}\n`,
  );

  // 版本节与包的对应关系要**显式**看见（同一次调用里 CHANGELOG 必须与包一起长出来）
  const cl = plan.rows.find((r) => r.entry === 'CHANGELOG.md');
  const v = cl ? changelogVersion(cl.buf.toString('utf8')) : null;
  process.stdout.write(`             CHANGELOG 版本节 ${v ? `${v.version}（${v.mark}）` : '（无）'} 与 zip 名一致：${v?.zipName === path.basename(res.zipPath) ? '✔' : '✖'}\n`);
  return v?.zipName === path.basename(res.zipPath) ? 0 : 1;
}

// ─────────────────────────────────────────────────────────── 入口

export function main(argv = process.argv.slice(2)) {
  let args;
  try {
    args = parseArgs(argv);
  } catch (err) {
    process.stderr.write(`✖ ${err.message}\n${HELP}`);
    return 2;
  }
  if (args.action === 'help') { process.stdout.write(HELP); return 0; }
  if (args.action === 'describe') {
    process.stdout.write(args.json ? `${JSON.stringify(describe(), null, 2)}\n` : describeText());
    return 0;
  }

  const target = args.action === 'plan' ? args.target : args.action;
  const dryRun = args.action === 'plan' || !args.write;
  try {
    if (target === 'install') return runInstall(args, { dryRun });
    if (target === 'pack') return runPack(args, { dryRun });
  } catch (err) {
    process.stderr.write(`✖ ${err.stack ?? err.message}\n`);
    return 2;
  }
  process.stderr.write(`未知动作：${args.action}\n${HELP}`);
  return 2;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  try {
    process.exitCode = main();
  } catch (err) {
    process.stderr.write(`ERROR: ${err.stack ?? err.message}\n`);
    process.exitCode = 2;
  }
}
