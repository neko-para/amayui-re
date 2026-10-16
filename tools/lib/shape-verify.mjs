/**
 * tools/lib/shape-verify.mjs —— **独立复核器**：按 witness EA 回语料**重解**一条指令
 *
 * ## 与 `shape-access.mjs` 的关系（硬口径）
 * * **两个实现**：本文件不 import 助手侧的任何解析 / 折叠代码；
 * * **方向相反**：助手 = 整个函数前向；这里是**从 EA 向后**的局部切片 + 一张**闭集合惯用法表**；
 * * 表外形态一律 `unverifiable`（**不许猜**）。
 *
 * ## 三档话 + 一档诚实
 * `verified` / `drifted` / `unverifiable`；并且**逐字段**区分"证明"与"推断" ——
 * 只有**证明**出的不一致才叫 drifted，带局部推断的不一致只能 unverifiable。
 */
import fs from 'node:fs';
import { DEFAULT_OUT_DIR } from './paths.mjs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

/** 语料只读：解压产物在 `corpus/disasm/files/`（gitignore）；基线的 `.lst`/`.c` 一对 */
const FILES = DEFAULT_OUT_DIR;
const pick = (ext) => {
  if (!fs.existsSync(FILES)) return null;
  const l = fs.readdirSync(FILES).filter((f) => f.endsWith(ext)).sort();
  return l.length ? path.join(FILES, l.find((f) => f.includes('AGE.EXE')) ?? l[0]) : null;
};
const LST = pick('.lst');
const CFILE = pick('.c');

/**
 * ★★ **折叠惯用法表**（复核器的"能力边界"就写在这里，是**闭集合**）。
 * 每一条都对应语料里真实出现的形态；表外的编码形态 ⇒ `unverifiable`（这是**有意的**：
 * 复核器宁可说"我复核不了"，也不拿一个不认识的形态硬套出一个数）。
 */
export const IDIOMS = {
  'mov r,[b+D]': '把 `*(对象根+D)` 读进寄存器（`b` 必须**证得出**是对象根）⇒ 仿射量 1·sym',
  'mov r,r2': '复制（沿 `r2` 的链继续向后切）',
  'add r,K': '＋常量', 'sub r,K': '－常量',
  'add r,r2': '＋另一个仿射量', 'sub r,r2': '－另一个仿射量',
  'shl r,N': '×2^N（`15·x` 就是 `shl 4` + `sub` 两件套拼出来的）',
  'sal r,N': '同 `shl`',
  'imul r,K': '×常量',
  'lea r,[b+A]': '取址（`b` 是对象根时 = 常量 `A`）',
  'xor r,r': '清零（⇒ 常量 0）',
  'inc r': '＋1', 'dec r': '－1',
};

/** 只有擦除语法的寄存器宽度表（本文件自己的一份，写法与助手侧不同：按**后缀**判） */
function widthOfReg(name) {
  const n = name.toLowerCase();
  if (/^r\d+d?$/.test(n) || /^e(ax|bx|cx|dx|si|di|bp|sp)$/.test(n)) return 4;
  if (/^(ax|bx|cx|dx|si|di|bp|sp)$/.test(n)) return 2;
  if (/^(al|bl|cl|dl|ah|bh|ch|dh)$/.test(n)) return 1;
  if (/^xmm\d+$/.test(n)) return 16;
  return null;
}
const isReg = (s) => widthOfReg(s) !== null;

const P = /^([A-Za-z_.][\w.]*):([0-9A-Fa-f]{8})(?:\s+(\S+)(?:\s+(.*))?)?$/;
const PROC = /^\S+\s+(\S+)\s+proc near/;

