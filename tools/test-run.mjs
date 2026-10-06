#!/usr/bin/env node
/**
 * tools/test-run.mjs —— **测试分级的选择器 + 报告器**（唯一入口，取代裸 `node --test`）
 *
 * ## 为什么要它（实测依据，不是设计癖好）
 * 旧形态是**一条命令跑全部**：23 个文件 / 220 用例 / **~28.7 s**，其中
 * `inventory` 13.8 s + `corpus-manifest` 9.8 s 就吃掉 **82%** —— 而这两个是**旧仓盘点**与
 * **清单/LFS/游戏安装**的校验，改一行 `ledger.mjs` 根本用不着它们。
 * 更要紧的是：**13/23 个文件依赖仓库外的东西**，失败/跳过取决于这台机器上有没有旧仓、有没有装游戏。
 * 旧形态下**没有任何命令能回答"我这次是全量绿还是只跑了一半"**。
 *
 * ## 分级怎么定：**写在文件里的 pragma**（不建清单文件 —— 清单会漂，pragma 跟着文件走）
 * 每个 `*.test.mjs` 的**首行**必须是：
 * ```js
 * /** @env pure @kind gate @why 一句话说清"红了意味着什么" *\/
 * ```
 *   * `@env` —— 决定"这台机器上它有没有意义"：
 *     - `pure`     只读仓库内的文本 / 纯函数（快；**默认档**）
 *     - `assets`   需要 LFS 资产或**游戏安装**（控制类内容很稳定 ⇒ 不值得每次跑）
 *     - `external` 需要**旧仓**或真机（迁移 / 收尾 / 探针时才跑）
 *   * `@kind` —— 决定"红了意味着什么"：
 *     - `gate` 不变量 / 清单 / 约定（`AGENTS.md` 落地的那些）
 *     - `contract` 领域能力的行为
 *     - `safety` 写路径不写坏数据（dry-run / 回读复验 / 回滚 / 幂等）
 *     - `product` 产出物满足不变量
 *   * `@why` —— 一句话，给下一个人看"这条红了要不要慌"（**不许写"测试用"这种废话**）
 *
 * ## 用法
 * ```bash
 * node tools/test-run.mjs                 # = pure（默认门禁）
 * node tools/test-run.mjs --env assets    # 只跑 assets
 * node tools/test-run.mjs --all           # 全部
 * node tools/test-run.mjs --list          # 只列集合与声明，不跑（★ 先看这个）
 * ```
 * ★ **不捕获子进程输出**：`node --test` 直接 `stdio: 'inherit'`（受限沙箱里管道会 EPERM）。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

export const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

/** 环境档（顺序即"由轻到重"） */
export const ENVS = ['pure', 'assets', 'external'];
/** 断言类别 */
export const KINDS = ['gate', 'contract', 'safety', 'product'];
/** 默认档：只跑 pure —— 见文件头"为什么要它" */
export const DEFAULT_ENVS = ['pure'];

/** 测试文件住在哪（与 `package.json` 的 test glob 必须一致） */
const SEARCH_DIRS = ['tools/test', 'packages', 'plugins', 'apps'];
const SKIP_DIRS = new Set(['node_modules', 'dist', '.git', '.tmp', '.staging', '.cache']);

/** pragma 行：`/** @env <env> @kind <kind> @why <一句话> *\/` */
const PRAGMA_RE = /^\/\*\*\s*@env\s+(\S+)\s+@kind\s+(\S+)\s+@why\s+(.+?)\s*\*\/\s*$/;

/** 找全部测试文件（排序固定 ⇒ 报错与输出稳定） */
export function findTestFiles(root = REPO_ROOT) {
  const out = [];
  const walk = (dir) => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
      if (e.isDirectory()) {
        if (SKIP_DIRS.has(e.name)) continue;
        walk(path.join(dir, e.name));
      } else if (e.isFile() && e.name.endsWith('.test.mjs')) {
        out.push(path.join(dir, e.name));
      }
    }
  };
  for (const d of SEARCH_DIRS) {
    const abs = path.join(root, d);
    if (fs.existsSync(abs)) walk(abs);
  }
  return out;
}

/**
 * 读一个文件的 pragma 声明（**只读头 40 行** ⇒ 选集合不必加载任何模块，几十毫秒）。
 * @returns {{file:string, rel:string, env?:string, kind?:string, why?:string, problems:string[]}}
 */
