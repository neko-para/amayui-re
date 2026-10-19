/**
 * tools/lib/x86-effects.mjs —— **从 `.lst` 机械枚举 x86 指令的 effect**（试点工具，不是完整 ISA 语义）
 *
 * ## 它回答什么
 * 给定一个函数 / 一个块入口，按**有限的指令文法**列出每条指令的 effect：
 * 内存读 / 内存写 / 地址计算 / 算术 / 寄存器搬移 / 栈 / 控制流 / 调用。
 * ★ 调用只记 `kind=call` + `target=<实际符号>`；不把 `_atoi` 改名成 `parse-int`。
 *
 * ## 边界（诚实项）
 * * 只认 `.lst` 里**显式写出的操作数** + 一组很窄的隐式规则（push/pop/ret 的栈、call 的目标）。
 *   ⛔ 不建模隐式寄存器（`mul`/`div`/`cpuid` 等标记为 `unknown`）、不展开被调方、不建模异常边。
 * * **不知道的记 `unknown`**（不是"没有 effect"）；`unknown` 必须进残差，不能静默。
 * * 它是**派生视图**：从语料重算；不写任何文件。
 *
 * 口径与"为什么"见 `data/spec/README.md` 与 `pnpm tools spec describe`。
 */
import { functionSpans } from './shape-access.mjs';

const INSN_RE = /^\.text:([0-9A-F]{8})\s+(.*)$/;
const LABEL_RE = /^[A-Za-z_.$?][\w.$?@]*:/;
const SKIP_MN = new Set(['proc', 'endp', 'assume', 'public', 'extrn', 'db', 'dd', 'dw', 'dq', 'align', 'even', 'org']);

const ARITH = new Set([
  'add', 'sub', 'adc', 'sbb', 'and', 'or', 'xor', 'shl', 'shr', 'sar', 'sal',
  'rol', 'ror', 'rcl', 'rcr', 'inc', 'dec', 'neg', 'not', 'cmp', 'test',
  'bt', 'bts', 'btr', 'btc', 'imul', 'mul', 'idiv', 'div',
]);
const X87_ARITH = new Set(['fadd', 'fsub', 'fmul', 'fdiv', 'fsubr', 'fdivr', 'fabs', 'fchs', 'fsqrt']);
const X87_LOAD = new Set(['fld', 'fild', 'fbld', 'fldz', 'fld1', 'fldpi']);
const X87_STORE = new Set(['fst', 'fstp', 'fist', 'fistp', 'fbstp']);
const COND_JMP = /^j(?:a|ae|b|be|c|e|g|ge|l|le|na|nae|nb|nbe|nc|ne|ng|nge|nl|nle|no|np|ns|nz|o|p|pe|po|s|z)$/;

/** 拆分逗号分隔的操作数（方括号内的逗号不算） */
export function splitOperands(text) {
  const out = [];
  let cur = '';
  let depth = 0;
  for (const ch of String(text ?? '')) {
    if (ch === '[') depth += 1;
    if (ch === ']') depth -= 1;
    if (ch === ',' && depth === 0) { out.push(cur.trim()); cur = ''; continue; }
    cur += ch;
  }
  if (cur.trim() !== '') out.push(cur.trim());
  return out;
}

