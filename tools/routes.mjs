#!/usr/bin/env node
/**
 * tools/routes.mjs — **CLI**：`data/routes/screens.json`（界面路径登记表）的自描述 / 校验 / 派生 / 回放
 *
 * 这份表登记的是**宿主测试装置**的路径知识：从 boot 出发、用脚本化输入时间线到达各个稳定界面，
 * 判据是"到达哪个脚本"。⛔ **它不是引擎 spec**：引擎语义在 `data/spec/`，本工具一个字节都不读那边。
 *
 * 经派发器：`pnpm tools routes <describe|list|validate|emit|replay> [args…]`
 * 也可独立跑：`node tools/routes.mjs --validate`
 *
 * ★ 分工（`tools/README.md` §0）：本文件只做"参数 → 模型 → 打印 / 落盘"；
 *   数据模型就是这份 CLI 自己（没有第二个 schema 副本）—— `describe()` 是字段语义的唯一真源。
 * ★ 写盘只有一处：`emit --out` 写**派生的**播放器输入脚本（写后回读逐字节复验）。
 *   `screens.json` 本身是**手写真源**，工具只读；`_doc` 指针由本工具定义、由 validate 逐字校验。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import { REPO_ROOT } from './lib/paths.mjs';

export const DOMAIN = {
  id: 'routes',
  title: '从 boot 出发的**界面路径登记表**（宿主输入时间线 → 到达的脚本；真源 `data/routes/screens.json`）',
  data: [
    '`data/routes/screens.json`（**手写真源**：路径 / 输入时间线 / 判据 / 证据）',
    '`apps/emulator-cxx/tests/input/route-*.txt`（**派生、入库**：`emit --out` 的产物）',
  ],
  access: '`screens.json` 只读（手写真源）；`emit --out` 写派生的 `route-*.txt`（写后回读复验）',
  tool: 'tools/routes.mjs',
};

export const OPERATIONS = [
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：字段 / 步骤语法 / 不变量（含谁在守）/ 操作（schema 真源）' },
  { name: 'list', argv: ['--list'], mutates: false, summary: '一行一条：id / from / arrive / title' },
  { name: 'validate', argv: ['--validate'], mutates: false, summary: '跑全部不变量（红 ⇒ 退出码 1）' },
  { name: 'emit', argv: ['--emit'], mutates: true, summary: '把一条 route 的 steps 渲染成播放器输入脚本：缺省 stdout，`--out <file>` 才写盘（写后回读复验）' },
  { name: 'replay', argv: ['--replay'], mutates: false, summary: '打印（不执行）该 route 的完整播放器命令行（含 --anchor-script / --expect-script / --max-steps 600000）' },
];

/** 本工具定义的 `_doc` 指针；`screens.json` 里的值必须逐字等于它。 */
export const DOC_POINTER =
  '由 tools/routes.mjs 拥有：字段语义 / 不变量 / 操作看 `pnpm tools routes describe`（tools/routes.mjs 的自描述）。';

/** 唯一合法的 schema 版本。改它 = 改 schema：工具、守卫、说明书要一起动。 */
export const SCHEMA = 1;

export const DEFAULT_ROUTES = path.join(REPO_ROOT, 'data', 'routes', 'screens.json');
export const DEFAULT_INPUT_DIR = path.join(REPO_ROOT, 'apps', 'emulator-cxx', 'tests', 'input');
/** 播放器可执行文件（相对仓库根；replay 打印的命令也相对仓库根执行）。 */
export const EMULATOR_EXE = 'apps/emulator-cxx/out/emulator-cxx.exe';
/**
 * 本机实测用的安装根。**它是宿主测试事实，不是仓库事实** ⇒ 换机器用 `--install` 或
 * `AMAYUI_INSTALL` 覆盖它；⛔ 不要把这个路径写进 `screens.json`（数据里只写命令形式）。
 */
export const DEFAULT_INSTALL = 'E:\\Games\\Eushully\\天結いキャッスルマイスター';

const INT32_MIN = -2147483648;
const INT32_MAX = 2147483647;
const UINT32_MAX = 4294967295;
const BIN_RE = /^[A-Z0-9_]+\.BIN$/;
const ID_RE = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const DATE_RE = /^\d{4}-\d{2}-\d{2}$/;
const INPUTS = ['move', 'down', 'up', 'key', 'wheel'];

const TOP_KEYS = ['schema', '_doc', 'routes'];
const ROUTE_KEYS = ['id', 'title', 'from', 'anchor', 'steps', 'arrive', 'observe', 'evidence', 'notes'];
const REQUIRED_ROUTE_KEYS = ['id', 'title', 'from', 'steps', 'arrive', 'evidence'];

