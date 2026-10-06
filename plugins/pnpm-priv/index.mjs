/**
 * plugins/pnpm-priv/index.mjs —— DSH **宿主侧**插件：注册唯一一个工具 `pnpm_priv`
 *
 * ## 它解决什么问题
 * Node 的**原生类型剥离**拒绝为 `node_modules` 下的文件剥类型
 * （`ERR_UNSUPPORTED_NODE_MODULES_TYPE_STRIPPING`）⇒ "跨包用包名导入 `.mts`" 走不通。
 * 实测的绕过条件是**真符号链接**（`lstat.isSymbolicLink === true`，realpath 落在 `node_modules` 之外），
 * 而 Windows 上建真符号链接要 `SeCreateSymbolicLinkPrivilege` —— 沙箱里的 `pnpm install`
 * 拿不到它，会**静默降级成 junction**。所以这一步必须搬到宿主里做。
 *
 * ## 三条不许破的线（与 `plugins/deploy` 同一套）
 * ① **闭接口**：op 只有 `lib/ops.mjs` 的 `OPS` 表里那几个；pnpm 的 verb 只允许白名单里的；
 * ② **必须落在本仓内**：`repoRoot` 由调用方给出、由 `planOp` 校验（绝对路径 + 有 `package.json` 与
 *    `pnpm-workspace.yaml` + 落在 `allowedRoots` 内）；**不接任意目录、不接"当前目录"**；
 * ③ **写类 op 默认 dry-run**：`install` / `add` / `remove` 缺 `write:true` 时**一个字都不动**；
 * ④ **每次留痕**：无论成败都往 `data/privileged-audit.log` 追加一行（append-only）。
 */
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import z from '@deepseek-ai/schemastery';
import { defineTool } from '@deepseek-ai/dsh-tools';

import { appendAudit, DEFAULT_AUDIT } from './lib/audit.mjs';
import { OPS, OP_NAMES, auditLine, planOp, realIo } from './lib/ops.mjs';
import { canSymlink, findAmayuiLinks, storePathHint } from './lib/probe.mjs';
import { resolvePnpm, runPnpm, tail } from './lib/run.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
/** 插件自己的上溯两级 = 本仓根（`plugins/pnpm-priv/` ⇒ 仓库根）；调用方仍须显式给 repoRoot 并被校验 */
export const SELF_REPO_ROOT = path.resolve(HERE, '..', '..');

/** Stable Loader identity. */
export const name = 'amayui-pnpm-priv';
/** 只依赖工具注册表 */
export const inject = ['tools'];

/**
 * 部署方（人）在 profile 的 loader entry 里配的东西 —— **这一层就是"授权"**。
 * `allowedRoots` 缺省 = 空数组 ⇒ **不限制**（但 `repoRoot` 仍必须过"是不是 workspace 根"的检查）；
 * 要收紧就写成 `['E:\\Projects\\amayui-re']`。
 */
export const Config = z.object({
  allowedRoots: z.array(z.string()).default([]),
  auditFile: z.string().default(DEFAULT_AUDIT),
});

const DESCRIPTION = [
  '提权安装依赖（**宿主侧特权**）：让 `pnpm` 以**真符号链接**建 workspace 链接，而不是 junction。',
  '★ 为什么需要它：Node 的原生类型剥离**拒绝**为 `node_modules` 下的文件剥类型，',
  '  而"跨包用包名导入 `.mts`"必然解析到 `node_modules/@amayui/<包>` ⇒ 那条路只在**真符号链接**下走得通',
  '  （实测：symbolic link 成功 / junction 报 ERR_UNSUPPORTED_NODE_MODULES_TYPE_STRIPPING）。',
  '  沙箱里 `pnpm install` 拿不到 SeCreateSymbolicLinkPrivilege ⇒ 会静默降级成 junction ⇒ 所以搬到宿主里做。',
  `op 只有这些：${OP_NAMES.join(' / ')}。`,
  '· `status`：先跑这个 —— 报告**本插件进程能否建真符号链接**（探针自清）、pnpm 入口、以及各消费方的链接形态；',
  '· `check-links`：逐个点名 `node_modules/@amayui/*` 里**不是真符号链接**的那些；',
  '· `install` / `add` / `remove`：跑 pnpm（verb 白名单），装完**自动复验链接形态**（缺 `write:true` 时只出计划）。',
  '安全口径：op 是闭集合 · `repoRoot` 必须是 pnpm workspace 根 · argv 数组不经 shell · 写类 op 默认 dry-run · 每次留痕。',
].join(' ');

