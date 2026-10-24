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
 * ## 四条不许破的线（与 `plugins/deploy` 同一套）
 * ① **闭接口**：op 只有 `lib/ops.mjs` 的 `OPS` 表里那几个；pnpm 的 verb 只允许白名单里的；
 * ② **必须落在本仓内**：`repoRoot` 由调用方给出、由 `planOp` 校验（绝对路径 + 有 `package.json` 与
 *    `pnpm-workspace.yaml` + 落在 `allowedRoots` 内）；**不接任意目录、不接"当前目录"**；
 * ③ **写类 op 默认 dry-run**：`install` / `add` / `remove` / `run` 缺 `write:true` 时**一个字都不动**；
 * ④ **每次留痕**：无论成败都往 `data/privileged-audit.log` 追加一行（append-only）。
 *
 * ## `run` 为什么不是"通用 exec"
 * 沙箱里 `vite build`（Windows 上会 `spawn` 一个查真实路径的 helper）与 Electron（`CreateProcess` GUI）
 * 都会 `EPERM` —— 这两步只能在宿主里做。但"宿主里跑任意命令"等于把 ACL 沙箱整体拆掉（confused deputy），
 * 所以 `run` **只认任务名**：`build-renderer` / `smoke` / `electron`，任务名 → argv / cwd
 * 全部写死在 `lib/ops.mjs` 的 `TASKS` 表里，调用方给不了命令、flag、cwd。
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import z from '@deepseek-ai/schemastery';
import { defineTool } from '@deepseek-ai/dsh-tools';

import { appendAudit, DEFAULT_AUDIT } from './lib/audit.mjs';
import { OP_NAMES, TASK_NAMES, auditLine, planOp, realIo } from './lib/ops.mjs';
import { canSymlink, findAmayuiLinks, storePathHint } from './lib/probe.mjs';
import { launchDetached, resolvePnpm, runPnpm, runTask, tail } from './lib/run.mjs';

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
  /** 部署方（人）的总闸：`false` ⇒ `run` op 一律拒（其余 op 不受影响） */
  allowRun: z.boolean().default(true),
});