/** 全部不变量（validate 逐条跑；describe 原样吐出来）。`enforcedBy` 说清"谁在守它"。 */
export const INVARIANTS = [
  {
    id: 'schema-shape',
    text: '顶层只有 schema / _doc / routes；schema = 1；每条 route 只有已知字段，必填字段齐全且类型正确。',
    enforcedBy: '`pnpm tools routes validate` + `tools/test/routes.test.mjs`',
  },
  {
    id: 'doc-pointer',
    text: '`_doc` 逐字等于本工具给出的指针（指向 `pnpm tools routes describe`）—— 不许手写第二份说明书。',
    enforcedBy: '`pnpm tools routes validate` + `tools/test/routes.test.mjs`',
  },
  {
    id: 'id-kebab-unique',
    text: '`id` 是 kebab-case，且全表唯一。',
    enforcedBy: '`pnpm tools routes validate` + `tools/test/routes.test.mjs`',
  },
  {
    id: 'from-resolvable-acyclic',
    text: '`from` ∈ {`boot`} ∪ 表内其它 route 的 `id`；沿 `from` 上溯必须能到 `boot`（不许成环）。',
    enforcedBy: '`pnpm tools routes validate` + `tools/test/routes.test.mjs`',
  },
  {
    id: 'steps-grammar',
    text: '`steps` 每行都能被播放器的输入语法解析：`<毫秒> move x y` / `down N` / `up N` / `key N down|up` / `wheel D`；按钮与键编号 0..31，时间戳 uint32，坐标 int32。',
    enforcedBy: '`pnpm tools routes validate` + `tools/test/routes.test.mjs`',
  },
  {
    id: 'steps-canonical',
    text: '`steps` 每行已是规范形态（`emit` 渲染回同一行）—— 多余空白 / 行内注释不许留在数据里。',
    enforcedBy: '`pnpm tools routes validate` + `tools/test/routes.test.mjs`',
  },
  {
    id: 'steps-monotonic',
    text: '`steps` 的时间戳**非递减**（同一毫秒允许，顺序即书写顺序）。',
    enforcedBy: '`pnpm tools routes validate` + `tools/test/routes.test.mjs`',
  },
  {
    id: 'anchor-arrive-pair',
    text: '`anchor`（可省）/ `arrive`（必填）形如 `{ script: 正整数, bin: "NAME.BIN" }`。',
    enforcedBy: '`pnpm tools routes validate` + `tools/test/routes.test.mjs`',
  },
  {
    id: 'script-bin-bijection',
    text: '全表（`arrive` + `anchor`）里同一个 `script` 只对应一个 `bin`，同一个 `bin` 也只对应一个 `script` —— `script` 是真源，`bin` 只给人看，但不许互相打架。',
    enforcedBy: '`pnpm tools routes validate` + `tools/test/routes.test.mjs`',
  },
  {
    id: 'lineage-prefix',
    text: '子 route 与父 route 的 `anchor` 相同时，父的 `steps` 必须是子的前缀（`steps` 是**从 boot 起的完整时间线**：可以续写，不能改历史）。',
    enforcedBy: '`pnpm tools routes validate` + `tools/test/routes.test.mjs`',
  },
  {
    id: 'evidence-shape',
    text: '`evidence.verifiedAt` 是 `YYYY-MM-DD`，`evidence.command` 非空 —— 证据必须能照着再跑一次。',
    enforcedBy: '`pnpm tools routes validate` + `tools/test/routes.test.mjs`',
  },
];

const isObj = (v) => v !== null && typeof v === 'object' && !Array.isArray(v);
const isInt = (v) => Number.isInteger(v);

/** 相对仓库根的正斜杠路径（打印用；跨平台一致） */
const rel = (p) => path.relative(REPO_ROOT, p).split(path.sep).join('/');

/** 整数 token：默认 int32；`at` 用 0..uint32。 */
function intOf(token, what, min = INT32_MIN, max = INT32_MAX) {
  if (typeof token !== 'string' || !/^-?\d+$/.test(token)) throw new Error(`${what} 必须是整数：${String(token)}`);
  const n = Number(token);
  if (!Number.isSafeInteger(n) || n < min || n > max) throw new Error(`${what} 超出范围 [${min}, ${max}]：${token}`);
  return n;
}