const isMem = (op) => /\[/.test(op) || /^(?:ds|es|ss|cs|fs|gs):/i.test(op);
const isReg = (op) => /^(?:[re]?[abcd]x|[re]?[sd]i|[re]?[sb]p|[abcd][lh]|[sd]il|[sb]pl|[cdefgs]s|st\d*|mm\d+|xmm\d+)$/i.test(op);

/** 从 `.lst` 里取一个函数的指令（不含标签 / 指示符） */
export function functionInstructions(lines, sym) {
  const span = functionSpans(lines).find((s) => s.sym === sym);
  if (!span) return { ok: false, why: `找不到函数 ${sym}` };
  const insns = [];
  for (let i = span.from; i <= span.to; i += 1) {
    const m = INSN_RE.exec(lines[i]);
    if (!m) continue;
    const ea = `0x${Number.parseInt(m[1], 16).toString(16)}`;
    let body = m[2].replace(/;.*$/, '').trim();
    if (body === '' || LABEL_RE.test(body)) continue;
    if (/^[A-Za-z_$?@][\w.$?@]*$/.test(body)) continue;
    if (/^[A-Za-z_$?@][\w.$?@]*=/.test(body)) continue;
    const mn = (body.split(/\s+/)[0] ?? '').toLowerCase();
    if (SKIP_MN.has(mn)) continue;
    const rest = body.slice(mn.length).trim();
    insns.push({ ea, mn, ops: splitOperands(rest), raw: body, i });
  }
  return { ok: true, span, insns };
}

/** 从某个站点开始的基本块：线性推进到无条件跳转 / 返回 / 已知 throw 调用为止（含终止指令） */
export function blockInstructions(insns, startEa) {
  const start = insns.findIndex((x) => Number.parseInt(x.ea, 16) === Number.parseInt(String(startEa), 16));
  if (start < 0) return { ok: false, why: `块入口 ${startEa} 不在函数指令里` };
  const out = [];
  for (let k = start; k < insns.length; k += 1) {
    const x = insns[k];
    out.push(x);
    const terminal =
      x.mn === 'jmp' ||
      x.mn === 'ret' ||
      x.mn === 'retn' ||
      (x.mn === 'call' && /__CxxThrowException/i.test(x.ops[0] ?? ''));
    if (terminal) break;
  }
  return { ok: true, insns: out };
}

/**
 * 一条指令 → effect 列表（保守：不知道就 `unknown`）。
 * effect = `{id, site, op, kind, detail}`；id 在同一站点内确定（kind + 序号）。
 */
export function instructionEffects(insn) {
  const { ea, mn, ops } = insn;
  const out = [];
  const add = (kind, detail = '') => out.push({ site: ea, op: mn, kind, detail });
  const memOps = ops.map((o, i) => ({ o, i, mem: isMem(o) })).filter((x) => x.mem);

  // 调用：kind 只记 `call`，目标保留**实际被调符号**（不另起 parse-int / throw 之类的名字）
  if (mn === 'call') {
    const target = ops[0] ?? '';
    if (isMem(target)) add('mem-read', target);
    add('call', target);
  } else if (mn === 'ret' || mn === 'retn') {
    add('control', 'ret');
    add('stack', 'ret');
  } else if (mn === 'jmp') {
    for (const x of memOps) add('mem-read', x.o);
    add('control', 'jmp');
  } else if (mn.startsWith('j') && COND_JMP.test(mn)) {
    add('control', mn);
  } else if (mn === 'push') {
    add('stack', 'push');
    for (const x of memOps) add('mem-read', x.o);
    if (!ops.some((o) => isMem(o))) add('reg', 'push');
  } else if (mn === 'pop') {
    add('stack', 'pop');
    for (const x of memOps) add('mem-write', x.o);
    if (!ops.some((o) => isMem(o))) add('reg', 'pop');
  } else if (mn === 'enter' || mn === 'leave') {
    add('stack', mn);
  } else if (mn === 'lea') {
    add('addr', ops[1] ?? '');
  } else if (X87_LOAD.has(mn)) {
    for (const x of memOps) add('mem-read', x.o);
    if (mn === 'fild') add('convert', 'int→float');
    if (out.length === 0) add('reg', mn);
  } else if (X87_STORE.has(mn)) {
    for (const x of memOps) add('mem-write', x.o);
    if (mn === 'fist' || mn === 'fistp') add('convert', 'float→int');
    if (out.length === 0) add('reg', mn);
  } else if (X87_ARITH.has(mn)) {
    for (const x of memOps) add('mem-read', x.o);
    add('arith', mn);
  } else if (ARITH.has(mn)) {
    for (const x of memOps) {
      add('mem-read', x.o);
      // cmp/test 不写内存；其余读改写
      if (!['cmp', 'test'].includes(mn) && x.i === 0) add('mem-write', x.o);
    }
    add('arith', mn);
  } else if (mn === 'mov' || mn === 'movzx' || mn === 'movsx' || mn === 'xchg') {
    for (const x of memOps) {
      if (x.i === 0) add('mem-write', x.o);
      else add('mem-read', x.o);
    }
    if (out.length === 0) add('reg', mn);
  } else if (/^(movs|stos|lods|scas|cmps)/.test(mn)) {
    add('mem-read', 'string-op');
    add('mem-write', 'string-op');
    add('unknown', mn);
  } else if (mn === 'nop' || mn === 'int3' || mn === 'hlt') {
    add('internal', mn);
  } else {
    // 其余：显式内存操作数仍可分读/写；其余记 unknown（不猜寄存器语义）
    for (const x of memOps) {
      if (x.i === 0) { add('mem-read', x.o); add('mem-write', x.o); }
      else add('mem-read', x.o);
    }
    add('unknown', mn);
  }

  // 显式寄存器操作数：寄存器搬移/读写（内部 effect；不建模隐式寄存器）
  if (!['call', 'ret', 'retn', 'push', 'pop', 'enter', 'leave'].includes(mn)) {
    ops.forEach((o, i) => {
      if (!isReg(o)) return;
      add('reg', `${i === 0 ? 'write' : 'read'}:${o}`);
      if (i === 0 && (ARITH.has(mn) || mn === 'xchg')) add('reg', `read:${o}`);
    });
  } else if (mn === 'call' || mn === 'ret' || mn === 'retn') {
    add('reg', 'implicit-clobber');
  }

  const counts = new Map();
  return out.map((e) => {
    const n = counts.get(e.kind) ?? 0;
    counts.set(e.kind, n + 1);
    const extra = e.kind === 'call' ? { target: e.detail } : {};
    return { id: `${ea}/${e.kind}/${n}`, ...e, ...extra };
  });
}

/** 一个函数的全部 effect（逐指令） */
export function effectsForFunction(lines, sym) {
  const r = functionInstructions(lines, sym);
  if (!r.ok) return r;
  const effects = r.insns.flatMap((x) => instructionEffects(x));
  return { ok: true, span: r.span, insns: r.insns, effects };
}

/** 一组站点所属基本块的 effect（去重；站点必须在同一个函数里） */
export function effectsForSites(lines, sites, { sym }) {
  const r = functionInstructions(lines, sym);
  if (!r.ok) return r;
  const effects = [];
  const blocks = [];
  for (const ea of sites) {
    const b = blockInstructions(r.insns, ea);
    if (!b.ok) return { ok: false, why: b.why };
    blocks.push({ site: ea, insns: b.insns });
    effects.push(...b.insns.flatMap((x) => instructionEffects(x)).map((e) => ({ ...e, block: ea })));
  }
  const seen = new Set();
  return { ok: true, effects: effects.filter((e) => (seen.has(e.id) ? false : (seen.add(e.id), true))), blocks };
}

/** 一组 EA 站点：自动按所属函数分组，枚举每个站点的基本块 effect */
export function effectsForEaSites(lines, sites) {
  const spans = functionSpans(lines).filter((s) => Number.isFinite(s.ea)).sort((a, b) => a.ea - b.ea);
  const byFn = new Map();
  for (const ea of sites) {
    const n = Number.parseInt(String(ea).replace(/^0x/i, ''), 16);
    let hit = null;
    for (const s of spans) { if (s.ea > n) break; hit = s; }
    if (!hit) return { ok: false, why: `站点 ${ea} 不属于任何函数` };
    const idx = spans.indexOf(hit);
    const next = spans[idx + 1];
    if (next && n >= next.ea) return { ok: false, why: `站点 ${ea} 不在函数体内` };
    if (!byFn.has(hit.sym)) byFn.set(hit.sym, []);
    byFn.get(hit.sym).push(ea);
  }
  const effects = [];
  const blocks = [];
  for (const [sym, eas] of byFn) {
    const r = effectsForSites(lines, eas, { sym });
    if (!r.ok) return r;
    effects.push(...r.effects);
    blocks.push(...r.blocks);
  }
  const seen = new Set();
  return { ok: true, effects: effects.filter((e) => (seen.has(e.id) ? false : (seen.add(e.id), true))), blocks };
}