const DESCRIPTION = [
  '提权安装依赖 + **宿主侧转发固定任务**（**宿主侧特权**）：让 `pnpm` 以**真符号链接**建 workspace 链接，而不是 junction；',
  '并替沙箱跑那几个只能以正常完整性做的步骤（`vite build` / Electron 窗口）。',
  '★ 为什么需要它：Node 的原生类型剥离**拒绝**为 `node_modules` 下的文件剥类型，',
  '  而"跨包用包名导入 `.mts`"必然解析到 `node_modules/@amayui/<包>` ⇒ 那条路只在**真符号链接**下走得通',
  '  （实测：symbolic link 成功 / junction 报 ERR_UNSUPPORTED_NODE_MODULES_TYPE_STRIPPING）。',
  '  沙箱里 `pnpm install` 拿不到 SeCreateSymbolicLinkPrivilege ⇒ 会静默降级成 junction ⇒ 所以搬到宿主里做。',
  '  同理：沙箱里 `vite build`（Windows 上要 spawn 一个查真实路径的 helper ⇒ 实测 spawn EPERM）与 Electron（GUI）',
  '  在 Low 完整性下都起不来 ⇒ 这两步也只能在宿主里做。',
  `op 只有这些：${OP_NAMES.join(' / ')}。`,
  '· `status`：先跑这个 —— 报告**本插件进程能否建真符号链接**（探针自清）、pnpm 入口、以及各消费方的链接形态；',
  '· `check-links`：逐个点名 `node_modules/@amayui/*` 里**不是真符号链接**的那些；',
  '· `install` / `add` / `remove`：跑 pnpm（verb 白名单），装完**自动复验链接形态**；',
  `· \`run\`：转发**一个固定任务**（\`task\` 是闭集合：${TASK_NAMES.join(' / ')}）—— 任务名 → argv / cwd 全部写死，`,
  '  调用方**给不了命令、flag 或 cwd**；`electron` 可选 `installDir`（绝对路径、必须存在）拼成 `--install <dir>`。',
  '安全口径：op 是闭集合 · 没有通用 exec · `repoRoot` 必须是 pnpm workspace 根 · argv 数组不经 shell · 写类 op（含 `run`）默认 dry-run · 每次留痕。',
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

/** 产物的状态（"跑完真的更新了吗"—— 只看退出码会得到假绿） */
function artifactState(repoRoot, rel) {
  if (!rel) return null;
  const file = path.resolve(repoRoot, rel);
  try {
    const st = fs.statSync(file);
    return { file, mtimeMs: st.mtimeMs, size: st.size };
  } catch {
    return null;
  }
}

export function apply(ctx, config) {
  const cfg = {
    allowedRoots: [...(config?.allowedRoots ?? [])],
    auditFile: config?.auditFile ?? DEFAULT_AUDIT,
    allowRun: config?.allowRun !== false,
  };
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
        task: { type: 'string', enum: TASK_NAMES, description: `run 用：固定任务名（闭集合：${TASK_NAMES.join(' / ')}）—— argv / cwd 由任务表写死，给不出命令` },
        installDir: { type: 'string', description: 'run task=electron 用：游戏安装目录（**绝对路径、必须存在**）；拼成 `--install <dir>`，其它任务不接受' },
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
        const { problems, plan } = planOp(args.op, args, { repoRoot: SELF_REPO_ROOT, allowedRoots: cfg.allowedRoots, io: realIo, allowRun: cfg.allowRun });
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

        // ⑤ run：转发一个**固定任务** —— argv / cwd / 超时全部来自 `TASKS` 表
        //    ★ 调用方只给任务名（+ electron 的 installDir），给不出命令、flag、cwd。
        if (plan.kind === 'task') {
          const cmdText = `${plan.runner} ${plan.argv.join(' ')}`;
          const missingLine = plan.missing.length
            ? `★ 缺前置产物：${plan.missing.join(' / ')} —— 先跑 \`op=run task=build-renderer write=true\``
            : '';
          if (!plan.write) {
            const note = record(true, `run ${plan.task}（dry-run，未启动）`, null, plan.repoRoot, cmdText);
            return {
              op: 'run',
              ok: true,
              summary:
                `（未给 write:true ⇒ **这是计划，没有启动任何东西**）\n` +
                `任务：\`${plan.task}\` —— ${plan.summary}\n` +
                `命令：${cmdText}\n　cwd：${plan.cwd}\n` +
                `　模式：${plan.mode === 'detach' ? '起进程就返回（交互式）' : `等它退出（超时 ${Math.round(plan.timeoutMs / 60000)} 分）`}\n` +
                `${missingLine ? `${missingLine}\n` : ''}${note}`,
              details: { plan: { task: plan.task, argv: plan.argv, cwd: plan.cwd, mode: plan.mode }, repoRoot: plan.repoRoot },
            };
          }
          if (plan.missing.length) {
            const note = record(false, `run ${plan.task}：缺前置产物 ${plan.missing[0]}`, null, plan.repoRoot, cmdText);
            return {
              op: 'run',
              ok: false,
              summary: `✖ run ${plan.task}：缺前置产物，**什么都没启动**\n- ${plan.missing.join('\n- ')}\n先跑 \`op=run task=build-renderer write=true\`\n${note}`,
              details: { missing: plan.missing },
            };
          }

          const before = artifactState(plan.repoRoot, plan.artifact);
          const r =
            plan.mode === 'detach'
              ? await launchDetached({
                  runner: plan.runner,
                  repoRoot: plan.repoRoot,
                  app: plan.app,
                  argv: plan.argv,
                  cwd: plan.cwd,
                  logFile: path.resolve(plan.repoRoot, '.tmp', 'host-run-electron.log'),
                })
              : await runTask({
                  runner: plan.runner,
                  repoRoot: plan.repoRoot,
                  app: plan.app,
                  argv: plan.argv,
                  cwd: plan.cwd,
                  timeoutMs: plan.timeoutMs,
                  signal: exec?.signal,
                });
          const eperm = /EPERM|EACCES/.test(String(r.err ?? ''));
          const epermLine = eperm ? '\n★ 宿主侧也 EPERM/EACCES ⇒ **特权没成立**（插件没跑在宿主进程里，或还有别的策略挡着）—— 如实报告，不要重试\n' : '';

          // 交互式：起进程就返回（它不会随回合结束而退出）
          if (plan.mode === 'detach') {
            const note = record(r.ok, `run ${plan.task} pid=${r.pid ?? '—'}`, r.code, plan.repoRoot, r.cmd);
            const appLogAbs = path.resolve(plan.repoRoot, '.tmp', 'emulator-new.log');
            let appTail = '';
            try {
              appTail = tail(fs.readFileSync(appLogAbs, 'utf8'), 6);
            } catch {
              appTail = '';
            }
            return {
              op: 'run',
              ok: r.ok,
              summary:
                `${r.ok ? '✔' : '✖'} run ${plan.task}：${r.ok ? `已启动（pid ${r.pid}）` : `起不来（${r.err ?? `退出 ${r.code}`}）`}\n` +
                `命令：${r.cmd}\n` +
                (r.ok ? '★ 它是**独立进程**：不会随本回合结束而退出，关窗口即结束（宿主完整性 ⇒ 窗口能建出来）\n' : '') +
                `日志：${path.relative(SELF_REPO_ROOT, appLogAbs)}（app 自己写的持久日志）· ${path.relative(SELF_REPO_ROOT, r.logFile)}（stdout/stderr）\n` +
                `${appTail ? `**app 日志尾部**：\n${appTail}\n` : ''}${epermLine}${note}`,
              details: { pid: r.pid, code: r.code, err: r.err, cmd: r.cmd, logFile: r.logFile, appLog: appLogAbs },
            };
          }

          // 等它退出：输出尾部 + 产物是否**真的**更新（只看退出码会得到假绿）
          const now = artifactState(plan.repoRoot, plan.artifact);
          const fresh = plan.artifact ? (before === null ? now !== null : now !== null && now.mtimeMs > before.mtimeMs) : null;
          const artifactLine = plan.artifact
            ? fresh
              ? `产物：${plan.artifact} ✔ 已更新（${now.size} B）`
              : `产物：${plan.artifact} ✖ **没有更新**（退出码看着对也不算成功）`
            : '';
          const ok = r.ok && fresh !== false;
          const note = record(ok, `run ${plan.task}${plan.artifact ? (fresh ? ' 产物已更新' : ' 产物未更新') : ''}`, r.code, plan.repoRoot, r.cmd);
          return {
            op: 'run',
            ok,
            summary:
              `${ok ? '✔' : '✖'} run ${plan.task}：${r.ok ? '退出 0' : `退出 ${r.code ?? '—'}`}（${r.ms} ms）\n` +
              `命令：${r.cmd}\n` +
              `${tail(`${r.out}\n${r.err}`)}\n` +
              `${artifactLine ? `${artifactLine}\n` : ''}${epermLine}${note}`,
            details: { code: r.code, ms: r.ms, cmd: r.cmd, artifact: plan.artifact, fresh, output: tail(`${r.out}\n${r.err}`, 200, 20000) },
          };
        }

        const note = record(false, '落地器缺失（代码 bug）', null, plan.repoRoot);
        return { op: args.op, ok: false, summary: `✖ ${args.op}：plan.kind=${plan.kind} 没有落地器（代码 bug）\n${note}`, details: {} };
      },
    }),
  );
}