/**
 * 解析**播放器输入脚本的一行**（与 `apps/emulator-cxx` 的 `parse_input_script` 同一套语法）。
 * @returns {{at:number, input:string, args:(number|string)[]}} 结构化的一步（`args` 已按语法校验）
 */
export function parseStep(line) {
  if (typeof line !== 'string') throw new Error('step 必须是字符串（播放器输入脚本的一行）');
  const trimmed = line.trim();
  if (trimmed === '') throw new Error('step 是空行');
  const [atTok, input, ...args] = trimmed.split(/\s+/);
  if (!/^\d+$/.test(atTok)) throw new Error(`时间戳必须是非负整数：${atTok}`);
  const at = intOf(atTok, '时间戳', 0, UINT32_MAX);
  switch (input) {
    case 'move': {
      if (args.length !== 2) throw new Error(`move 需要恰好 x y 两个整数：${trimmed}`);
      return { at, input, args: [intOf(args[0], 'x'), intOf(args[1], 'y')] };
    }
    case 'down':
    case 'up': {
      if (args.length !== 1) throw new Error(`${input} 需要恰好一个按钮编号（0..31）：${trimmed}`);
      return { at, input, args: [intOf(args[0], '按钮编号', 0, 31)] };
    }
    case 'key': {
      if (args.length !== 2) throw new Error(`key 需要 \`key N down|up\`：${trimmed}`);
      const key = intOf(args[0], '键编号', 0, 31);
      if (args[1] !== 'down' && args[1] !== 'up') throw new Error(`key 的状态必须是 down / up：${trimmed}`);
      return { at, input, args: [key, args[1]] };
    }
    case 'wheel': {
      if (args.length !== 1) throw new Error(`wheel 需要恰好一个整数增量：${trimmed}`);
      return { at, input, args: [intOf(args[0], '滚轮增量')] };
    }
    default:
      throw new Error(`未知输入 ${JSON.stringify(input)}（可用：${INPUTS.join(' / ')}）：${trimmed}`);
  }
}

/** 把结构化的一步渲染成规范行（与 `parseStep` 互逆）。 */
export function renderStep(step) {
  return [String(step.at), step.input, ...step.args.map(String)].join(' ');
}

/** 读出真源（JSON 是不透明数据：字段语义只看 `describe()`）。 */
export function loadRoutes(file = DEFAULT_ROUTES) {
  const abs = path.resolve(file);
  let text;
  try {
    text = fs.readFileSync(abs, 'utf8');
  } catch (err) {
    throw new Error(`读不到 ${rel(abs)}：${err.message}`);
  }
  try {
    return JSON.parse(text);
  } catch (err) {
    throw new Error(`${rel(abs)} 不是合法 JSON：${err.message}`);
  }
}

/** 收集全表的 `(script, bin)` 对：`arrive` 与（可省的）`anchor` 都算。 */
export function collectScriptBins(routes) {
  const pairs = [];
  for (const route of Array.isArray(routes) ? routes : []) {
    if (!isObj(route)) continue;
    for (const key of ['arrive', 'anchor']) {
      const pair = route[key];
      if (!isObj(pair) || !isInt(pair.script) || typeof pair.bin !== 'string') continue;
      pairs.push({ where: `${String(route.id)}.${key}`, script: pair.script, bin: pair.bin });
    }
  }
  return pairs;
}

/** `script` ↔ `bin` 的双向唯一性（任务 ⑤：同一个 script 不许对应两个 bin）。 */
export function scriptBinProblems(routes) {
  const problems = [];
  const byScript = new Map();
  const byBin = new Map();
  for (const p of collectScriptBins(routes)) {
    if (!byScript.has(p.script)) byScript.set(p.script, new Set());
    byScript.get(p.script).add(p.bin);
    if (!byBin.has(p.bin)) byBin.set(p.bin, new Set());
    byBin.get(p.bin).add(p.script);
  }
  for (const [script, bins] of byScript) {
    if (bins.size > 1) problems.push(`script ${script} 对应多个 bin：${[...bins].sort().join(' / ')}`);
  }
  for (const [bin, scripts] of byBin) {
    if (scripts.size > 1) problems.push(`bin ${bin} 对应多个 script：${[...scripts].sort((a, b) => a - b).join(' / ')}`);
  }
  return problems;
}

/** 校验 `{script, bin}`；`where` 只用于错误信息。返回问题串（没问题 = null）。 */
function pairProblem(where, pair) {
  if (!isObj(pair)) return `${where} 必须是对象 { script, bin }`;
  if (!isInt(pair.script) || pair.script <= 0) return `${where}.script 必须是正整数（实际 ${JSON.stringify(pair.script)}）`;
  if (typeof pair.bin !== 'string' || !BIN_RE.test(pair.bin)) return `${where}.bin 必须是 "NAME.BIN" 形态（实际 ${JSON.stringify(pair.bin)}）`;
  return null;
}