/** 语料（本文件自己的读法：整表 + EA → 行号索引） */
export function loadRaw() {
  const lines = fs.readFileSync(LST, 'utf8').split('\n');
  const byEa = new Map();
  const fnStart = []; // 每个函数起点行号（升序），用来"向前找所属函数"
  for (let i = 0; i < lines.length; i += 1) {
    const m = P.exec(lines[i]);
    if (m) {
      const ea = Number.parseInt(m[2], 16);
      // ★★ 一个 EA 上**有多行**：`.text:EA` 空行 / `.text:EA loc_XXXX:` 标签 / `.text:EA 指令`
      //    / `.text:EA ;   } // starts at 402045`（**纯注释行**，第一版把它当成了指令 ⇒
      //    248 条"惯用法表之外：undefined"，而那**全是假 unverifiable**）。
      //    ⇒ 判据：第三列必须**就是助记符形态**（小写字母 + 数字）。
      const isInsn = !!m[3] && /^[a-z][a-z0-9]*$/.test(m[3]);
      const cur = byEa.get(ea);
      if (cur === undefined || isInsn) byEa.set(ea, i);
    }
    if (PROC.test(lines[i])) fnStart.push(i);
  }
  return { lines, byEa, fnStart };
}

/** 这个 EA 属于哪个函数（向前找最近一个 `proc near`）—— 复核器自己算，不用助手的 `enclosingFunction` */
export function enclosing(raw, idx) {
  let lo = 0;
  let hi = raw.fnStart.length - 1;
  let ans = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (raw.fnStart[mid] <= idx) { ans = raw.fnStart[mid]; lo = mid + 1; } else hi = mid - 1;
  }
  if (ans < 0) return null;
  const m = PROC.exec(raw.lines[ans]);
  return { from: ans, sym: m[1] };
}

/** 对象根寄存器（本文件自己的 `.c` 读法） */
let ROOTS = null;
function roots() {
  if (ROOTS) return ROOTS;
  const c = fs.readFileSync(CFILE, 'utf8');
  ROOTS = new Map();
  for (const m of c.matchAll(/__(\w+)\s+(\S+?)\s*\(([^)]*)/g)) {
    const [, conv, sym, p0] = m;
    if (ROOTS.has(sym)) continue;
    if (conv === 'thiscall' || conv === 'fastcall') ROOTS.set(sym, 'ecx');
    else if (conv === 'usercall' || conv === 'userpurge') {
      const g = /@<([a-z0-9]+)>/.exec(sym) ?? /@<([a-z0-9]+)>/.exec(p0 ?? '');
      if (g) ROOTS.set(sym, g[1]);
    }
  }
  return ROOTS;
}

/** 一条指令 → `{mn, ops, idx}`（复核器自己的切分：括号配对 + 逗号分层） */
function insnAt(raw, idx) {
  const m = P.exec(raw.lines[idx]);
  if (!m || !m[3]) return null;
  const mn = m[3];
  const rest = m[4] ?? '';
  const ops = [];
  let depth = 0;
  let cur = '';
  for (const ch of rest) {
    if (ch === '[') depth += 1;
    else if (ch === ']') depth -= 1;
    if (ch === ',' && depth === 0) { ops.push(cur.trim()); cur = ''; } else cur += ch;
  }
  if (cur.trim()) ops.push(cur.trim());
  return { mn, ops, idx, line: idx + 1, text: raw.lines[idx].trimEnd() };
}

/** `[b+Dh]` → `{base, disp}`（本文件自己的读法；只认这两种成分，别的一律 null） */
function simpleMem(op) {
  const m = /^(?:[a-z]{2}:\s*)?\[([^\]]+)\]$/.exec((op ?? '').replace(/^(byte|word|dword|qword|xmmword|tbyte)\s+ptr\s+/i, '').trim());
  if (!m) return null;
  const parts = m[1].split('+').map((x) => x.trim());
  if (parts.length !== 2) return null;
  if (!isReg(parts[0])) return null;
  const d = /^([0-9A-Fa-f]{1,8})h$/.exec(parts[1]) ?? /^(\d+)$/.exec(parts[1]);
  if (!d) return null;
  return { base: parts[0].toLowerCase(), disp: Number.parseInt(d[1], 16) };
}

