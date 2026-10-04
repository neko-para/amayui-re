/**
 * plugins/deploy/lib/ops.mjs —— **闭接口**：能被"特权地"做的事**只有这张表里的几件**
 *
 * ## 为什么必须闭
 * 插件跑在 DSH 宿主进程里（不受 ACL 沙箱约束，见 `../README.md`），所以它等于"一把上了膛的枪"。
 * 凡是"接受任意命令"的形态（`op:'run', cmd:'…'`）都等于把沙箱整体拆掉 —— 那正是 setuid 时代的
 * **confused deputy** 教训（现代系统改用窄接口的中介：Nix daemon / XDG Desktop Portal / polkit）。
 * 于是这里只有：
 *
 * | op | 做什么 | 需要 `write: true` |
 * |---|---|---|
 * | `status` | 报告宿主进程的事实（**含"宿主起的子进程是什么完整性"** ⇒ 验证不受沙箱约束） | 否（只读） |
 * | `probe-link` | 对游戏安装目录建一个硬链接、验完**立刻删** | 否（只动元数据、自清） |
 * | `probe-write` | 在指定目录写一个临时件、验完**立刻删** | 否（自清） |
 * | `install-tree` | `pnpm tools release install`（ALF 硬链接 / 覆盖件） | **是** |
 * | `bake-ui` | `pnpm tools ui-bake build`（headless Chrome） | **是** |
 * | `relabel-medium` | `icacls <树> /setintegritylevel Medium /T /C` | **是** |
 *
 * ★ 每个 op 的 argv 都是**在这里拼死的形状**：调用方只能给"受校验的参数"（路径必须落在允许的根内、
 *   `alf` 只能是枚举值、`leProfile` 必须是 GUID 形态…），**给不出新 flag、也拼不出 shell**（一律 argv 数组，
 *   不经 shell）。新增能力 = 改这张表 + 改 README，而不是"让调用方自己写命令"。
 * ★ 本文件是**纯逻辑**（不碰 fs、不打印）：路径与参数校验走注入的 `io` ⇒ 单测能覆盖"越界路径被拒"。
 */
import fs from 'node:fs';
import path from 'node:path';

/** 默认允许写入的根（相对 `repoRoot`；用户可在插件配置里覆盖成工作区外的目录） */
export const DEFAULT_ALLOWED_ROOTS = ['dist'];

/**
 * **默认 io 必须是真 fs**：单测会注入假 io（让判据与机器无关），但插件跑起来时如果不给 io，
 * 拿"什么都说不存在"的桩去校验就会把**所有**路径判成不存在（实测踩过：`probe-write` 被自己拒了）。
 */
export const realIo = {
  exists: (p) => fs.existsSync(p),
  isDir: (p) => {
    try {
      return fs.statSync(p).isDirectory();
    } catch {
      return false;
    }
  },
};

/** 哪几件事值得"不受沙箱约束地"做 */
export const OPS = {
  status: {
    kind: 'status',
    write: false,
    summary: '报告宿主进程的事实：宿主起的子进程的完整性级别（沙箱里起不出 Medium）、node/插件路径等',
  },
  'probe-link': {
    kind: 'probe-link',
    write: false,
    summary: '对游戏安装目录里的一个 ALF 建硬链接、核对 inode 后**立刻删**（受限沙箱里必 EPERM/EACCES）',
  },
  'probe-write': {
    kind: 'probe-write',
    write: false,
    summary: '在指定目录写一个临时件、回读后**立刻删**（用来验"工作区外也能写"）',
  },
  'install-tree': {
    kind: 'cli',
    write: true,
    cli: ['release', 'install'],
    summary: '同步测试安装树 → `pnpm tools release install`（ALF 硬链接；`write:false` 时只出 dry-run 计划）',
  },
  'bake-ui': {
    kind: 'cli',
    write: true,
    cli: ['ui-bake', 'build'],
    summary: '烧 UI 图 → `pnpm tools ui-bake build`（要 headless Chrome，沙箱里起不来）',
  },
  'relabel-medium': {
    kind: 'icacls',
    write: true,
    summary: '把一棵树的完整性标签改成 Medium（`icacls <树> /setintegritylevel Medium /T /C`）',
  },
};