/** 校验 `evidence`：日期 + 可再跑的命令。 */
function evidenceProblem(routeId, evidence) {
  if (!isObj(evidence)) return `${routeId}.evidence 必须是对象 { verifiedAt, command }`;
  if (typeof evidence.verifiedAt !== 'string' || !DATE_RE.test(evidence.verifiedAt)) {
    return `${routeId}.evidence.verifiedAt 必须是 YYYY-MM-DD（实际 ${JSON.stringify(evidence.verifiedAt)}）`;
  }
  const d = new Date(`${evidence.verifiedAt}T00:00:00Z`);
  if (Number.isNaN(d.getTime()) || d.toISOString().slice(0, 10) !== evidence.verifiedAt) {
    return `${routeId}.evidence.verifiedAt 不是真实日期：${evidence.verifiedAt}`;
  }
  if (typeof evidence.command !== 'string' || evidence.command.trim() === '') {
    return `${routeId}.evidence.command 必须非空（要能照着再跑一次）`;
  }
  return null;
}

const sameAnchor = (a, b) => {
  const x = isObj(a) ? `${a.script}\u0000${a.bin}` : null;
  const y = isObj(b) ? `${b.script}\u0000${b.bin}` : null;
  return x === y;
};

/**
 * 跑**全部不变量**。返回 `{inv, message}[]`：空数组 = 绿；每条问题都带不变量 id。
 * ★ 它**不抛异常**（坏数据也是"问题"，不是崩溃）—— 只有 JSON 本身读不出来才在 load 时抛。
 */
