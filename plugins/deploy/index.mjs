/**
 * plugins/deploy/index.mjs —— DSH **宿主侧**插件：注册唯一一个工具 `deploy`
 *
 * ## 它是什么
 * 一个**受信任的部署指令**：把"沙箱拒绝的那几步"（对游戏安装目录建硬链接 / 起 headless Chrome /
 * 改完整性标签）做成一个**闭接口工具**。插件代码跑在 DSH 宿主进程里，而 ACL 沙箱只约束
 * **工具执行器**spawn 出来的子进程（`dsh-bash-sandbox` / `dsh-pwsh-sandbox` 那一类）——
 * 于是宿主侧的 `fs` / `child_process` 不受那套令牌限制。设计、安全口径与装法见 `README.md`。
 *
 * ## 三条不许破的线
 * ① **闭接口**：op 只有 `lib/ops.mjs` 的 `OPS` 表里那几个，argv 在那里拼死；**没有**"执行任意命令"这种 op；
 * ② **默认 dry-run**：写类 op 必须显式给 `write: true`，否则只跑工具自己的 dry-run 出计划；
 * ③ **每次留痕**：无论成败都往 `data/privileged-audit.log` 追加一行（append-only 文本）。
 */
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import z from '@deepseek-ai/schemastery';
import { defineTool } from '@deepseek-ai/dsh-tools';

import { appendAudit, DEFAULT_AUDIT } from './lib/audit.mjs';
import { DEFAULT_ALLOWED_ROOTS, OP_NAMES, OPS, auditLine, planOp } from './lib/ops.mjs';
import { probeLink, probeStatus, probeWrite } from './lib/probe.mjs';
import { runRepoCli, runTool, tail } from './lib/run.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
/** 仓库根 = `plugins/deploy/` 上溯两级（本插件是**本仓的**插件，这个相对关系是它的一部分） */
export const REPO_ROOT = path.resolve(HERE, '..', '..');

/** Stable Loader identity. */
export const name = 'amayui-deploy';
/** 只依赖工具注册表（本插件的其余工作直接用 node 的 fs / child_process） */
export const inject = ['tools'];

/**
 * 部署方（人）在 profile 的 loader entry 里配的东西 —— **这一层就是"授权"**：
 * 允许写到哪些根、审计落到哪。模型改不了它。
 */
export const Config = z.object({
  /** 允许写入的根（相对仓库根；要写到工作区外就加绝对路径，例如 `E:\Projects`） */
  allowedRoots: z.array(z.string()).default([...DEFAULT_ALLOWED_ROOTS]),
  /** 审计文件的落点（相对仓库根） */
  auditFile: z.string().default(DEFAULT_AUDIT),
});

const PROBE_OPS = new Set(['status', 'probe-link', 'probe-write']);

const DESCRIPTION = [
  '受信任的部署工具（**宿主侧特权**）：做沙箱拒绝的那几步，并把每次调用记进 append-only 审计。',
  `op 只有这些：${OP_NAMES.join(' / ')}。`,
  '· `status`：报告宿主进程事实（含"宿主起的子进程是什么完整性"）—— 先跑这个确认特权是否成立；',
  '· `probe-link` / `probe-write`：**特权探针**，做完立刻撤销（用来证伪"插件没起作用"）；',
  '· `install-tree`：同步测试安装树（内部就是 `pnpm tools release install`，ALF 硬链接；缺 `write:true` 时只出计划）；',
  '· `bake-ui`：烧 UI 图（headless Chrome，沙箱里起不来；缺 `write:true` 时只出计划）；',
  '· `relabel-medium`：把一棵树的完整性标签改成 Medium（否则从该树起的进程是 Low 完整性，Locale Emulator 起不了窗口）。',
  '安全口径：op 是闭集合、参数受校验（`out` 必须落在 `allowedRoots` 内）、argv 数组不经 shell、写类 op 默认 dry-run。',
].join(' ');

