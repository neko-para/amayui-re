/**
 * plugins/pnpm-priv/lib/ops.mjs —— **闭接口**：能被"特权地"做的事**只有这张表里的几件**
 *
 * ## 为什么需要这个插件
 * Node 的**原生类型剥离**（跑 `.ts` / `.mts` 不用构建）**拒绝为 `node_modules` 下的文件剥类型**：
 * `ERR_UNSUPPORTED_NODE_MODULES_TYPE_STRIPPING`。而"跨包用包名导入"必然把文件解析到
 * `node_modules/@amayui/<包>` 下面 —— 于是**包名导入**与**跨包 `.mts`** 在 Node 上互斥。
 *
 * ★ 实测（2026-10，本机）证明了绕过条件：
 *   * 经 **真符号链接**（`lstat.isSymbolicLink === true`，realpath 落在 `node_modules` **之外**）导入 `.mts` ⇒ **成功**；
 *   * 经 **junction**（`isSymbolicLink === false`，realpath 仍在 `node_modules` 内）⇒ **拒绝**；
 *   * 纯真实路径 / `./node_modules/../../x.mts` 这种"URL 里有、解析路径没有"的形态 ⇒ **成功**。
 *   ⇒ 判据是**解析后的路径含不含 `node_modules`**，不是"是不是包名导入"。
 *
 * ## 为什么这需要特权
 * Windows 上建**真**目录符号链接要 `SeCreateSymbolicLinkPrivilege`。沙箱里的 `pnpm install`
 * 拿不到它 ⇒ pnpm **静默降级成 junction**（装完不报错、链接形态却是错的）⇒ 上面那条路走不通。
 * 这就是本插件存在的唯一理由：**在宿主里跑 `pnpm install`**，让它能建真符号链接。
 *
 * ## 与 `plugins/deploy` 同一套安全口径
 * ① **闭接口**：op 只有下表那几个；`pnpm` 的 verb 只允许白名单里的；
 * ② **必须落在本仓内**：`cwd` 一律取校验过的仓库根，**不接任意目录**（见 `planOp` 的 `repoRoot` 检查）；
 * ③ **argv 数组不经 shell**：拼不出注入，也给不出新 flag；
 * ④ **写类 op 默认 dry-run**：`install` / `add` / `remove` 必须显式 `write:true`。
 *
 * ★ 本文件是**纯逻辑**（不碰 fs、不打印）：路径校验走注入的 `io` ⇒ 单测能覆盖"越界仓库被拒"。
 */
import fs from 'node:fs';
import path from 'node:path';

/** 允许宿主执行的 pnpm **动词**（白名单；不给任意脚本） */
export const ALLOWED_VERBS = ['install', 'add', 'remove'];

/** 默认允许操作的仓库根（`undefined` ⇒ 用插件自己的上溯两级，见 `index.mjs`） */
export const DEFAULT_ALLOWED_ROOTS = [];

/**
 * 默认 io 必须是真 fs（同 `plugins/deploy/lib/ops.mjs` 的教训：拿"什么都说不存在"的桩去校验，
 * 会把所有路径判成不存在 ⇒ 自己把自己拒了）。
 */
export const realIo = {
  exists: (p) => fs.existsSync(p),
  isDir: (p) => {
    try { return fs.statSync(p).isDirectory(); } catch { return false; }
  },
};

export const OPS = {
  status: {
    kind: 'status',
    write: false,
    summary:
      '报告**链接形态与特权**事实：本插件进程能否建真符号链接（探针自清）、`pnpm store path`、' +
      '以及各 workspace 消费者的 `node_modules/@amayui/*` 是 symlink 还是 junction',
  },
  'check-links': {
    kind: 'check-links',
    write: false,
    summary:
      '逐个检查 `node_modules/@amayui/*` 的链接形态：`isSymbolicLink === false` 的**点名**（那种链接下 ' +
      'Node 不能对 `.mts` 剥类型 ⇒ `ERR_UNSUPPORTED_NODE_MODULES_TYPE_STRIPPING`）',
  },
  install: {
    kind: 'pnpm',
    write: true,
    verb: 'install',
    summary: '`pnpm install`（宿主侧 ⇒ 能建真符号链接）+ 装完**复验链接形态**（`pnpm install --frozen-lockfile` 可选）',
  },
  add: {
    kind: 'pnpm',
    write: true,
    verb: 'add',
    summary: '`pnpm add [--save-dev] <spec>`（只接受包名/版本范围形态的 spec）+ 复验链接',
  },
  remove: {
    kind: 'pnpm',
    write: true,
    verb: 'remove',
    summary: '`pnpm remove <spec>` + 复验链接',
  },
};

export const OP_NAMES = Object.keys(OPS);

/**
 * 一个"包名 spec"的合法形态：`@scope/name` 或 `name`，可带 `@range`。
 * ★ 刻意**不接受** `../x` / `file:` / `link:` / 带 shell 元字符的东西 —— 那些要么越过闭接口、
 *   要么把"装依赖"变成"改别处的东西"。
 */
const SPEC_RE = /^(?:@[a-z0-9][a-z0-9._-]*\/)?[a-z0-9][a-z0-9._-]*(?:@[~^]?[0-9][0-9a-z.\-+]*|@(?:latest|next|beta|rc|workspace:\*))?$/i;

/** 这个路径是不是"某个允许的根之内"（按**路径段**比较，不是字符串前缀） */
export function insideAnyRoot(target, roots) {
  const t = path.resolve(target);
  return roots.some((r) => {
    const rel = path.relative(path.resolve(r), t);
    return rel === '' || (!rel.startsWith('..') && !path.isAbsolute(rel));
  });
}