export function validateRoutes(data) {
  const problems = [];
  const add = (inv, message) => problems.push({ inv, message });

  // ── schema-shape / doc-pointer
  if (!isObj(data)) {
    add('schema-shape', '顶层必须是对象 { schema, _doc, routes }');
    return problems;
  }
  for (const key of Object.keys(data)) {
    if (!TOP_KEYS.includes(key)) add('schema-shape', `未知顶层字段 ${JSON.stringify(key)}（允许：${TOP_KEYS.join(' / ')}）`);
  }
  if (data.schema !== SCHEMA) add('schema-shape', `schema 必须是 ${SCHEMA}（实际 ${JSON.stringify(data.schema)}）`);
  if (data._doc !== DOC_POINTER) {
    add('doc-pointer', `_doc 必须逐字等于工具给出的指针（期望 ${JSON.stringify(DOC_POINTER)}）`);
  }
  if (!Array.isArray(data.routes)) {
    add('schema-shape', 'routes 必须是数组');
    return problems;
  }

  const routes = data.routes;
  const byId = new Map();
  const parsedSteps = new Map();

  for (let index = 0; index < routes.length; index += 1) {
    const route = routes[index];
    const where = isObj(route) && typeof route.id === 'string' ? `route ${route.id}` : `routes[${index}]`;

    if (!isObj(route)) {
      add('schema-shape', `${where} 必须是对象`);
      continue;
    }
    for (const key of Object.keys(route)) {
      if (!ROUTE_KEYS.includes(key)) add('schema-shape', `${where} 有未知字段 ${JSON.stringify(key)}（允许：${ROUTE_KEYS.join(' / ')}）`);
    }
    for (const key of REQUIRED_ROUTE_KEYS) {
      if (!(key in route)) add('schema-shape', `${where} 缺必填字段 ${key}`);
    }

    // ── id-kebab-unique
    if (typeof route.id !== 'string' || !ID_RE.test(route.id)) {
      add('id-kebab-unique', `${where}.id 必须是 kebab-case（小写字母 / 数字 / 单连字符）`);
    } else if (byId.has(route.id)) {
      add('id-kebab-unique', `id 重复：${route.id}`);
    } else {
      byId.set(route.id, route);
    }

    if (typeof route.title !== 'string' || route.title.trim() === '') add('schema-shape', `${where}.title 必须是非空字符串`);
    if (typeof route.from !== 'string' || route.from === '') add('schema-shape', `${where}.from 必须是非空字符串`);

    // ── anchor / arrive
    if ('anchor' in route && route.anchor !== null) {
      const p = pairProblem(`${where}.anchor`, route.anchor);
      if (p) add('anchor-arrive-pair', p);
    }
    const arriveProblem = pairProblem(`${where}.arrive`, route.arrive);
    if (arriveProblem) add('anchor-arrive-pair', arriveProblem);

    // ── steps：语法 / 规范形态 / 非递减
    if (!Array.isArray(route.steps)) {
      add('schema-shape', `${where}.steps 必须是数组`);
    } else {
      const parsed = [];
      let ok = true;
      for (let i = 0; i < route.steps.length; i += 1) {
        try {
          const step = parseStep(route.steps[i]);
          parsed.push(step);
          const canonical = renderStep(step);
          if (route.steps[i] !== canonical) {
            add('steps-canonical', `${where}.steps[${i}] 不是规范形态：应为 ${JSON.stringify(canonical)}，实际 ${JSON.stringify(route.steps[i])}`);
          }
        } catch (err) {
          ok = false;
          add('steps-grammar', `${where}.steps[${i}]：${err.message}`);
        }
      }
      if (ok) parsedSteps.set(route.id, parsed.map(renderStep));
      for (let i = 1; i < parsed.length; i += 1) {
        if (parsed[i].at < parsed[i - 1].at) {
          add('steps-monotonic', `${where}.steps[${i}] 的时间戳递减：${parsed[i - 1].at} → ${parsed[i].at}`);
        }
      }
    }

    // ── observe / evidence / notes
    if ('observe' in route && route.observe !== null) {
      if (!isObj(route.observe) || typeof route.observe.note !== 'string' || route.observe.note.trim() === '') {
        add('schema-shape', `${where}.observe 必须是 { note: 非空字符串 }（人看的画面描述，不作判据）`);
      }
    }
    const ev = evidenceProblem(String(route.id), route.evidence);
    if (ev) add('evidence-shape', ev);
    if ('notes' in route && typeof route.notes !== 'string') add('schema-shape', `${where}.notes 必须是字符串`);
  }

  // ── from-resolvable-acyclic
  for (const [id, route] of byId) {
    if (route.from === 'boot') continue;
    if (!byId.has(route.from)) {
      add('from-resolvable-acyclic', `route ${id} 的 from=${JSON.stringify(route.from)} 既不是 boot、也不在表内`);
      continue;
    }
    const seen = [id];
    let cur = route;
    while (isObj(cur) && typeof cur.from === 'string' && cur.from !== 'boot') {
      if (seen.includes(cur.from)) {
        add('from-resolvable-acyclic', `from 链成环：${[...seen, cur.from].join(' → ')}`);
        break;
      }
      seen.push(cur.from);
      cur = byId.get(cur.from);
    }
  }

  // ── script-bin-bijection
  for (const message of scriptBinProblems(routes)) add('script-bin-bijection', message);

  // ── lineage-prefix（只在同一时间基上比较：父子 anchor 相同）
  for (const [id, route] of byId) {
    if (route.from === 'boot') continue;
    const parent = byId.get(route.from);
    if (!parent || !sameAnchor(route.anchor, parent.anchor)) continue;
    const childSteps = parsedSteps.get(id);
    const parentSteps = parsedSteps.get(parent.id);
    if (!childSteps || !parentSteps) continue;
    for (let i = 0; i < parentSteps.length; i += 1) {
      if (childSteps[i] !== parentSteps[i]) {
        add('lineage-prefix', `route ${id} 的 steps 与父 ${parent.id} 的分叉：第 ${i + 1} 条应为 ${JSON.stringify(parentSteps[i])}，实际 ${JSON.stringify(childSteps[i] ?? null)}`);
        break;
      }
    }
  }

  return problems;
}

/** 输入脚本的文件名（与入库的派生件同名）。 */
export const inputFileName = (id) => `route-${id}.txt`;

/**
 * 把一条 route 的 `steps` 渲染成**播放器输入脚本**。
 * ★ 输出是**逐字节确定**的：入库的 `route-*.txt` 必须与它完全相同（守卫比对）。
 * ★ 空时间线 ⇒ 只剩注释行（播放器解析为 0 条事件，不报错）。
 */