export function apply(ctx, config) {
  const cfg = { allowedRoots: [...(config?.allowedRoots ?? DEFAULT_ALLOWED_ROOTS)], auditFile: config?.auditFile ?? DEFAULT_AUDIT };
  ctx.tools.register(
    defineTool({
      name: 'deploy',
      description: DESCRIPTION,
      parameters: {
        op: { type: 'string', required: true, enum: OP_NAMES, description: '要做的那一件事（闭集合，见工具描述）' },
        write: { type: 'boolean', description: '写类 op 必须显式 true 才真的落盘；缺省/ false = 只出工具自己的 dry-run 计划' },
        out: { type: 'string', description: '落点（install-tree 的树目录 / bake-ui 的产物目录 / relabel-medium 的树）；必须是 allowedRoots 之内的绝对路径' },
        dir: { type: 'string', description: 'probe-link / probe-write 用：与目标同卷的一个已存在目录' },
        baked: { type: 'string', description: 'install-tree 用：AGF 产物目录（`ui-bake build` 的落点）' },
        alf: { type: 'string', enum: ['hardlink', 'copy'], description: 'install-tree 用：ALF 走硬链接（缺省）还是真拷贝' },
        leCmd: { type: 'string', description: 'install-tree 用：Locale Emulator 的 LEProc.exe 绝对路径（顺手写一份 启动游戏-LE.cmd）' },
        leProfile: { type: 'string', description: 'install-tree 用：LE 配置档 GUID' },
        relabelMedium: { type: 'boolean', description: 'install-tree 用：落盘后把整棵树的完整性标签改成 Medium' },
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
        const record = (ok, note, code = null, out = null) => {
          const line = auditLine({ at: new Date().toISOString(), op: args.op, write: args.write === true, out, ok, code, ms: Date.now() - t0, note });
          const res = appendAudit(REPO_ROOT, line, cfg.auditFile);
          return res.ok ? `审计：${path.relative(REPO_ROOT, res.file)}（${res.lines} 行）` : `审计写不动：${res.reason}（这一行是：${line}）`;
        };

        // ① 校验：不绿一个字都不做
        const { problems, plan } = planOp(args.op, args, { repoRoot: REPO_ROOT, allowedRoots: cfg.allowedRoots });
        if (problems.length) {
          const note = record(false, `校验不过：${problems[0]}`, null, args.out ?? null);
          return { op: args.op, ok: false, summary: `✖ ${args.op} 校验不过（**什么都没做**）：\n- ${problems.join('\n- ')}\n${note}`, details: { problems } };
        }

        // ② 探针类：宿主侧直接做（做完自清）
        if (PROBE_OPS.has(args.op)) {
          try {
            const r =
              args.op === 'status' ? probeStatus({ repoRoot: REPO_ROOT, pluginDir: HERE }) : args.op === 'probe-link' ? probeLink({ repoRoot: REPO_ROOT, dir: plan.dir }) : probeWrite({ dir: plan.dir });
            const note = record(r.ok, OPS[args.op].summary, null, plan.dir ?? null);
            return { op: args.op, ok: r.ok, summary: `${r.ok ? '✔' : '✖'} ${args.op}\n${r.summary}\n${note}`, details: r.details };
          } catch (err) {
            const note = record(false, `${err.code ?? ''} ${err.message}`.trim(), err.code ?? null, plan.dir ?? null);
            return {
              op: args.op,
              ok: false,
              summary:
                `✖ ${args.op} 失败：${err.code ?? ''} ${err.message}\n` +
                `　若错误码是 EPERM/EACCES ⇒ **本插件没拿到特权**（宿主侧没跑起来，或路径被别的东西挡住）——这是要如实报告的结论，不要重试报错\n${note}`,
              details: { code: err.code ?? null },
            };
          }
        }

        // ③ 转发类：跑本仓自己的工具（argv 已在 planOp 里拼死）
        if (plan.kind === 'cli') {
          const r = await runRepoCli({ repoRoot: REPO_ROOT, argv: plan.argv, signal: exec?.signal });
          const okLine = r.ok ? `✔ 完成（${r.ms} ms，退出 0）` : `✖ 退出 ${r.code}（${r.ms} ms）`;
          const note = record(r.ok, r.cmd, r.code, plan.out ?? null);
          const head = `${OPS[args.op].summary}\n` + (plan.write ? '' : '（未给 write:true ⇒ 这是**计划**，没有落盘）\n');
          return {
            op: args.op,
            ok: r.ok,
            summary: `${r.ok ? '✔' : '✖'} ${args.op}：${okLine}\n命令 ${r.cmd}\n${head}${tail(`${r.out}\n${r.err}`)}\n${note}`,
            details: { code: r.code, ms: r.ms, cmd: r.cmd, output: tail(`${r.out}\n${r.err}`, 200, 20000) },
          };
        }

        // ④ icacls
        if (plan.kind === 'icacls') {
          const r = await runTool({ cwd: REPO_ROOT, cmd: 'icacls', argv: [plan.out, '/setintegritylevel', 'Medium', '/T', '/C'], signal: exec?.signal });
          const note = record(r.ok, `icacls ${plan.out}`, r.code, plan.out);
          return {
            op: args.op,
            ok: r.ok,
            summary:
              `${r.ok ? '✔' : '✖'} relabel-medium：${plan.out} ⇒ Medium（${r.ms} ms，退出 ${r.code ?? '—'}）\n` +
              `${tail(`${r.out}\n${r.err}`, 20)}\n` +
              '★ 代价：改完这棵子树就落在**受限沙箱可写范围之外**了（见 tools/release.md §3.1）\n' +
              note,
            details: { code: r.code, out: plan.out, ms: r.ms },
          };
        }

        const note = record(false, '落地器缺失（代码 bug）', null, null);
        return { op: args.op, ok: false, summary: `✖ ${args.op}：plan.kind=${plan.kind} 没有落地器（代码 bug）\n${note}`, details: {} };
      },
    }),
  );
}