/** `[b+R*s+Dh]` / `[b+R*s]` → `{base, idxReg, scale, disp}` */
function indexedMem(op) {
  const m = /^(?:[a-z]{2}:\s*)?\[([^\]]+)\]$/.exec((op ?? '').replace(/^(byte|word|dword|qword|xmmword|tbyte)\s+ptr\s+/i, '').trim());
  if (!m) return null;
  const parts = m[1].split('+').map((x) => x.trim());
  const out = { base: null, idxReg: null, scale: 0, disp: 0 };
  for (const p of parts) {
    let q = /^([a-z]{2,4})\*(\d+)$/i.exec(p);
    if (q) { if (out.idxReg) return null; out.idxReg = q[1].toLowerCase(); out.scale = Number(q[2]); continue; }
    if (isReg(p)) {
      if (out.base === null) { out.base = p.toLowerCase(); continue; }
      if (out.idxReg === null) { out.idxReg = p.toLowerCase(); out.scale = 1; continue; }
      return null;
    }
    q = /^([0-9A-Fa-f]{1,8})h$/.exec(p) ?? /^(\d+)$/.exec(p);
    if (q) { out.disp += Number.parseInt(q[1], 16); continue; }
    return null;
  }
  return out;
}

/** 位移前缀形态 `ds:8[ecx*8]`（复核器也要认，否则会把 `+8` 丢掉） */
function prefixDisp(op) {
  const m = /^(?:[a-z]{2}:)?([0-9A-Fa-f]{1,8}h|\d+)\s*\[/i.exec((op ?? '').trim());
  return m ? Number.parseInt(m[1].replace(/h$/i, ''), 16) : 0;
}

const WRITES_FIRST = new Set(['mov', 'movzx', 'movsx', 'lea', 'add', 'sub', 'adc', 'sbb', 'and', 'or', 'xor', 'shl', 'shr', 'sar', 'sal', 'rol', 'ror', 'imul', 'pop', 'inc', 'dec', 'neg', 'not']);
/** 被调用方保存的寄存器（跨 `call` 不变）—— 与 caller-saved 的规则**必须分开** */
const CALLEE_SAVED = new Set(['esi', 'edi', 'ebx', 'ebp']);

/** 这一行之后**紧跟**函数收尾 ⇒ 它属于一条**退出路径**（`pop esi` / `mov esp,ebp` / `retn` 三件套） */
function exitsHere(raw, i) {
  // ★ 窗口给到 12 条：实测 `sub_40ED40` 的 `pop esi`（`0x40EF4F`）与 `mov esp,ebp`（`0x40EF5B`）
  //   之间还隔着异常展开的十来条 ⇒ 窗口取 4 会漏（表现是 esi 的来源被认成那条 pop）。
  for (let j = i + 1; j <= i + 12; j += 1) {
    const m = P.exec(raw.lines[j] ?? '');
    const body = `${m?.[3] ?? ''} ${m?.[4] ?? ''}`.trim();
    if (/^mov\s+esp,\s*ebp$/i.test(body) || /^leave$/i.test(body) || /^retn?\b/i.test(body)) return true;
    if (/^call\s+__CxxThrowException/i.test(body) || /^jmp\s+__CxxThrowException/i.test(body)) return true;
  }
  return false;
}

/**
 * 往前找 `reg` 的最近一次定义。
 * ★ 三条口径（都不是"随便选的"，写在这里以免下游当成实现细节）：
 * ① **caller-saved（`eax/ecx/edx`）**：窗口 24 条，且**中间隔着 `call`** 就视为值不可知（`clobbered`）；
 * ② **callee-saved（`esi/edi/ebx/ebp`）**：扫到函数开头（它们跨调用不变），不受 `call` 影响；
 * ③ **退出路径上的 `pop`**：跳过继续往前找（实测 `sub_40ED40` 的错误分支里有一条 `pop esi`，
 *    线性"最近一次定义"会把它当成 esi 的来源 ⇒ 后面几百行整片假 unverifiable）。
 */
function lastDef(raw, siteIdx, fnFrom, reg, window = 24) {
  const calleeSaved = CALLEE_SAVED.has(reg);
  const lower = calleeSaved ? fnFrom : Math.max(fnFrom, siteIdx - window);
  for (let i = siteIdx - 1; i >= lower; i -= 1) {
    const ins = insnAt(raw, i);
    if (!ins) continue;
    if (!calleeSaved && (ins.mn === 'call' || ins.mn === 'jmp') && ['eax', 'ecx', 'edx'].includes(reg)) return { clobbered: true, idx: i };
    const d = (ins.ops[0] ?? '').toLowerCase();
    if (ins.mn === 'pop' && d === reg && exitsHere(raw, i)) continue; // ③
    if (WRITES_FIRST.has(ins.mn) && d === reg) return ins;
    if (ins.mn === 'xor' && d === reg && (ins.ops[1] ?? '').toLowerCase() === reg) return ins;
  }
  return null;
}

/** 站点所属的**基本块起点**（向前走到最近的标签行之后）—— 用于"对象根寄存器在本块里有没有被改过" */
function blockStart(raw, siteIdx, fnFrom) {
  for (let i = siteIdx - 1; i >= fnFrom; i -= 1) {
    const m = P.exec(raw.lines[i]);
    if (!m) continue;
    const body = (m[3] ?? '') + (m[4] ? ` ${m[4]}` : '');
    if (body.trim().endsWith(':')) return i + 1;
    if (PROC.test(raw.lines[i])) return i + 1;
  }
  return fnFrom;
}

/**
 * 基址寄存器是否**证得出**是对象根（`ecx`）。
 * ★★ 这里分**证明**与**推断**两种（`notes` 里记下推断的理由）：
 *   "本块内没被写过"在**入口块**里是证明；在别的块里只是推断（值可能来自前驱块）。
 *   ⇒ 复核器只在**证明**出结果时才敢说 `drifted`；带推断的不一致只能报 `unverifiable`。
 */
function baseIsRoot(raw, siteIdx, fn, baseReg, notes = null) {
  const root = roots().get(fn.sym);
  if (!root) return false;
  const bs = blockStart(raw, siteIdx, fn.from);
  if (baseReg === root) {
    for (let i = bs; i < siteIdx; i += 1) {
      const ins = insnAt(raw, i);
      if (ins && WRITES_FIRST.has(ins.mn) && (ins.ops[0] ?? '').toLowerCase() === root) return false;
    }
    if (notes && bs > fn.from) notes.push(`${root} 在本块内没被写过，但本块不是函数入口块 ⇒ **推断**`);
    return true;
  }
  const def = lastDef(raw, siteIdx, fn.from, baseReg);
  if (!def || def.clobbered) return false;
  if (def.mn === 'mov' && (def.ops[1] ?? '').toLowerCase() === root) {
    if (notes && def.idx < bs) notes.push(`${baseReg} 由**更早的块**里的 \`mov ${baseReg},${root}\` 得到 ⇒ **推断**`);
    return true;
  }
  return false;
}

const aff = (k, b, sym = null) => ({ k, b, sym });
const addA = (a, b) => (a && b && (a.sym === b.sym || !a.sym || !b.sym) ? aff(a.k + b.k, a.b + b.b, a.sym ?? b.sym) : null);

/**
 * **向后局部切片**：把 `reg` 在 `before` 之前的值解成一个仿射量（`null` = 解不出）。
 * 只认 `IDIOMS` 那张表；表外 ⇒ `null`（调用方据此报 `unverifiable`）。
 */
function resolveReg(raw, reg, before, fn, depth = 0, seen = null, lv = null) {
  if (depth > 8) return null;
  // ★★ 声明给了值 ⇒ 这个寄存器**被钉住**：归纳变量的取值来自声明，而不是从 x86 惯用法反推
  if (lv && Object.hasOwn(lv, reg)) return aff(0, lv[reg], null);
  if (seen) seen.add(reg);
  const def = lastDef(raw, before, fn.from, reg);
  if (!def || def.clobbered) return null;
  const { mn, ops, idx } = def;
  const src = (ops[1] ?? '').trim();
  const srcReg = src.toLowerCase();
  if (mn === 'mov' && srcReg === reg) return resolveReg(raw, reg, idx, fn, depth + 1, seen, lv);
  if (mn === 'mov' || mn === 'movzx' || mn === 'movsx') {
    const mem = simpleMem(src);
    if (mem) {
      if (mem.base !== reg && baseIsRoot(raw, idx, fn, mem.base)) return aff(1, 0, `*(Engine+0x${mem.disp.toString(16)})`);
      return null;
    }
    if (srcReg === reg) return resolveReg(raw, reg, idx, fn, depth + 1, seen, lv);
    return isReg(srcReg) ? resolveReg(raw, srcReg, idx, fn, depth + 1, seen, lv) : null;
  }
  if (mn === 'shl' || mn === 'sal') {
    const sh = /^0?([0-9A-Fa-f]{1,2})h$/.exec(src) ?? /^(\d+)$/.exec(src);
    if (!sh) return null;
    const inner = resolveReg(raw, reg, idx, fn, depth + 1, seen, lv);
    if (!inner) return null;
    return aff(inner.k * 2 ** Number.parseInt(sh[1], 16), inner.b * 2 ** Number.parseInt(sh[1], 16), inner.sym);
  }
  if (mn === 'imul') {
    const k = /^0?([0-9A-Fa-f]{1,8})h$/.exec(src) ?? /^(\d+)$/.exec(src);
    if (!k) return null;
    const inner = resolveReg(raw, reg, idx, fn, depth + 1, seen, lv);
    if (!inner) return null;
    const n = Number.parseInt(k[1], 16);
    return aff(inner.k * n, inner.b * n, inner.sym);
  }
  if (mn === 'add' || mn === 'sub') {
    const inner = resolveReg(raw, reg, idx, fn, depth + 1, seen, lv);
    if (!inner) return null;
    const sgn = mn === 'add' ? 1 : -1;
    const k = /^0?([0-9A-Fa-f]{1,8})h$/.exec(src) ?? /^(-?\d+)$/.exec(src);
    if (k) return aff(inner.k, inner.b + sgn * Number.parseInt(k[1], 16), inner.sym);
    const other = isReg(srcReg) ? resolveReg(raw, srcReg, idx, fn, depth + 1, seen, lv) : null;
    if (!other) return null;
    return addA(inner, aff(sgn * other.k, sgn * other.b, other.sym));
  }
  if (mn === 'xor' && srcReg === reg) return aff(0, 0, null);
  if (mn === 'lea') {
    // 纯算术的 `lea`：`lea ecx,[edi+edx*2]`（`edi` 是常量 0）⇒ `30·cur` 就是从这里来的。
    // ★ 若基址**证得出是对象根**，那它是**指针**而不是仿射量 ⇒ 这里必须返回 null。
    const m = indexedMem(src);
    if (!m) return null;
    if (m.base && baseIsRoot(raw, idx, fn, m.base)) return null;
    let acc = aff(0, m.disp + prefixDisp(src), null);
    if (m.base) {
      const b = resolveReg(raw, m.base, idx, fn, depth + 1, seen, lv);
      if (!b) return null;
      acc = addA(acc, b);
      if (!acc) return null;
    }
    if (m.idxReg) {
      const i = resolveReg(raw, m.idxReg, idx, fn, depth + 1, seen, lv);
      if (!i) return null;
      acc = addA(acc, aff(i.k * m.scale, i.b * m.scale, i.sym));
      if (!acc) return null;
    }
    return acc;
  }
  return null;
}

/** 索引寄存器在站点处的仿射量（含"位移前缀"） */
function indexAffine(raw, ins, fn, reg) {
  return resolveReg(raw, reg, ins.idx, fn);
}

/** 函数末尾的行号（下一个 `endp`） */
function functionEnd(raw, idx) {
  for (let i = idx + 1; i < raw.lines.length; i += 1) if (/^\S+\s+\S+\s+endp\b/.test(raw.lines[i].trim()) || /\bendp\b\s*$/.test(raw.lines[i])) return i;
  return raw.lines.length - 1;
}

/** 跳转指令的目标行号（只认函数内标签 `loc_XXXXXX`） */
function jumpTargetIdx(raw, ins) {
  const m = /^(?:short\s+)?(loc_[0-9A-Fa-f]{6})$/.exec((ins.ops[0] ?? '').trim());
  if (!m) return null;
  const ea = Number.parseInt(m[1].slice(4), 16);
  return raw.byEa.get(ea) ?? null;
}

/**
 * ★★ **循环变量**判定（这条是本工作流程里最要紧的一条，因为它的反面是**假 verified**）
 *
 * 判据：站点之后存在一条**回边**（跳到 `T ≤ 站点` 的向后跳），且 `T` 在**该寄存器的定义点之后**
 * （⇒ 定义点只在循环外执行一次），而**回边区间 `[T, 回边]` 里写过这个寄存器** ⇒
 * 这个站点在每次迭代落在**不同的槽**上。实测：`0x40F5C1` 的 `edi` 由 `xor edi,edi`（`0x40F581`）
 * 初始化，循环体 `0x40F583` 起，`inc edi` / `cmp edi,3` / `jl loc_40F583` ⇒ 三轮落在
 * `+0x64/+0x68/+0x6C`。把它报成单个槽就是**假 verified**（复核"通过"而结论是错的）。
 */
function loopVariant(raw, siteIdx, reg, defIdx) {
  const end = functionEnd(raw, siteIdx);
  for (let i = siteIdx + 1; i <= end; i += 1) {
    const ins = insnAt(raw, i);
    if (!ins || !ins.mn.startsWith('j')) continue;
    const t = jumpTargetIdx(raw, ins);
    if (t === null || t > siteIdx || t <= defIdx) continue;
    for (let k = t; k <= i; k += 1) {
      const q = insnAt(raw, k);
      if (q && WRITES_FIRST.has(q.mn) && (q.ops[0] ?? '').toLowerCase() === reg) return { from: t, to: i };
    }
  }
  return null;
}

/**
 * 基址寄存器 → 「对象根 + 还没折进位移的索引项」（`null` = 证不出）。
 * 三种真实形态：
 * ① 基址寄存器**就是**对象根（`ecx`，且本块内没被写过）；
 * ② 它由 `mov base, ecx` 得到（装载器把小对象指针搬进 `esi` 的常见形态）；
 * ③ 它由 `lea base, [ecx + rX*s (+D)]` 得到（**对象根 + 索引**：`0x40F064 lea eax,[esi+ecx*8]`）。
 */
function resolveBase(raw, siteIdx, fn, reg) {
  const notes = [];
  if (baseIsRoot(raw, siteIdx, fn, reg, notes)) return { terms: [], disp: 0, assumed: notes };
  const def = lastDef(raw, siteIdx, fn.from, reg);
  if (!def || def.clobbered) return null;
  const src = (def.ops[1] ?? '').trim();
  if (def.mn === 'mov' && baseIsRoot(raw, def.idx, fn, src.toLowerCase(), notes)) return { terms: [], disp: 0, assumed: notes };
  if (def.mn === 'lea') {
    const m = indexedMem(src);
    if (!m || !m.base || !baseIsRoot(raw, def.idx, fn, m.base, notes)) return null;
    const terms = m.idxReg ? [{ reg: m.idxReg, scale: m.scale, at: def.idx }] : [];
    return { terms, disp: m.disp + prefixDisp(src), assumed: notes };
  }
  return null;
}

/**
 * 独立重解一个 EA ⇒ `{ok, shape, reason}`。
 * `shape` 的字段与助手侧**同名不同源**：obj / absDisp / stride / indexSym / width / access。
 */
export function describeEa(raw, ea, opts = {}) {
  const lv = opts.loopVarValues ?? null;
  const idx = raw.byEa.get(ea);
  if (idx === undefined) return { ok: false, reason: `EA 0x${ea.toString(16)} 不在语料里` };
  const ins = insnAt(raw, idx);
  if (!ins) return { ok: false, reason: `EA 0x${ea.toString(16)} 这一行不是指令` };
  const fn = enclosing(raw, idx);
  // ★ 段前缀符号（`mov eax, ds:dword_559A68`）也是内存访问：**单独一类**（进程全局 / 数据段对象），
  //   它没有"对象根 + 位移"的形状 —— 复核器照样要能出话，不许当成"没有访问"。
  //   ★ 踩过：用一条"符号后面跟逗号 ⇒ 它是目的操作数"的正则判读写 ⇒ `mov eax, ds:dword_X` 里的
  //   `eax,` 会把整条判成**写**（实测 1,680 条假 drifted）。⇒ 必须**按操作数序号**判。
  const segIdx = ins.ops.findIndex((o) => !o.includes('[') && /^(?:[a-z]{2}:\s*)/i.test(o.trim()));
  const memOpIdx = ins.ops.findIndex((o) => o.includes('['));
  const targetIdx = memOpIdx >= 0 ? memOpIdx : segIdx;
  const memIsFirst = targetIdx === 0;
  // 访问方向（本文件自己的一份；x87 与 `div/idiv` 的族**逐条列出**，不靠"一元就 rw"那种兜底）
  const READ = new Set(['cmp', 'test', 'push', 'call', 'jmp', 'div', 'idiv', 'mul', 'imul', 'bt', 'fld', 'fild', 'fadd', 'fsub', 'fmul', 'fdiv', 'fcom', 'fcomp', 'fucom', 'fucomp', 'fiadd', 'fisub', 'fimul', 'fidiv', 'ficom', 'ficomp']);
  const WRITE = new Set(['fst', 'fstp', 'fist', 'fistp', 'fisttp', 'fstsw', 'fnstsw', 'fbstp']);
  const RMW = new Set(['inc', 'dec', 'add', 'sub', 'adc', 'sbb', 'and', 'or', 'xor', 'neg', 'not', 'shl', 'shr', 'sar', 'sal', 'rol', 'ror']);
  let access;
  if (ins.mn === 'lea') access = 'a';
  else if (WRITE.has(ins.mn)) access = 'w';
  else if (READ.has(ins.mn)) access = 'r';
  else if (RMW.has(ins.mn)) access = memIsFirst ? 'rw' : 'r';
  else access = memIsFirst ? 'w' : 'r';
  // 宽度（自己的判据：显式关键字优先，否则看**另一个**操作数的寄存器宽度）
  const kwAny = /^(byte|word|dword|qword|xmmword|tbyte)\s+ptr/i.exec((ins.ops[targetIdx] ?? '').trim()) ?? null;
  let width = kwAny ? { byte: 1, word: 2, dword: 4, qword: 8, xmmword: 16, tbyte: 10 }[kwAny[1].toLowerCase()] : null;
  if (width === null) {
    for (let j = 0; j < ins.ops.length; j += 1) {
      if (j === targetIdx) continue;
      const w = widthOfReg(ins.ops[j].trim());
      if (w) { width = w; break; }
    }
  }
  if (memOpIdx < 0 && segIdx >= 0) {
    const sym = ins.ops[segIdx].replace(/^(byte|word|dword|qword|xmmword|tbyte)\s+ptr\s+/i, '').replace(/^[a-z]{2}:\s*/i, '').trim();
    return { ok: true, shape: { obj: sym, absDisp: null, stride: 0, indexSym: null, width, access }, assumed: [], assumedFields: [], fn: fn?.sym, text: ins.text };
  }
  const memOp = ins.ops[memOpIdx];
  // 形状
  const mem = indexedMem(memOp);
  if (!mem) return { ok: false, reason: `内存操作数的形态落在复核器的**惯用法表**之外：${memOp}` };
  const extraDisp = prefixDisp(memOp);
  const assumptions = [];
  /** 被推断影响的**字段**（其余字段照旧是"证明"出来的：宽度 / 读写是逐字读指令，不受推断影响） */
  const assumedFields = new Set();
  const baseRes = mem.base ? resolveBase(raw, idx, fn, mem.base) : null;
  if (!baseRes) {
    return { ok: false, reason: `基址寄存器 ${mem.base ?? '(无)'} 证不出是对象根（函数 ${fn?.sym ?? '?'}）` };
  }
  if (baseRes.assumed?.length) { assumptions.push(...baseRes.assumed); assumedFields.add('obj'); assumedFields.add('absDisp'); }
  let stride = 0;
  let indexSym = null;
  let disp = baseRes.disp + mem.disp + extraDisp;
  const terms = [...baseRes.terms];
  if (mem.idxReg) terms.push({ reg: mem.idxReg, scale: mem.scale, at: idx });
  const bs = blockStart(raw, idx, fn.from);
  for (const t of terms) {
    const seen = new Set();
    const a = resolveReg(raw, t.reg, t.at, fn, 0, seen, lv);
    if (!a) return { ok: false, reason: `索引寄存器 ${t.reg} 的折叠链解不出（惯用法表外 / 隔着 call / 超出窗口）` };
    // ★ 循环变量判定要覆盖**整条依赖链**（`ecx` 自己不是循环变量，但它是 `lea ecx,[edi+…]` 来的，
    //   而 `edi` 是循环计数器）——只查站点上的那个寄存器会**漏**（实测 0x40F5C1 就是这样漏掉的）
    for (const r of seen) {
      // ★★ 已由声明钉住的寄存器 ⇒ 不再按"循环变量"拒收（它的取值来自声明，不来自反推）
      if (lv && Object.hasOwn(lv, r)) continue;
      const d = lastDef(raw, t.at, fn.from, r);
      const loop = loopVariant(raw, idx, r, d && !d.clobbered ? d.idx : -1);
      if (loop) {
        return { ok: false, reason: `索引寄存器 ${t.reg} 的依赖链里有**循环变量** ${r}（回边 行${loop.from + 1}→行${loop.to + 1} 内被改写）⇒ 这次访问随迭代落在不同槽上，不能报成单个槽` };
      }
      if (d && !d.clobbered && d.idx < bs) { assumptions.push(`索引链上的 ${r} 定义在**更早的块**里 ⇒ **推断**`); assumedFields.add('stride'); assumedFields.add('indexSym'); }
    }
    if (a.sym) { indexSym = a.sym; stride += t.scale * a.k; }
    disp += t.scale * a.b;
  }
  return { ok: true, shape: { obj: 'Engine', absDisp: disp, stride, indexSym, width, access }, fn: fn.sym, text: ins.text, assumed: assumptions, assumedFields: [...assumedFields] };
}

/**
 * **S1a 的判据**：拿一条"声明"（witness EA + 形状）回语料独立重解，给三种话之一。
 * @param {object} row 声明：`{witness, obj, origin, off, absDisp, stride, indexSym, width, access}`
 */
export function checkRow(raw, row, opts = {}) {
  const d = describeEa(raw, row.witness, opts);
  if (!d.ok) return { verdict: 'unverifiable', reason: d.reason, findings: [] };
  const act = d.shape;
  const findings = [];
  const cmp = (field, declared, actual, hard = true) => {
    if (declared === undefined || declared === null) return;
    if (declared !== actual) findings.push({ field, declared, actual, hard });
  };
  cmp('obj', row.obj, act.obj);
  cmp('absDisp', row.absDisp === undefined ? undefined : row.absDisp, act.absDisp);
  cmp('stride', row.stride === undefined ? undefined : row.stride, act.stride);
  cmp('indexSym', row.indexSym === undefined ? undefined : row.indexSym, act.indexSym);
  cmp('width', row.width, act.width);
  cmp('access', row.access, act.access);
  // 原点约定：只是**算术自洽**（原点表是声明的约定，不是语料事实）
  if (row.origin !== undefined && row.off !== undefined) {
    const ORIGIN_ABS = { 'Engine.frames[0]': 0x5d894 };
    const base = ORIGIN_ABS[row.origin];
    if (base === undefined) findings.push({ field: 'origin', declared: row.origin, actual: '复核器不认识这个原点（请先在原点表里登记）', hard: false });
    else if (base + row.off !== act.absDisp) findings.push({ field: 'off', declared: `0x${row.off.toString(16)}`, actual: `0x${(act.absDisp - base).toString(16)}` });
  }
  const hard = findings.filter((f) => f.hard);
  // ★★ **证明 vs 推断**（按**字段**分）：只有当不一致落在**被推断影响**的字段上时，才降级成
  //    `unverifiable`；宽度 / 读写 / 位移这些是**逐字读指令**得来的，推断不影响它们。
  const assumedFields = new Set(d.assumedFields ?? []);
  const proven = hard.filter((f) => !assumedFields.has(f.field));
  const onlyAssumed = hard.length > 0 && proven.length === 0;
  if (onlyAssumed) {
    return { verdict: 'unverifiable', reason: `不一致只落在**被推断影响**的字段（${hard.map((f) => f.field).join(',')}）：${d.assumed[0] ?? ''} ⇒ 判不了（**不是** drifted）`, findings, actual: act, fn: d.fn, text: d.text, assumed: d.assumed };
  }
  return { verdict: proven.length ? 'drifted' : 'verified', findings, actual: act, fn: d.fn, text: d.text, assumed: d.assumed ?? [] };
}

if (process.argv[1]?.endsWith('check.mjs')) {
  const ea = Number.parseInt(process.argv[2] ?? '', 16);
  const raw = loadRaw();
  const d = describeEa(raw, ea);
  console.log(JSON.stringify(d, null, 2));
}