export function emitScript(route) {
  if (!isObj(route)) throw new Error('route 必须是对象');
  if (typeof route.id !== 'string' || !Array.isArray(route.steps)) throw new Error('route 缺 id / steps');
  const lines = [
    `# route: ${route.id}（${String(route.title ?? '')}）`,
    '# 宿主测试装置：脚本化输入时间线（⛔ 不是引擎 spec；引擎语义在 data/spec）。',
    `# from: ${String(route.from ?? '')}`,
  ];
  if (isObj(route.anchor)) {
    lines.push(`# anchor: ${route.anchor.script} ${route.anchor.bin}（时间线 0 点 = 首次进入该脚本）`);
  }
  if (isObj(route.arrive)) {
    lines.push(`# arrive: ${route.arrive.script} ${route.arrive.bin}（播放器 --expect-script 的判据）`);
  }
  lines.push(`# 由 \`pnpm tools routes emit --id ${route.id}\` 从 data/routes/screens.json 生成；⛔ 别手改。`);
  for (const line of route.steps) lines.push(renderStep(parseStep(line)));
  return `${lines.join('\n')}\n`;
}

/** 缺省安装根：`--install` > `AMAYUI_INSTALL` > 本机实测默认。 */
export function defaultInstall(override) {
  if (typeof override === 'string' && override !== '') return override;
  const env = process.env.AMAYUI_INSTALL;
  return typeof env === 'string' && env !== '' ? env : DEFAULT_INSTALL;
}

/** 打印用引号：一律加双引号（安装路径含日文/空格都不能被 shell 拆开）。 */
const quote = (s) => `"${String(s).replace(/"/g, '\\"')}"`;

/**
 * 该 route 对应的**完整播放器命令行**（相对仓库根）。`replay` 只打印它，不执行。
 * ★ `--max-steps 600000` 是**上界**，不是断言：判据仍是 `--expect-script`。
 */
export function replayCommand(route, options = {}) {
  if (!isObj(route)) throw new Error('route 必须是对象');
  if (!isObj(route.arrive)) throw new Error('route 缺 arrive');
  const install = defaultInstall(options.install);
  const parts = [
    EMULATOR_EXE,
    '--install',
    quote(install),
    '--skip-logo',
    '--play-headless',
    '--input-script',
    `apps/emulator-cxx/tests/input/${inputFileName(route.id)}`,
  ];
  if (isObj(route.anchor)) parts.push('--anchor-script', String(route.anchor.script));
  parts.push('--expect-script', String(route.arrive.script), '--tick-ms', '16', '--max-steps', '600000');
  return parts.join(' ');
}

// ───────────────────────────────────────────────── 自描述

export function describe() {
  return {
    file: 'data/routes/screens.json',
    owner: 'tools/routes.mjs',
    scope: '宿主测试装置的路径知识：从 boot 出发、用脚本化输入时间线到达各稳定界面（⛔ 不是引擎 spec；引擎语义在 data/spec）',
    schema: SCHEMA,
    fields: [
      { name: 'schema', type: '1', meaning: 'schema 版本；唯一合法值 1。改它 = 改 schema（工具 / 守卫 / 说明书一起动）。' },
      { name: '_doc', type: 'string', meaning: '本工具定义的**自描述指针**（逐字校验）；⛔ 不许手写第二份 schema 说明。' },
      { name: 'routes[]', type: 'array', meaning: '路径条目；每条的判据是"到达哪个脚本"。' },
      { name: 'routes[].id', type: 'kebab-case string', meaning: '唯一 id；派生脚本文件名 = `route-<id>.txt`。' },
      { name: 'routes[].title', type: 'string', meaning: '人看的名字（不进判据）。' },
      { name: 'routes[].from', type: 'boot | route id', meaning: '**血缘**（这条路径从哪个界面出发）。⛔ 不参与执行：`steps` 永远是从 boot 起的完整时间线。' },
      { name: 'routes[].anchor', type: '{script, bin}?', meaning: '可省。时间线 0 点 = **首次进入该脚本**；省了 = 播放开始。带它才能让时间线与 boot 快慢解耦。' },
      { name: 'routes[].steps', type: 'string[]', meaning: '播放器输入脚本的行（语法见 stepGrammar）；**从 boot 起的完整时间线**，时间戳非递减。' },
      { name: 'routes[].arrive', type: '{script, bin}', meaning: '判据：`script` 是唯一真值（喂给 `--expect-script`）；`bin` 只给人看。' },
      { name: 'routes[].observe.note', type: 'string', meaning: '人看的画面描述（⛔ 不作判据：这里没有像素 oracle）。' },
      { name: 'routes[].evidence', type: '{verifiedAt, command}', meaning: '可再校验的证据：实际跑过的命令 + 日期（`YYYY-MM-DD`）。' },
      { name: 'routes[].notes', type: 'string?', meaning: '坑与待核（例如"界面还没画出像素、坐标是反推的"）。' },
    ],
    stepGrammar: [
      { syntax: '<毫秒> move x y', meaning: '把光标搬到 1280×720 虚拟屏坐标 (x,y)。★ 界面只在**坐标变化**时跑命中查找 ⇒ 先停在框外再搬进框内。' },
      { syntax: '<毫秒> down N / <毫秒> up N', meaning: '按钮位 N（0..31，左键 = 0）。★ 点击在**抬起**时生效 ⇒ down / up 至少隔一个 tick。' },
      { syntax: '<毫秒> key N down|up', meaning: '键盘位 N（0..31，引擎输入位号，不是 VK）。' },
      { syntax: '<毫秒> wheel D', meaning: '滚轮增量 D（有符号）。' },
      { syntax: '# …', meaning: '注释；空行忽略。同一毫秒的事件按书写顺序应用（播放器稳定排序）。' },
    ],
    invariants: INVARIANTS,
    operations: OPERATIONS.map((o) => ({ name: o.name, mutates: o.mutates, summary: o.summary })),
    writePath: '唯一写盘是 `emit --out <file>`（写派生的播放器输入脚本，写后回读逐字节复验）；`screens.json` 本身是手写真源，工具只读。',
    derived: '`apps/emulator-cxx/tests/input/route-<id>.txt`（入库；守卫 `tools/test/routes.test.mjs` 逐字节比对 emit 的输出）。',
    replay: {
      exe: EMULATOR_EXE,
      installDefault: DEFAULT_INSTALL,
      installEnv: 'AMAYUI_INSTALL',
      installFlag: '--install <目录>',
      note: '命令相对仓库根执行；`--max-steps 600000` 是上界，判据仍是 `--expect-script`。',
    },
  };
}