export const OP_NAMES = Object.keys(OPS);

/** `alf` 的合法取值（透传给 `release install`） */
const ALF_MODES = ['hardlink', 'copy'];
/** LE 配置档是 GUID（透传给 `release install --le-profile`） */
const GUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

/** 这个路径是不是"某个允许的根之内"（**按路径段比较**，不是字符串前缀 —— `/dist-other` 不算 `/dist` 之内） */
export function insideAnyRoot(target, roots) {
  const t = path.resolve(target);
  return roots.some((r) => {
    const rel = path.relative(path.resolve(r), t);
    return rel === '' || (!rel.startsWith('..') && !path.isAbsolute(rel));
  });
}

/** 根目录本身 / 盘符根 / 用户主目录：`--out` 指到这些地方一律拒（一次误操作代价太大） */
function dangerousOut(target) {
  const t = path.resolve(target);
  if (t === path.parse(t).root) return `落点是盘符根：${t}`;
  const home = process.env.USERPROFILE ?? process.env.HOME;
  if (home && path.resolve(home) === t) return `落点是用户主目录：${t}`;
  return null;
}

/**
 * 把一次调用**校验并翻译成"要做的事"**（不执行任何东西）。
 *
 * @param {string} op `OPS` 里的 op
 * @param {object} args 模型给的参数
 * @param {{repoRoot:string, allowedRoots?:string[], io?:{exists:(p:string)=>boolean, isDir:(p:string)=>boolean}}} env
 * @returns {{problems:string[], plan?:object}} `problems` 非空 ⇒ **一个字都不做**
 */