/** 把一组链接检查结果渲染成几行（超标的点名前 10 个） */
function renderLinks(links) {
  if (!links.length) return '（没找到任何 `node_modules/@amayui/*` 链接）';
  const bad = links.filter((l) => l.kind !== 'symlink');
  const head = `链接 ${links.length} 个：**真符号链接 ${links.length - bad.length}** · 不是的 ${bad.length}`;
  if (!bad.length) return `${head}\n  全部是真符号链接 ⇒ Node 能对 \`.mts\` 剥类型`;
  const list = bad.slice(0, 10).map((l) => `  ✖ ${l.consumer}/@amayui/${l.name}（${l.kind}；realpath ${l.real ?? '—'}）`);
  return `${head}\n  这些**不是真符号链接** ⇒ 经它们导入 \`.mts\` 会 ERR_UNSUPPORTED_NODE_MODULES_TYPE_STRIPPING：\n${list.join('\n')}${bad.length > 10 ? `\n  …另有 ${bad.length - 10} 个` : ''}`;
}

export function apply(ctx, config) {
  const cfg = { allowedRoots: [...(config?.allowedRoots ?? [])], auditFile: config?.auditFile ?? DEFAULT_AUDIT };
  ctx.tools.register(
    defineTool({
      name: 'pnpm_priv',
      description: DESCRIPTION,
      parameters: {
        op: { type: 'string', required: true, enum: OP_NAMES, description: '要做的那一件事（闭集合，见工具描述）' },
        repoRoot: { type: 'string', description: '仓库根（**必须是绝对路径**、且是 pnpm workspace 根：同时有 package.json 与 pnpm-workspace.yaml）' },
        write: { type: 'boolean', description: '写类 op 必须显式 true 才真的跑 pnpm；缺省/ false = 只出计划' },
        package: { type: 'string', description: 'add / remove 用：合法包名形态（name 或 @scope/name，可带 @range）——不接受路径 / file: / link:' },
        dev: { type: 'boolean', description: 'add 用：加 -D（写进 devDependencies）' },
        workspace: { type: 'boolean', description: 'add / remove 用：加 -w（作用于 workspace 根）' },
        frozenLockfile: { type: 'boolean', description: 'install 用：加 --frozen-lockfile' },
        force: { type: 'boolean', description: 'install 用：加 --force（重建整棵 node_modules）' },
      },
      output: {
        schema: {
          type: 'object',
          additionalProperties: false,
          properties: {
            op: { type: 'string', required: true },
            ok: { type: 'boolean', required: true },
            summary: { type: 'string', required: true },
            details: { type: 'object', additionalProperties: true },
          },
        },
        render: (_args, value) => [{ type: 'text', text: value.summary }],
      },
      async execute(args, exec) {
        const t0 = Date.now();
        const record = (ok, note, code = null, repo = null, cmd = null) => {
          const line = auditLine({ at: new Date().toISOString(), op: args.op, repo, write: args.write === true, cmd, ok, code, ms: Date.now() - t0, note });
          const res = appendAudit(SELF_REPO_ROOT, line, cfg.auditFile);
          return res.ok ? `审计：${path.relative(SELF_REPO_ROOT, res.file)}（${res.lines} 行）` : `审计写不动：${res.reason}`;
        };

        // ① 校验：不绿一个字都不做
        const { problems, plan } = planOp(args.op, args, { repoRoot: SELF_REPO_ROOT, allowedRoots: cfg.allowedRoots, io: realIo });
        if (problems.length) {
          const note = record(false, `校验不过：${problems[0]}`, null, args.repoRoot ?? null);
          return { op: args.op, ok: false, summary: `✖ ${args.op} 校验不过（**什么都没做**）：\n- ${problems.join('\n- ')}\n${note}`, details: { problems } };
        }

        // ② status：报告特权与链接事实（探针在临时目录自清）
        if (args.op === 'status') {
          const sym = canSymlink();
          const links = findAmayuiLinks(plan.repoRoot);
          const pnpm = resolvePnpm();
          const note = record(sym.ok, sym.ok ? '可建真符号链接' : sym.why, sym.code ?? null, plan.repoRoot);
          return {
            op: 'status',
            ok: sym.ok,
            summary:
              `${sym.ok ? '✔' : '✖'} **宿主特权**：${sym.why}\n` +
              `　pnpm 入口：${pnpm ? pnpm.display : '**找不到**（PATH / npm_execpath 都没有）'}\n` +
              `　store 候选：${storePathHint(plan.repoRoot).join(' · ') || '（没找到）'}\n` +
              `**链接形态**（${path.relative(SELF_REPO_ROOT, plan.repoRoot) || plan.repoRoot}）：\n${renderLinks(links)}\n${note}`,
            details: { symlinkOk: sym.ok, why: sym.why, pnpm: pnpm?.display ?? null, links },
          };
        }

        // ③ check-links：只读点名
        if (args.op === 'check-links') {
          const links = findAmayuiLinks(plan.repoRoot);
          const bad = links.filter((l) => l.kind !== 'symlink');
          const note = record(bad.length === 0, `非符号链接 ${bad.length} / ${links.length}`, null, plan.repoRoot);
          return { op: 'check-links', ok: bad.length === 0, summary: `${renderLinks(links)}\n${note}`, details: { links, bad: bad.length } };
        }

        // ④ install / add / remove：dry-run 或真跑 + 复验
        if (plan.kind === 'pnpm') {
          if (!plan.write) {
            const note = record(true, 'dry-run（未写）', null, plan.repoRoot, `pnpm ${plan.argv.join(' ')}`);
            return {
              op: args.op,
              ok: true,
              summary: `（未给 write:true ⇒ **这是计划，没有跑 pnpm**）\n命令：pnpm ${plan.argv.join(' ')}\n　在 ${plan.repoRoot}\n${note}`,
              details: { plan: plan.argv, repoRoot: plan.repoRoot },
            };
          }
          const r = await runPnpm({ repoRoot: plan.repoRoot, argv: plan.argv, signal: exec?.signal });
          const links = findAmayuiLinks(plan.repoRoot);
          const bad = links.filter((l) => l.kind !== 'symlink');
          const note = record(r.ok, r.cmd, r.code, plan.repoRoot, r.cmd);
          return {
            op: args.op,
            ok: r.ok,
            summary:
              `${r.ok ? '✔' : '✖'} ${args.op}：pnpm ${plan.argv.join(' ')} ⇒ ${r.ok ? '退出 0' : `退出 ${r.code}`}（${r.ms} ms）\n` +
              `${tail(`${r.out}\n${r.err}`)}\n` +
              `**装完复验**：\n${renderLinks(links)}\n${note}`,
            details: { code: r.code, ms: r.ms, cmd: r.cmd, output: tail(`${r.out}\n${r.err}`, 200, 20000), links, bad },
          };
        }

        const note = record(false, '落地器缺失（代码 bug）', null, plan.repoRoot);
        return { op: args.op, ok: false, summary: `✖ ${args.op}：plan.kind=${plan.kind} 没有落地器（代码 bug）\n${note}`, details: {} };
      },
    }),
  );
}