export function describeText() {
  const d = describe();
  const L = [];
  L.push(`routes — ${d.file}（${d.owner}）`);
  L.push('');
  L.push(`范围　${d.scope}`);
  L.push(`写盘　${d.writePath}`);
  L.push(`派生　${d.derived}`);
  L.push('');
  L.push('字段');
  for (const f of d.fields) L.push(`  ${f.name.padEnd(28)} ${f.type.padEnd(22)} ${f.meaning}`);
  L.push('');
  L.push('步骤语法（与 apps/emulator-cxx 的 parse_input_script 同一套）');
  for (const g of d.stepGrammar) L.push(`  ${g.syntax.padEnd(32)} ${g.meaning}`);
  L.push('');
  L.push('不变量（`pnpm tools routes validate` 逐条跑；红了退出码 1）');
  for (const inv of d.invariants) L.push(`  ${inv.id.padEnd(26)} ${inv.text}`);
  L.push('');
  L.push('操作');
  for (const o of d.operations) L.push(`  ${o.name.padEnd(10)}${o.mutates ? '(会写)' : '      '} ${o.summary}`);
  L.push('');
  L.push(`回放　${d.replay.exe}（相对仓库根）；--install 缺省 ${d.replay.installDefault}（可用 ${d.replay.installEnv} 或 --install 覆盖）`);
  L.push(`　　　${d.replay.note}`);
  return `${L.join('\n')}\n`;
}

// ───────────────────────────────────────────────── CLI

const HELP = `tools/routes.mjs — 界面路径登记表（data/routes/screens.json）的自描述 / 校验 / 派生 / 回放

  node tools/routes.mjs --describe [--json]
  node tools/routes.mjs --list
  node tools/routes.mjs --validate
  node tools/routes.mjs --emit --id <id> [--out <file>]
  node tools/routes.mjs --replay --id <id> [--install <目录>]
  pnpm tools routes <describe|list|validate|emit|replay> [args…]

★ 这份表是**宿主测试装置**的路径知识，⛔ 不是引擎 spec（引擎语义在 data/spec）。
★ 唯一写盘：emit --out 写派生的播放器输入脚本（写后回读复验）；screens.json 是手写真源，工具只读。
`;

function parseArgs(argv) {
  const out = { op: null, id: null, out: null, install: null, json: false, routes: DEFAULT_ROUTES };
  const OPS = OPERATIONS.map((o) => o.name);
  const takesValue = new Set(['id', 'out', 'install', 'routes']);
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--help' || a === '-h') out.op = 'help';
    else if (a === '--json') out.json = true;
    else if (a.startsWith('--') && OPS.includes(a.slice(2))) out.op = a.slice(2);
    else if (a.startsWith('--')) {
      const key = a.slice(2);
      if (!takesValue.has(key)) throw new Error(`未知参数：${a}（--help 看用法）`);
      const value = argv[i + 1];
      if (value === undefined) throw new Error(`${a} 需要一个值`);
      i += 1;
      out[key] = value;
    } else if (out.op === null && OPS.includes(a)) out.op = a;
    else throw new Error(`多余的参数：${a}（--help 看用法）`);
  }
  return out;
}