export function readPragma(file, root = REPO_ROOT) {
  const rel = path.relative(root, file).split(path.sep).join('/');
  const head = fs.readFileSync(file, 'utf8').split('\n', 40);
  const line = head.find((l) => l.includes('@env') || l.includes('@kind') || l.includes('@why'));
  if (!line) {
    return { file, rel, problems: [`${rel}: 首行缺 pragma 声明（需要 \`/** @env <${ENVS.join('|')}> @kind <${KINDS.join('|')}> @why <一句话> */\`）`] };
  }
  const m = PRAGMA_RE.exec(line.trim());
  if (!m) {
    return { file, rel, problems: [`${rel}: pragma 形态非法 → \`${line.trim()}\`（应为 \`/** @env <${ENVS.join('|')}> @kind <${KINDS.join('|')}> @why <一句话> */\`）`] };
  }
  const [, env, kind, why] = m;
  const problems = [];
  if (!ENVS.includes(env)) problems.push(`${rel}: @env 非法 ${JSON.stringify(env)}（应为 ${ENVS.join('/')}）`);
  if (!KINDS.includes(kind)) problems.push(`${rel}: @kind 非法 ${JSON.stringify(kind)}（应为 ${KINDS.join('/')}）`);
  if (why.trim().length < 8) problems.push(`${rel}: @why 太短（要一句话说清"红了意味着什么"，别写"测试用"）`);
  return { file, rel, env, kind, why, problems };
}

/** 全部文件的声明 + 问题清单 */
export function survey(root = REPO_ROOT) {
  const decls = findTestFiles(root).map((f) => readPragma(f, root));
  return { decls, problems: decls.flatMap((d) => d.problems) };
}

function parseArgs(argv) {
  const out = { envs: [...DEFAULT_ENVS], all: false, list: false, only: null };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--all') out.all = true;
    else if (a === '--list') out.list = true;
    else if (a === '--env') out.envs = String(argv[++i] ?? '').split(',').map((s) => s.trim()).filter(Boolean);
    else if (a === '--kind') out.only = String(argv[++i] ?? '');
    else if (a === '--help' || a === '-h') out.help = true;
    else throw new Error(`不认识的选项：${a}`);
  }
  return out;
}

const HELP = `tools/test-run.mjs —— 测试分级选择器（默认只跑 @env pure）

  node tools/test-run.mjs                  # pure（日常门禁）
  node tools/test-run.mjs --list           # ★ 先看集合与各文件声明，不跑
  node tools/test-run.mjs --env assets     # 只跑 assets（要 LFS/游戏安装）
  node tools/test-run.mjs --env pure,assets
  node tools/test-run.mjs --all            # 全部（含 external：旧仓/真机）
  node tools/test-run.mjs --kind gate      # 只看某一类（与 --env 可叠）

★ 分级写在每个 *.test.mjs 的首行 pragma 里；判据与"为什么"见本文件头注释。
★ 不捕获子进程输出（受限沙箱里管道会 EPERM）—— node --test 直接继承 stdio。
`;

export function main(argv = process.argv.slice(2)) {
  const args = parseArgs(argv);
  if (args.help) {
    process.stdout.write(HELP);
    return 0;
  }
  const { decls, problems } = survey();

  // ★ 声明不齐一律红（旧仓 R1；判据 = "档位不许靠记得"）
  if (problems.length) {
    process.stderr.write(`✗ 测试分级声明有问题（${problems.length} 处）：\n  - ${problems.join('\n  - ')}\n\n`);
    return 1;
  }

  const envs = args.all ? ENVS : args.envs;
  for (const e of envs) if (!ENVS.includes(e)) throw new Error(`--env 非法：${e}（应为 ${ENVS.join('/')}）`);

  let picked = decls.filter((d) => envs.includes(d.env));
  if (args.only) picked = picked.filter((d) => d.kind === args.only);
  const skipped = decls.filter((d) => !picked.includes(d));

  if (args.list) {
    const L = [`测试分级（${decls.length} 个文件）`, ''];
    for (const e of ENVS) {
      const xs = decls.filter((d) => d.env === e);
      if (!xs.length) continue;
      L.push(`${e}（${xs.length}）${envs.includes(e) ? '' : '  ← 本次不跑'}`);
      for (const d of xs) L.push(`  [${d.kind}] ${d.rel}　—　${d.why}`);
      L.push('');
    }
    process.stdout.write(`${L.join('\n')}\n`);
    return 0;
  }

  const L = [
    `档位        ${envs.join(' + ')}${args.only ? `　·　仅 kind=${args.only}` : ''}`,
    `本次跑      ${picked.length} 个文件`,
    `本次不跑    ${skipped.length} 个${skipped.length ? `　（${[...new Set(skipped.map((d) => d.env))].join(' / ')} ⇒ \`pnpm test:all\` 跑全量）` : ''}`,
    '',
  ];
  process.stdout.write(`${L.join('\n')}\n`);
  if (picked.length === 0) {
    process.stdout.write('（没有匹配的测试文件）\n');
    return 0;
  }

  // ★ stdio 继承：不捕获（受限沙箱的管道会 EPERM）；退出码直接透传
  const res = spawnSync(process.execPath, ['--test', '--test-isolation=none', ...picked.map((d) => d.file)], {
    cwd: REPO_ROOT,
    stdio: 'inherit',
  });
  return res.status ?? 1;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  try {
    process.exitCode = main();
  } catch (err) {
    process.stderr.write(`ERROR: ${err.message}\n`);
    process.exitCode = 2;
  }
}