/**
 * 把一次调用**校验并翻译成"要做的事"**（不执行任何东西）。
 *
 * @param {string} op `OPS` 里的 op
 * @param {object} args 模型给的参数
 * @param {{repoRoot:string, allowedRoots?:string[], io?:object}} env
 * @returns {{problems:string[], plan?:object}} `problems` 非空 ⇒ **一个字都不做**
 */
export function planOp(op, args = {}, env = {}) {
  const problems = [];
  const io = env.io ?? realIo;
  const raw = args ?? {};
  const spec = OPS[op];
  if (!spec) return { problems: [`不认识的 op：${JSON.stringify(op)}（只有：${OP_NAMES.join(' / ')}）`] };

  // ── ① 仓库根：**必须**显式给出、必须是目录、必须落在允许的根内
  const repoRoot = typeof raw.repoRoot === 'string' && raw.repoRoot.trim() !== '' ? path.resolve(raw.repoRoot) : null;
  if (!repoRoot) {
    return { problems: ['repoRoot 必须是非空绝对路径 —— 本插件**只**在明确的仓库根里动作（不接相对路径、不接"当前目录"）'] };
  }
  if (!path.isAbsolute(raw.repoRoot)) {
    return { problems: [`repoRoot 必须是绝对路径（实际 ${JSON.stringify(raw.repoRoot)}）—— 相对路径不足以谈"允许的根"`] };
  }
  const roots = (env.allowedRoots ?? DEFAULT_ALLOWED_ROOTS).map((r) => path.resolve(r));
  if (roots.length > 0 && !insideAnyRoot(repoRoot, roots)) {
    return { problems: [`repoRoot 不在允许的根内：${repoRoot}（允许：${roots.join(' / ')}）—— 要装别处就改插件配置的 allowedRoots`] };
  }
  // ★ "这个目录是不是一个仓库"：必须同时有 package.json 与 pnpm-workspace.yaml
  //   （只用 package.json 会把任意 npm 包目录也放进来；`pnpm install` 在这里会改别处）
  for (const f of ['package.json', 'pnpm-workspace.yaml']) {
    if (!io.exists(path.join(repoRoot, f))) problems.push(`repoRoot 里没有 ${f}：${repoRoot}（本插件只在 pnpm workspace 根里动作）`);
  }
  if (problems.length) return { problems };

  if (op === 'status') return { problems, plan: { op, kind: 'status', write: false, repoRoot } };

  if (op === 'check-links') {
    return { problems, plan: { op, kind: 'check-links', write: false, repoRoot, dirs: Array.isArray(raw.dirs) ? raw.dirs.map(String) : undefined } };
  }

  // ── ② 写类 op：必须显式 write:true（"默认 dry-run"这条不变量不许是假的）
  const write = raw.write === true;
  const verb = spec.verb;
  if (raw.verb !== undefined) {
    if (typeof raw.verb !== 'string' || !ALLOWED_VERBS.includes(raw.verb)) {
      problems.push(`verb 只能是 ${ALLOWED_VERBS.join(' / ')}（实际 ${JSON.stringify(raw.verb)}）`);
    }
  }
  const chosenVerb = typeof raw.verb === 'string' && ALLOWED_VERBS.includes(raw.verb) ? raw.verb : verb;
  if (op === 'install' && typeof raw.verb === 'string' && raw.verb !== 'install') {
    problems.push('op=install 的 verb 只能是 install（要 add/remove 请用对应的 op）');
  }
  if (op !== 'install' && chosenVerb !== verb) {
    problems.push(`op=${op} 的 verb 只能是 ${verb}`);
  }

  const argv = [chosenVerb];
  if (op === 'install') {
    if (raw.frozenLockfile === true) argv.push('--frozen-lockfile');
    if (raw.force === true) argv.push('--force');
  } else {
    const pkg = raw.package;
    if (typeof pkg !== 'string' || !SPEC_RE.test(pkg.trim())) {
      problems.push(`${op} 需要 package（合法包名形态：name 或 @scope/name，可带 @range）—— 不接受路径 / file: / link: / shell 元字符：${JSON.stringify(raw.package)}`);
    } else {
      if (raw.dev === true) argv.push('-D');
      if (raw.workspace === true) argv.push('-w');
      argv.push(pkg.trim());
    }
  }
  if (write) argv.push('--config.confirmModulesPurge=false');
  if (problems.length) return { problems };
  return { problems, plan: { op, kind: 'pnpm', write, verb: chosenVerb, argv, repoRoot } };
}

/** 审计用的一行（**一条记录一行**；换行/制表一律压平） */
export function auditLine(rec) {
  const clean = (s, n = 300) => String(s ?? '').replace(/[\r\n\t]+/g, ' ').slice(0, n);
  return [
    rec.at,
    `op=${clean(rec.op, 40)}`,
    `repo=${clean(rec.repo, 200)}`,
    `write=${rec.write ? 1 : 0}`,
    rec.cmd ? `cmd=${clean(rec.cmd, 200)}` : null,
    `ok=${rec.ok ? 1 : 0}`,
    rec.code === null || rec.code === undefined ? null : `code=${rec.code}`,
    `ms=${Math.max(0, Math.round(Number(rec.ms) || 0))}`,
    rec.note ? `note=${clean(rec.note)}` : null,
  ]
    .filter(Boolean)
    .join(' ');
}