function routeById(data, id) {
  if (typeof id !== 'string' || id === '') throw new Error('需要 --id <route id>（list 看全部）');
  const routes = Array.isArray(data?.routes) ? data.routes : [];
  const route = routes.find((r) => isObj(r) && r.id === id);
  if (!route) throw new Error(`没有 id 为 ${JSON.stringify(id)} 的 route（list 看全部）`);
  return route;
}

function printValidation(data, file = DEFAULT_ROUTES) {
  const problems = validateRoutes(data);
  const routes = Array.isArray(data?.routes) ? data.routes : [];
  process.stdout.write(`routes validate — ${rel(path.resolve(file))}（${routes.length} 条 route）\n`);
  for (const inv of INVARIANTS) {
    const bad = problems.filter((p) => p.inv === inv.id);
    process.stdout.write(`${bad.length ? 'FAIL' : 'ok  '} ${inv.id}\n`);
  }
  for (const p of problems) process.stdout.write(`  [${p.inv}] ${p.message}\n`);
  if (problems.length === 0) process.stdout.write(`\n全部 ${INVARIANTS.length} 条不变量通过。\n`);
  else process.stdout.write(`\n${problems.length} 个问题（红了先修数据，别绕过）。\n`);
  return problems;
}

function printList(data) {
  const routes = Array.isArray(data?.routes) ? data.routes : [];
  const rows = routes.map((r) => ({
    id: String(r?.id ?? '?'),
    from: String(r?.from ?? '?'),
    arrive: isObj(r?.arrive) ? `${r.arrive.script} ${r.arrive.bin ?? '?'}` : '?',
    title: String(r?.title ?? ''),
  }));
  const width = (key) => Math.max(...rows.map((r) => r[key].length), 0);
  const w = { id: width('id'), from: width('from'), arrive: width('arrive') };
  for (const r of rows) {
    process.stdout.write(`${r.id.padEnd(w.id)}  ${r.from.padEnd(w.from)}  ${r.arrive.padEnd(w.arrive)}  ${r.title}\n`);
  }
}

function run(argv) {
  const args = parseArgs(argv);
  if (!args.op) {
    process.stdout.write(HELP);
    return 2;
  }
  if (args.op === 'help') {
    process.stdout.write(HELP);
    return 0;
  }
  if (args.op === 'describe') {
    process.stdout.write(args.json ? `${JSON.stringify(describe(), null, 2)}\n` : describeText());
    return 0;
  }

  const data = loadRoutes(args.routes);
  if (args.op === 'list') {
    printList(data);
    return 0;
  }
  if (args.op === 'validate') return printValidation(data, args.routes).length === 0 ? 0 : 1;

  // emit / replay：先校验整表 —— 坏数据不许被派生/回放成"看起来能用"的命令
  const problems = validateRoutes(data);
  if (problems.length > 0) {
    process.stdout.write(`routes ${args.op}：数据没通过校验，先跑 \`pnpm tools routes validate\`。\n`);
    for (const p of problems) process.stdout.write(`  [${p.inv}] ${p.message}\n`);
    return 1;
  }
  const route = routeById(data, args.id);

  if (args.op === 'emit') {
    const text = emitScript(route);
    if (typeof args.out !== 'string' || args.out === '') {
      process.stdout.write(text);
      return 0;
    }
    const abs = path.resolve(args.out);
    fs.mkdirSync(path.dirname(abs), { recursive: true });
    fs.writeFileSync(abs, text, 'utf8');
    const back = fs.readFileSync(abs, 'utf8');
    if (back !== text) throw new Error(`回读不一致：${rel(abs)}（写盘坏了，别信这份产物）`);
    process.stdout.write(`已写 ${rel(abs)}（${Buffer.byteLength(text, 'utf8')} 字节；回读逐字节一致）\n`);
    return 0;
  }

  if (args.op === 'replay') {
    process.stdout.write(`${replayCommand(route, { install: args.install })}\n`);
    return 0;
  }

  throw new Error(`未知动作：${args.op}`);
}

export function main(argv = process.argv.slice(2)) {
  try {
    return run(argv);
  } catch (err) {
    process.stderr.write(`ERROR: ${err.message}\n`);
    return 2;
  }
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  process.exitCode = main();
}