export function planOp(op, args = {}, env = {}) {
  const problems = [];
  const io = env.io ?? realIo;
  const repoRoot = path.resolve(env.repoRoot ?? '.');
  const roots = (env.allowedRoots ?? DEFAULT_ALLOWED_ROOTS).map((r) => path.resolve(repoRoot, r));
  const raw = args ?? {};
  const OPS_KNOWN = OPS[op];
  if (!OPS_KNOWN) {
    return { problems: [`不认识的 op：${JSON.stringify(op)}（只有：${OP_NAMES.join(' / ')}）`] };
  }
  const write = raw.write === true;
  if (write && !OPS_KNOWN.write) problems.push(`op=${op} 不接受 write:true（它自己不写东西，只动元数据并自清）`);

  const checkOut = (value, { mustExist = false, mustBeDir = false } = {}) => {
    if (typeof value !== 'string' || value.trim() === '') {
      problems.push('out 必须是非空绝对路径');
      return null;
    }
    if (!path.isAbsolute(value)) {
      problems.push(`out 必须是绝对路径（实际 ${JSON.stringify(value)}）—— 相对路径不足以谈"允许的根"`);
      return null;
    }
    const bad = dangerousOut(value);
    if (bad) problems.push(bad);
    if (!insideAnyRoot(value, roots)) {
      problems.push(`out 不在允许的根内：${path.resolve(value)}（允许：${roots.join(' / ')}）—— 要写到别处就改插件配置的 allowedRoots`);
      return null;
    }
    if ((mustExist || mustBeDir) && !io.exists(value)) problems.push(`out 不存在：${value}`);
    else if (mustBeDir && !io.isDir(value)) problems.push(`out 不是目录：${value}`);
    return path.resolve(value);
  };

  if (op === 'status') return { problems, plan: { op, kind: 'status', write: false } };

  if (op === 'probe-link' || op === 'probe-write') {
    const dir = raw.dir;
    if (typeof dir !== 'string' || !path.isAbsolute(dir)) {
      problems.push(`${op} 需要 dir（非空绝对路径）：硬链接/临时件要落在与目标**同一卷**的目录里`);
      return { problems };
    }
    if (op === 'probe-write' && !io.isDir(dir)) problems.push(`dir 不是目录：${dir}`);
    return { problems, plan: { op, kind: OPS[op].kind, write: false, dir: path.resolve(dir) } };
  }

  if (op === 'relabel-medium') {
    const out = checkOut(raw.out, { mustBeDir: true });
    return { problems, plan: out ? { op, kind: 'icacls', write: true, out } : undefined };
  }

  // install-tree / bake-ui：拼死形状的 argv，只透传**受校验的**值
  if (op === 'install-tree') {
    const out = checkOut(raw.out);
    const argv = [...OPS[op].cli];
    if (out) argv.push('--out', out);
    if (raw.baked !== undefined) {
      if (typeof raw.baked !== 'string' || !path.isAbsolute(raw.baked) || !io.isDir(raw.baked)) problems.push(`baked 必须是一个存在的绝对目录：${JSON.stringify(raw.baked)}`);
      else argv.push('--baked', path.resolve(raw.baked));
    }
    if (raw.alf !== undefined) {
      if (!ALF_MODES.includes(raw.alf)) problems.push(`alf 只能是 ${ALF_MODES.join(' / ')}`);
      else argv.push('--alf', raw.alf);
    }
    if (raw.leCmd !== undefined) {
      if (typeof raw.leCmd !== 'string' || !path.isAbsolute(raw.leCmd) || !io.exists(raw.leCmd)) problems.push(`leCmd 必须是一个存在的绝对文件：${JSON.stringify(raw.leCmd)}`);
      else argv.push('--le-cmd', path.resolve(raw.leCmd));
    }
    if (raw.leProfile !== undefined) {
      if (typeof raw.leProfile !== 'string' || !GUID_RE.test(raw.leProfile)) problems.push(`leProfile 必须是 GUID 形态：${JSON.stringify(raw.leProfile)}`);
      else argv.push('--le-profile', raw.leProfile);
    }
    if (raw.relabelMedium === true) argv.push('--relabel-medium');
    if (write) argv.push('--write');
    return { problems, plan: { op, kind: 'cli', write, argv, out: out ?? null } };
  }

  if (op === 'bake-ui') {
    // ★ `ui-bake build` **没有 dry-run**：它一定会写 `dist/ui-bake/`（或 --out）。
    //   所以必须显式 write:true 才放行 —— 否则"写类 op 缺省 dry-run"这条不变量就是假的。
    if (!write) {
      problems.push(
        'bake-ui 必须显式给 write:true：`ui-bake build` 没有 dry-run（它一定会写 AGF 产物）——' +
          '只出计划请用只读动作（`pnpm tools ui-bake plan|verify`，不经本插件）',
      );
      return { problems };
    }
    const argv = [...OPS[op].cli];
    const out = raw.out === undefined ? null : checkOut(raw.out);
    if (out) argv.push('--out', out);
    argv.push('--write');
    return { problems, plan: { op, kind: 'cli', write, argv, out } };
  }

  problems.push(`op=${op} 还没实现（这是代码 bug：OPS 里有、planOp 里没有）`);
  return { problems };
}

/** 审计用的一行（**一条记录一行**；换行/制表一律压平，免得把台账写坏） */
export function auditLine(rec) {
  const clean = (s, n = 300) => String(s ?? '').replace(/[\r\n\t]+/g, ' ').slice(0, n);
  return [
    rec.at,
    `op=${clean(rec.op, 40)}`,
    `write=${rec.write ? 1 : 0}`,
    rec.out ? `out=${clean(rec.out)}` : null,
    `ok=${rec.ok ? 1 : 0}`,
    rec.code === null || rec.code === undefined ? null : `code=${rec.code}`,
    `ms=${Math.max(0, Math.round(Number(rec.ms) || 0))}`,
    rec.note ? `note=${clean(rec.note)}` : null,
  ]
    .filter(Boolean)
    .join(' ');
}
