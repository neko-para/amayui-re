/**
 * tools/lib/shape-access.mjs —— **形状层助手**：逐 site 的"访问记录"（EA 锚）
 *
 * ## 这一层是什么
 * "哪个 EA 以什么形状碰了哪个偏移"的**观察**（前向符号执行；只跟地址 / 算术，不跟值）：
 * obj（对象根位次）/ 绝对位移 / 步长 / 索引来源 / 宽度 / 访问方向 / 跳数（hop）。
 * ★ 身份只许用 **EA / 地址表达式 / 位次名（obj0）**；⛔ 本层**不设原点名**
 *   （`frames` / `cur` 这类是 B 层语义，归 `data/ledger/`）。
 *
 * ## 它不是判据
 * 判据在 `lib/shape-verify.mjs`（另一个实现、方向相反）。两侧**不共用解析 / 折叠代码** —— 硬口径。
 * 语料只读；`.c` 只用来读调用约定（对象根在哪个寄存器）与做第二意见。
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

// ───────────────────────────────────────────────────────────── 行文法（助手侧自己的一份）
const PREFIX = /^([A-Za-z_.][\w.]{0,30}):([0-9A-Fa-f]{8})\s?(.*)$/;
const PROC = /^([A-Za-z_.][\w.]*):([0-9A-Fa-f]{8})\s+(\S+)\s+proc near/;
const ENDP = /^\S+\s+(\S+)\s+endp\b/;
const INSN = /^([a-z][a-z0-9]*)\s*(.*)$/;
const DECL = /^[A-Za-z_$?@][\w.$?@]*=\s*[A-Za-z_]\w*\s+ptr\s+/;
const SEGSYM = /^(ds|es|ss|cs|fs|gs):(.+)$/i;

const REG_BYTES = {
  al: 1, cl: 1, dl: 1, bl: 1, ah: 1, ch: 1, dh: 1, bh: 1,
  ax: 2, cx: 2, dx: 2, bx: 2, si: 2, di: 2, bp: 2, sp: 2,
  eax: 4, ecx: 4, edx: 4, ebx: 4, esi: 4, edi: 4, ebp: 4, esp: 4,
};
for (let i = 0; i < 8; i += 1) { REG_BYTES[`xmm${i}`] = 16; REG_BYTES[`mm${i}`] = 8; }
// ★ 踩过：`before.trim()` 之后末尾没有空格 ⇒ 必须 `\s*$` 收尾，否则**显式宽度整类漏掉**（表现是 width=unknown）
const SIZE_KEYWORD = /^(byte|word|dword|qword|xmmword|tbyte|oword)\s+ptr\s*$/;
const KW_BYTES = { byte: 1, word: 2, dword: 4, qword: 8, xmmword: 16, tbyte: 10, oword: 16 };

/** 顶层逗号切操作数（助手侧自己的一份；与 `tools/lib/catalog.mjs` 无关） */
function ops(rest) {
  const out = [];
  let d = 0;
  let cur = '';
  for (const ch of rest) {
    if (ch === '[' || ch === '(') d += 1;
    else if (ch === ']' || ch === ')') d -= 1;
    if (ch === ',' && d <= 0) { out.push(cur.trim()); cur = ''; } else cur += ch;
  }
  if (cur.trim() !== '') out.push(cur.trim());
  return out;
}

/** `[...]` → `{prefix, inner, base, indices, disp, stack, explicit}`（不是内存操作数 ⇒ null） */
function parseMem(raw) {
  const op = raw.trim();
  const m = /\[([^\]]*)\]/.exec(op);
  if (!m) return null;
  const before = op.slice(0, m.index).trim();
  // ★ 实测两种 IDA 写法：`dword ptr [esi+ecx*8+5D8F4h]`（尺寸在括号前）与
  //   `ds:8[ecx*8]`（**位移写在段前缀里**）⇒ 后者不认会把 `+8` 静默丢掉
  const dispPrefix = /^(?:[a-z]{2}:)?([0-9A-Fa-f]{1,8}h|\d+)$/i.exec(before.replace(/^(byte|word|dword|qword|xmmword|tbyte|oword)\s+ptr\s+/i, '').trim());
  const kw = SIZE_KEYWORD.exec(before);
  const inner = m[1];
  const body = inner.replace(/^(ds|es|ss|cs|fs|gs):/i, '').trim();
  const t = { raw: op, inner, explicit: kw ? KW_BYTES[kw[1]] : null, base: null, indices: [], disp: dispPrefix ? Number.parseInt(dispPrefix[1].replace(/h$/i, ''), 16) : 0, stack: false };
  for (const term of body.match(/[+-]?[^+-]+/g) ?? []) {
    const s = term.trim();
    const sign = s.startsWith('-') ? -1 : 1;
    const b = s.replace(/^[+-]/, '').trim();
    let mm = /^([a-z]{2,4})\*(\d+)$/i.exec(b);
    if (mm) { t.indices.push({ reg: mm[1].toLowerCase(), scale: sign * Number(mm[2]) }); continue; }
    mm = /^([a-z]{2,4})$/i.exec(b);
    if (mm) { if (!t.base) t.base = mm[1].toLowerCase(); else t.indices.push({ reg: mm[1].toLowerCase(), scale: sign }); continue; }
    // ★★ 踩过（**静默丢**的典型）：位移长度写成 `{4,8}` ⇒ `44h` / `0Ch` / `10h` 这类**短位移解不出**
    //   ⇒ 整个操作数被当成"栈槽"跳过。实测代价：全语料少算 ~17.6k 条 site，而且**不报错**。
    mm = /^([0-9A-Fa-f]{1,8})h$/.exec(b);
    if (mm) { t.disp += sign * Number.parseInt(mm[1], 16); continue; }
    mm = /^(\d+)$/.exec(b);
    if (mm) { t.disp += sign * Number(mm[1]); continue; }
    if (/^(var_|arg_)/i.test(b)) { t.stack = true; continue; }
    t.stack = true; // 名字解不出 ⇒ 当栈槽（保守：宁可不报，也不编一个偏移）
  }
  if (t.base === 'ebp' || t.base === 'esp') t.stack = true;
  return t;
}

/**
 * **noreturn** 调用：返回后**没有** fall-through。
 * ★ 闭集合只有它 —— 语料里 `__CxxThrowException@8` 出现 **288 次**，是唯一一个"调用即抛"的帮手
 *   （`RaiseException` 那几处是导入表/文本行，不是 call）。新增一个名字 = 一条语料观察，不许凭印象扩。
 */
const NORETURN = /^_*CxxThrowException(@\d+)?$/i;

/**
 * 栈槽的键（**只做值传播，不做站点**）。
 * ★ 为什么必须做：对象根会被溢出到栈槽、在 catch / 循环块里读回来
 *   （实测 `sub_429460`：`mov esi,ecx` → `mov [ebp+var_354],esi` → `mov esi,[ebp+var_354]`
 *   ⇒ 8 处访问本该按 obj0 判，早先全成"未解析基址"）。⛔ 栈槽本身仍然**不算**对象形状
 *   （`siteOf` 直接跳过栈操作数），这里只让"值"能过桥。
 * ★ 键用 `inner`（括号里那段）：它与尺寸关键字无关（`dword ptr [ebp+var_4]` 与 `[ebp+var_4]` 同键）。
 */
const slotKey = (mem) => `@slot:${mem.inner.toLowerCase().replace(/\\s+/g, '')}`;

// ───────────────────────────────────────────────────────────── 值模型
const UNK = { t: 'unk' };
const isUnk = (v) => !v || v.t === 'unk';
const ofThis = (off) => `Engine+0x${(off >>> 0).toString(16)}`;
const objName = (obj, off) => (obj === 'this' ? ofThis(off) : `${obj}+0x${(off >>> 0).toString(16)}`);

function aff(k, b, sym = null) { return { t: 'val', sym, k, b }; }
/**
 * ★ 踩过：`mov eax,[esi+5D880h]` 读出来的值既是**指针**（当 base ⇒ memberptr）又是**算术来源**
 * （`mov ecx,eax; shl ecx,4; sub ecx,eax` ⇒ 15·cur）。⇒ 必须让 `load` 也能参与仿射算术，
 * 否则"折叠链"整类看不见（表现是 index 解析失败，而不是报错）。
 */
function asAff(v) {
  if (isUnk(v)) return null;
  if (v.t === 'val') return v;
  if (v.t === 'load') return aff(1, 0, v.sym);
  return null;
}
function addV(a, b) {
  const x = asAff(a);
  const y = asAff(b);
  if (!x || !y) return UNK;
  if (x.sym && y.sym && x.sym !== y.sym) return UNK;
  return aff(x.k + y.k, x.b + y.b, x.sym ?? y.sym);
}
function subV(a, b) {
  const y = asAff(b);
  return y ? addV(a, aff(-y.k, -y.b, y.sym)) : UNK;
}
function mulImm(a, n) {
  const x = asAff(a);
  return x ? aff(x.k * n, x.b * n, x.sym) : UNK;
}
function constOf(v) { const x = asAff(v); return x && x.k === 0 ? x.b : null; }

function foldNote(v) {
  if (isUnk(v)) return '?';
  if (v.t === 'this') return 'obj';
  if (v.t === 'glob') return `ds:${v.sym}`;   // ★ 全局里装着的东西（`glob` 的语义见 `evalOperand`）
  if (v.t === 'addr') return `&(${objName(v.obj, v.off)}${v.terms.length ? '+idx' : ''})`;
  if (v.t === 'load') return v.sym;
  if (v.t === 'val') return v.sym ? `${v.k}*${v.sym}${v.b ? `+0x${(v.b >>> 0).toString(16)}` : ''}` : `0x${(v.b >>> 0).toString(16)}`;
  return '?';
}

/**
 * 一个内存操作数 → 形状（obj / 绝对位移 / 步长 / 索引来源）。
 * `base` 与 `indices` 都是**进入本条指令时**的寄存器值。
 */
/**
 * 一个内存操作数 → 形状（obj / 绝对位移 / 步长 / 索引来源 / **路径**）。
 * `base` 与 `indices` 都是**进入本条指令时**的寄存器值。
 *
 * ★ **路径（`basePath`）是"基址经过了几跳指针"的身份**：`[]` = 就是 `obj0`；`[A]` = "从 `obj0+A` 读出的指针"；
 *   `[A,B]` = 再一跳。它是**对象层级 = 路径长度 + 1** 的唯一依据（`baseHop` 的精确版）。
 *   ★ 踩过：早先只有 `pointeeHost`（**最后一跳**的偏移），于是
 *   ① `lea eax,[<二级对象>+k]` 之后再解引用 ⇒ 路径丢了（报成层级 1，从所有桶里掉出去）；
 *   ② 三跳链报成 `pointeeHost = 第二跳的偏移` ⇒ 拿它去对 `obj0` 的成员表 = **宿主认错层**（会假解释）。
 */
function shapeOf(fr, mem) {
  const out = { obj: null, off: 0, terms: [], absolute: false, problems: [], basePath: null };
  if (!mem.base) {
    if (mem.indices.length === 0) { out.problems.push('base'); return out; }
    out.problems.push('base');
    return out;
  }
  const bv = fr.get(mem.base);
  if (bv && bv.t === 'this') { out.obj = 'this'; out.off = 0; out.basePath = []; }
  else if (bv && bv.t === 'addr') { out.obj = bv.obj; out.off = bv.off; out.terms.push(...bv.terms); out.basePath = bv.path ?? []; }
  else if (bv && bv.t === 'load') { out.obj = objName(bv.obj, bv.off); out.off = 0; out.basePath = bv.path ?? []; }
  else { out.problems.push('base'); }
  for (const { reg, scale } of mem.indices) {
    const v = fr.get(reg);
    const a = asAff(v);
    if (a && a.sym) out.terms.push({ sym: a.sym, k: a.k, b: a.b, scale });
    else if (a && a.k === 0) out.terms.push({ sym: null, k: 0, b: a.b, scale });
    else { out.problems.push('index'); }
  }
  out.off += mem.disp;
  for (const t of out.terms) { out.off += t.scale * t.b; t.b = 0; }
  const syms = new Set(out.terms.filter((t) => t.sym).map((t) => t.sym));
  if (syms.size > 1) out.problems.push('index');
  out.stride = 0;
  out.indexSym = null;
  for (const t of out.terms) { out.stride += t.scale * t.k; if (t.sym) out.indexSym = t.sym; }
  if (!out.indexSym) out.stride = 0;
  out.absDisp = out.obj === 'this' ? out.off : null;
  return out;
}

/**
 * 纯算术形态的 `lea`（`lea ecx,[edi+edx*2]`，`edi` 是常量 0 / 仿射量）⇒ 一个**数值**（不是指针）。
 * ★ 这条是"`*4` 形态与 `*8` 形态净步长同为 0x78"那个已知结论的落点：`0x40F5BE` 的 `lea ecx,[edi+edx*2]`
 *   把 `15·cur` 折成 `30·cur`，`[esi+ecx*4+5D8F8h]` 的净步长因此是 `4×30 = 120`。
 */
function numOf(fr, mem) {
  let acc = aff(0, mem.disp);
  if (mem.base) acc = addV(acc, fr.get(mem.base));
  if (isUnk(acc)) return null;
  for (const { reg, scale } of mem.indices) {
    const v = fr.get(reg);
    if (isUnk(v)) return null;
    const m = mulImm(v, scale);
    acc = addV(acc, m);
    if (isUnk(acc)) return null;
  }
  return acc;
}

// ───────────────────────────────────────────────────────────── 逐函数分析
/**
 * 寄存器状态 + 一个**可选**的调用点策略。
 *
 * ★ `killCallerSavedOnCall`：`eax/ecx/edx` 是 caller-saved ⇒ 按 ABI 该杀。**但本语料里实测**
 * `sub_41BF50` 的 `[ecx+5EC8Ch]`（`0x41BFB6`）与 `[ecx+5D800h]`（`0x41BFDD`）都出现在 `call` 之后、
 * 且中途**没有**任何 `mov ecx,` 重载 ⇒ 这些 callee 事实上保留了 `ecx`。
 * ⇒ 这个开关**必须带着观察报数**（保守档 vs 校准档），不许当成"无所谓的选择"。
 */
class Frame {
  constructor(rootReg, opts = {}) {
    this.r = new Map();
    this.rootReg = rootReg ?? null;
    this.stack = [];
    this.opts = opts;
    if (this.rootReg) this.r.set(this.rootReg, { t: 'this' });
  }
  get(reg) { return this.r.get(reg) ?? UNK; }
  set(reg, v) { this.r.set(reg, v); if (reg === 'ecx') this.r.set('#ecxSet', { t: 'flag' }); }
  clobberCall() {
    this.r.set('eax', UNK);
    this.r.set('edx', UNK);
    if (this.opts.killCallerSavedOnCall !== false) this.r.set('ecx', UNK);
    this.r.delete('#ecxSet');
  }
  snapshot() { return new Map(this.r); }
  restore(map) { this.r = new Map(map); this.stack = []; }
}

/** 值的**规范字符串**（用于控制流汇合点的比较：不同 ⇒ 降到 UNK） */
function vkey(v) {
  if (!v || v.t === 'unk') return 'unk';
  if (v.t === 'this') return 'this';
  if (v.t === 'flag') return 'flag';
  // ★ 符号必须进 key：否则两个不同的全局（`ds:dword_A` / `ds:dword_B`）在汇合点被当成同一个值，
  //   `mergeState` 会**静默保留**其中一支（实测形态：`var = cond ? ds:X : ds:Y`）。
  if (v.t === 'glob') return `glob:${v.seg ?? 'ds'}:${v.sym}`;
  if (v.t === 'addr') return `addr:${v.obj}:${v.off}:${(v.path ?? []).join(',')}:${(v.terms ?? []).map((t) => `${t.sym}*${t.k}*${t.scale}`).join(',')}`;
  if (v.t === 'load') return `load:${v.obj}:${v.off}:${(v.path ?? []).join(',')}`;
  if (v.t === 'val') return `val:${v.sym ?? ''}:${v.k}:${v.b}`;
  return 'unk';
}
/** 汇合：两边同 ⇒ 留；不同 ⇒ 丢（= UNK）。`a === null` 表示这个点**还没被到达过** */
function mergeState(a, b) {
  if (a === null) return new Map(b);
  const out = new Map();
  for (const [k, v] of a) {
    const w = b.get(k);
    if (w !== undefined && vkey(w) === vkey(v)) out.set(k, v);
  }
  return out;
}
function sameState(a, b) {
  if (a === null || b === null) return a === b;
  if (a.size !== b.size) return false;
  for (const [k, v] of a) if (vkey(b.get(k)) !== vkey(v)) return false;
  return true;
}

const RMW = new Set(['inc', 'dec', 'add', 'sub', 'and', 'or', 'xor', 'neg', 'not', 'shl', 'shr', 'sar', 'sal', 'rol', 'ror', 'adc', 'sbb']);
/** **只读**的一元助记符（★ 原先漏了 x87 与 `div/idiv` ⇒ 967 条 `access` 判错，是**独立复核器**抓出来的） */
const READ_ONLY = new Set([
  'cmp', 'test', 'push', 'call', 'jmp',
  'div', 'idiv', 'mul', 'imul', 'bt',
  'fld', 'fild', 'fadd', 'fsub', 'fmul', 'fdiv', 'fcom', 'fcomp', 'fucom', 'fucomp',
  'fiadd', 'fisub', 'fimul', 'fidiv', 'ficom', 'ficomp',
]);
/** **只写**的一元助记符（x87 的 store 族） */
const WRITE_ONLY = new Set(['fst', 'fstp', 'fist', 'fistp', 'fisttp', 'fstsw', 'fnstsw', 'fbstp']);

/**
 * 一次内存访问是读 / 写 / 读改写 / 只取址。
 * ★ 口径：`RMW` 类**只有操作数 0 是内存时**才是 `rw`（`add eax,[mem]` 只读那个内存操作数）。
 */
function accessOf(mn, opIdx, opCount, isMem0) {
  if (mn === 'lea') return 'a';
  if (WRITE_ONLY.has(mn)) return 'w';
  if (READ_ONLY.has(mn)) return 'r';
  if (RMW.has(mn)) return isMem0 ? 'rw' : 'r';
  if (mn === 'mov' || mn === 'movzx' || mn === 'movsx') return isMem0 ? 'w' : 'r';
  if (opCount === 1) return 'rw';
  return isMem0 ? 'w' : 'r';
}
function widthFromOther(operands, i) {
  for (let j = 0; j < operands.length; j += 1) {
    if (j === i) continue;
    const w = REG_BYTES[operands[j].trim().toLowerCase()];
    if (w) return w;
  }
  return null; // ⇒ unknown，**不许**当 4
}

/** 直接跳转目标（函数内标签）：`jmp short loc_42CA8A` / `jz loc_...` */
const JMP_TARGET = /^(?:short\s+)?(loc_[0-9A-F]{6}|def_[0-9A-F]{6})$/;

/**
 * 解析一个函数：指令表（含标签归属）+ 后继边。
 * ★ 为什么要这一步：**线性扫过函数体**会把互斥路径拼在一起（实测 `sub_41BF50` 的 `retn 4` 之后
 *   紧接着就是下一个 case 块 ⇒ 上一个 case 的寄存器值污染下一个 case）——未解析 base 一度占一半。
 */
function parseFunction(lines, from, to) {
  const insns = [];
  const labels = new Map(); // 标签名 → 指令下标
  const jumptable = []; // 带 `; jumptable` 标注的标签名（间接跳转的目标集）
  let pending = [];
  for (let i = from; i <= to; i += 1) {
    const pl = PREFIX.exec(lines[i]);
    if (!pl) continue;
    const ea = Number.parseInt(pl[2], 16);
    const body = (pl[3] ?? '').replace(/\s+;.*$/, '').trim();
    if (body.endsWith(':')) {
      const nm = body.slice(0, -1);
      pending.push(nm);
      if (/;\s*jumptable/i.test(lines[i])) jumptable.push(nm);
      continue;
    }
    if (body === '' || body.startsWith(';') || DECL.test(body)) continue;
    const im = INSN.exec(body);
    if (!im) continue;
    for (const nm of pending) labels.set(nm, insns.length);
    insns.push({ i, ea, raw: lines[i].trimEnd(), body, mn: im[1], operands: ops(im[2] ?? ''), labels: [...pending], succ: [] });
    pending = [];
  }
  const idxOfLabel = (op) => {
    const m = JMP_TARGET.exec((op ?? '').trim());
    return m ? labels.get(m[1]) ?? null : null;
  };
  for (let k = 0; k < insns.length; k += 1) {
    const { mn, operands, raw } = insns[k];
    const next = k + 1 < insns.length ? k + 1 : null;
    if (mn === 'jmp') {
      const t = idxOfLabel(operands[0]);
      if (t !== null) insns[k].succ = [t];
      else if (/jpt_|ds:/i.test(raw)) insns[k].succ = jumptable.map((n) => labels.get(n)).filter((x) => x !== undefined);
      else insns[k].succ = [];
      continue;
    }
    if (mn.startsWith('ret')) { insns[k].succ = []; continue; }
    if (mn.startsWith('j')) {
      const t = idxOfLabel(operands[0]);
      insns[k].succ = (t !== null ? [t, next] : [next]).filter((x) => x !== null);
      continue;
    }
    // ★★ `noreturn` 之后**没有** fall-through：否则抛出路径上的寄存器值会污染汇合点后面的块。
    //   实测（`sub_41C6A0`）：`add esi,8` 在抛异常路径上，而 `jl` 那条边的块里 `esi` 仍是 obj0
    //   ⇒ 早先汇合后判 UNK，后面 9 处访问（+8 后落在已声明成员 0x5d888/0x5d88c/0x5eb5c）全丢。
    if (mn === 'call' && NORETURN.test((operands[0] ?? '').trim())) { insns[k].succ = []; continue; }
    insns[k].succ = next === null ? [] : [next];
  }
  return { insns, labels, jumptable };
}

/** 结尾弹出（`pop` 之后紧跟 `mov esp,ebp` / `leave` / `retn`）⇒ 走的是**退出路径**，不动状态 */
function makeEpiloguePop(lines, to) {
  return (i) => {
    for (let j = i + 1; j <= Math.min(i + 5, to); j += 1) {
      const b = (PREFIX.exec(lines[j])?.[3] ?? '').trim();
      if (/^mov\s+esp,\s*ebp$/i.test(b) || /^leave$/i.test(b) || /^retn?\b/i.test(b)) return true;
    }
    return false;
  };
}

/** 一条内存操作数 → site 行（**声明**，不是判据；判据在 `check.mjs`） */
function siteOf(fr, mn, operands, idx, ea, line, rawLine) {
  const raw = operands[idx];
  const mem = parseMem(raw);
  // ★ `lea r,[r+0]` **不是访问**：同寄存器、位移 0、无索引 —— MSVC 用它当 6 字节对齐填充
  //   （实测 `0x46547A lea ebx,[ebx+0]` 被记成 `obj0+0x0` 的访问 ⇒ `T_Font+0x0` 4 处假残差）。
  //   `lea r,[r+K]`（K≠0）仍要算"取址"（那是内嵌子对象的基址），`lea r,[s+0]`（换寄存器）也不算填充。
  if (mn === 'lea' && mem && !mem.stack && mem.base
      && mem.indices.length === 0 && mem.disp === 0) {
    const dst = (operands[0] ?? '').trim().toLowerCase();
    if (dst && dst === mem.base) return null;
  }
  const seg = SEGSYM.exec(raw.replace(/^(byte|word|dword|qword|xmmword|tbyte)\s+ptr\s+/i, '').trim());
  if (!mem && !seg) return null;
  if (mem && mem.stack) return null; // 栈槽不是本单的对象形状
  const access = accessOf(mn, idx, operands.length, idx === 0);
  const head = raw.slice(0, raw.indexOf('[') >= 0 ? raw.indexOf('[') : raw.length).trim();
  const kw = SIZE_KEYWORD.exec(head);
  const width = kw ? KW_BYTES[kw[1]] : (mem?.explicit ?? widthFromOther(operands, idx));
  const row = {
    ea, line, raw: rawLine, mn, access,
    width: width ?? 'unknown',
    widthHow: kw || mem?.explicit ? 'explicit' : width ? 'register' : 'unresolved',
    problems: [],
  };
  if (seg && !mem) {
    // `mov eax, ds:dword_559A68` —— 进程全局 / 数据段符号：也是内存访问，**不许静默丢**
    Object.assign(row, { baseKind: 'absolute', obj: seg[2], absDisp: null, stride: 0, indexSym: null, fold: `&${seg[2]}`, pointeePath: [] });
  } else {
    const sh = shapeOf(fr, mem);
    const baseV = mem.base ? fr.get(mem.base) : UNK;
    // ★ S2e：基址是**全局符号**（`mov eax, ds:dword_55E1BC` 之后 `[eax+K]`）时，把符号记进站点，
    const path = sh.basePath ?? [];
    Object.assign(row, {
      baseReg: mem.base ?? null,
      baseKind: sh.obj === 'this' ? 'this' : !sh.obj ? 'unresolved' : baseV.t === 'addr' ? 'embed' : 'memberptr',
      obj: sh.obj === 'this' ? 'Engine' : sh.obj,
      absDisp: sh.absDisp,
      stride: sh.stride,
      indexSym: sh.indexSym,
      // ★ 二级对象用（判定器"下钻"要的）：**路径**（几跳、每跳哪个成员偏移）+ 最后一跳对象内的偏移 + 寻址 scale
      pointeePath: path,                                        // [] = obj0 相对；[A] = 二级；[A,B] = 三级
      pointeeHost: path.length === 1 ? path[0] : null,           // 旧字段（**只对一跳**有意义）——保留兼容
      // ★ 负的 pointee 偏移 = **分配的头部**，不是"缺一条成员"（折叠层也没算错：地址确实是 `p−K`）：
      //   MSVC `new[]` 在元素数组前面塞一个**个数 cookie**（写端 `mov [p],n` + `add p,4`，读端用 `[p-4]`），
      //   于是 `[base-4]` 指的是"分配块头"而不是宿主对象的成员 ⇒ 置 null 并打 `neg-off` 标记，走"判不了"档。
      //   合成复现见 `.tmp/shape/probe-negative-pointee.mjs`（5 行就重现）；守卫 `tools/test/shape-access.test.mjs`。
      pointeeOff: path.length ? (typeof sh.off === 'number' && sh.off < 0 ? null : sh.off) : null,
      // ★ 相对**基址那个对象**的偏移：路径解不出时 `pointeeOff` 是 null，但"这个访问打在基址对象的哪个偏移上"总是知道的
      globSym: baseV && baseV.t === 'glob' ? baseV.sym : null,
      baseOff: sh.off,
      // ★ S2af：**强度削减的乘数链**。`[base + reg*S]` 里的 `reg` 往往不是原始下标，而是 `m·idx`
      //   （`mov edx,eax; shl edx,4; sub edx,eax` ⇒ 15·idx）。寄存器抽象值里已经有乘数 `k`（`v.t === 'val'`）
      //   ⇒ 有效步长 = S × k。⛔ 拿裸 `S` 去比元素大小会**双向出错**：既报假冲突（8 vs 0x1C），
      //   也漏掉真匹配（0x78 === size(T1)）。
      memScale: (() => {
        if (!mem.indices.length) return 1;
        const ixv = fr.get(mem.indices[0].reg);
        const m = ixv && ixv.t === 'val' && typeof ixv.k === 'number' && ixv.k > 1 ? ixv.k : null;
        return m ? mem.indices[0].scale * m : mem.indices[0].scale;
      })(),
      baseHop: path.length,                                      // 对象层级 = baseHop + 1（精确值）
      fold: mem.indices.map((x) => `[${x.reg}*${x.scale}]=${foldNote(fr.get(x.reg))}`).join(' ') || null,
    });
    row.problems.push(...sh.problems);
  }
  if (row.width === 'unknown') row.problems.push('width');
  return row;
}

/** 一条调用点 → link 行（对象根来源分类） */
function linkOf(fr, body, ea, line, rawLine) {
  const cs = /\b(call|jmp)\s+(sub_[0-9A-F]{6})\b/.exec(body);
  if (!cs) return null;
  const v = fr.get('ecx');
  let cls = 'none';
  let detail = null;
  if (v.t === 'this') cls = 'same';
  else if (v.t === 'glob') { cls = 'global'; detail = `ds:${v.sym}`; }
  else if (v.t === 'addr' && v.obj === 'this') { cls = 'embed'; detail = `this+0x${v.off.toString(16)}`; }
  else if (v.t === 'load' && v.obj === 'this') { cls = 'memberptr'; detail = `*(this+0x${v.off.toString(16)})`; }
  else if (v.t === 'val') cls = 'other-value';
  else if (fr.get('#ecxSet').t === 'flag') cls = 'defined-unclassified';
  // ★ 调用点的**实参来源**（6 个通用寄存器）：给"调用方 ↔ 被调方 类型一致性"用（见 shape-calls.mjs）。
  //   ⛔ 栈上传参（`push`）本版不覆盖：那要假设 ABI 的"参数位 ↔ push 顺序"映射，属未证约定。
  const prov = (reg) => {
    const x = fr.get(reg);
    if (!x || x.t === 'unk' || x.t === 'flag') return { k: 'unknown' };
    if (x.t === 'this') return { k: 'this' };
    if (x.t === 'glob') return { k: 'global', sym: x.sym, seg: x.seg ?? 'ds' };
    if (x.t === 'addr') return { k: 'interior', obj: x.obj, off: x.off, path: x.path ?? [] };
    if (x.t === 'load') return { k: 'member', obj: x.obj, off: x.off, path: x.path ?? [] };
    if (x.t === 'val') return { k: 'val', sym: x.sym ?? null };
    return { k: 'unknown' };
  };
  const args = Object.fromEntries(['ecx', 'eax', 'edx', 'esi', 'edi', 'ebx'].map((r) => [r, prov(r)]));
  return { ea, line, raw: rawLine, target: cs[2], kind: cs[1], cls, tail: cs[1] === 'jmp', detail, ecx: foldNote(v), args };
}

/**
 * 逐函数分析（**控制流感知**）：先在指令图上把寄存器状态迭代到不动点，再用每个点的入口状态
 * 收集 site / link。未到达的指令**按空状态照样报**（全 UNK）—— 不许因为"我没走到"就不报。
 */
export function analyzeLines(lines, from, to, rootReg, opts = {}) {
  const { insns } = parseFunction(lines, from, to);
  if (insns.length === 0) return { sites: [], links: [], rounds: 0, insns: 0 };
  const epilogue = makeEpiloguePop(lines, to);
  const entry = new Array(insns.length).fill(null);
  entry[0] = new Frame(rootReg, opts).snapshot();
  let rounds = 0;
  let changed = true;
  while (changed && rounds < 40) {
    changed = false;
    rounds += 1;
    for (let k = 0; k < insns.length; k += 1) {
      const st = entry[k];
      if (st === null) continue;
      const fr = new Frame(null, opts);
      fr.restore(st);
      step(fr, insns[k].mn, insns[k].operands, { lines, i: insns[k].i, to, isEpiloguePop: epilogue });
      const outMap = fr.snapshot();
      for (const s of insns[k].succ) {
        if (s === null || s < 0 || s >= insns.length) continue;
        const merged = mergeState(entry[s], outMap);
        if (!sameState(merged, entry[s])) { entry[s] = merged; changed = true; }
      }
    }
  }
  const sites = [];
  const links = [];
  for (let k = 0; k < insns.length; k += 1) {
    const rec = insns[k];
    const fr = new Frame(null, opts);
    fr.restore(entry[k] ?? new Map());
    for (let idx = 0; idx < rec.operands.length; idx += 1) {
      const row = siteOf(fr, rec.mn, rec.operands, idx, rec.ea, rec.i + 1, rec.raw);
      if (row) sites.push(row);
    }
    const lk = linkOf(fr, rec.body, rec.ea, rec.i + 1, rec.raw);
    if (lk) links.push(lk);
    step(fr, rec.mn, rec.operands, { lines, i: rec.i, to, isEpiloguePop: epilogue });
  }
  return { sites, links, rounds, insns: insns.length };
}

function step(fr, mn, o, ctx) {
  const R = (x) => (x ?? '').trim().toLowerCase();
  switch (mn) {
    case 'mov': {
      const [d, s] = o;
      if (!d || !s) return;
      // ★ 栈槽溢出：`mov [ebp+x], <值>` ⇒ 记下来，后面 `mov reg,[ebp+x]` 才能把身份找回来。
      //   ⛔ 只记**整宽**写（没有 byte/word 之类尺寸关键字）——半宽写会让 4 字节槽里留下陈旧的上半段。
      const dm = parseMem(d);
      if (dm && dm.stack) {
        if (!dm.explicit || dm.explicit === 4) fr.set(slotKey(dm), evalOperand(fr, s));
        return;
      }
      const dl = R(d);
      if (!(dl in REG_BYTES)) return;
      fr.set(dl, evalOperand(fr, s));
      return;
    }
    case 'lea': {
      const [d, s] = o;
      const dl = R(d);
      if (!(dl in REG_BYTES) || !s) return;
      const mem = parseMem(s);
      if (!mem || mem.stack) { fr.set(dl, UNK); return; }
      const sh = shapeOf(fr, mem);
      if (sh.obj) { fr.set(dl, { t: 'addr', obj: sh.obj, off: sh.off, terms: sh.terms, path: sh.basePath ?? [] }); return; }
      const n = numOf(fr, mem);
      fr.set(dl, n ?? UNK);
      return;
    }
    case 'movzx':
    case 'movsx': {
      const [d, s] = o;
      const dl = R(d);
      if (dl in REG_BYTES) fr.set(dl, evalOperand(fr, s));
      return;
    }
    case 'add':
    case 'sub':
    case 'and':
    case 'or':
    case 'xor': {
      const [d, s] = o;
      const dl = R(d);
      if (!(dl in REG_BYTES) || !s) return;
      const a = fr.get(dl);
      if (a.t === 'addr') {
        // `add <指针>, imm` 是常见的取址算术（`add esi, 8`）
        const n = constOf(evalOperand(fr, s));
        if (n !== null && (mn === 'add' || mn === 'sub')) fr.set(dl, { t: 'addr', obj: a.obj, off: a.off + (mn === 'add' ? n : -n), terms: a.terms });
        else fr.set(dl, UNK);
        return;
      }
      if (dl === R(s) && (mn === 'xor' || mn === 'sub')) { fr.set(dl, aff(0, 0)); return; }
      const b = evalOperand(fr, s);
      fr.set(dl, mn === 'add' ? addV(a, b) : mn === 'sub' ? subV(a, b) : UNK);
      return;
    }
    case 'shl':
    case 'sal': {
      const [d, s] = o;
      const dl = R(d);
      const n = constOf(evalOperand(fr, s ?? ''));
      if (!(dl in REG_BYTES) || n === null) return;
      fr.set(dl, mulImm(fr.get(dl), 2 ** n));
      return;
    }
    case 'imul': {
      const [d, s] = o;
      const dl = R(d);
      if (!(dl in REG_BYTES)) return;
      if (o.length === 2) {
        const n = constOf(evalOperand(fr, s));
        if (n !== null) { fr.set(dl, mulImm(fr.get(dl), n)); return; }
        const a = fr.get(dl);
        const b = evalOperand(fr, s);
        const ka = asAff(a)?.k ?? null;
        const kb = asAff(b)?.k ?? null;
        if (asAff(a)?.sym && kb !== null) fr.set(dl, mulImm(a, kb));
        else if (asAff(b)?.sym && ka !== null) fr.set(dl, mulImm(b, ka));
        else fr.set(dl, UNK);
      }
      return;
    }
    case 'inc':
    case 'dec': {
      const dl = R(o[0]);
      if (dl in REG_BYTES) fr.set(dl, addV(fr.get(dl), aff(0, mn === 'inc' ? 1 : -1)));
      return;
    }
    case 'push': {
      fr.stack.push(o[0] && R(o[0]) in REG_BYTES ? fr.get(R(o[0])) : evalOperand(fr, o[0] ?? ''));
      return;
    }
    case 'pop': {
      const dl = R(o[0]);
      const v = fr.stack.pop() ?? UNK;
      // ★ 退出路径上的 `pop esi` 不该污染 fall-through 的状态（实测：装载器 `0x40EF4F` 的 `pop esi`
      //   曾把后面 200 行整片判成未解析）
      if (!(dl in REG_BYTES) || ctx.isEpiloguePop(ctx.i)) return;
      if (v.t === 'this') fr.set(dl, v); else fr.set(dl, v);
      return;
    }
    case 'call':
    case 'jmp':
      fr.clobberCall();
      return;
    // ★ 串指令会推进 `esi`/`edi`（`movs*` 两者；`stos*` 只 `edi`）。⛔ `clobberCall()` 不够：
    //   `esi`/`edi` 是**粘性寄存器**，不清就会让 `rep movsd` 之后 `[esi+disp]` 仍被当成对象相对位移 = 假解释。
    //   语料 5 处抽样：`rep` 之后代码都用别的寄存器或栈，没有一处仍把 `esi/edi` 当对象基址。
    case 'movs':
    case 'movsb':
    case 'movsw':
    case 'movsd':
    case 'rep':
    case 'repe':
    case 'repne':
      fr.clobberCall();
      for (const rg of ['esi', 'edi', 'ecx', 'eax']) fr.set(rg, UNK);
      return;
    case 'stos':
    case 'stosb':
    case 'stosw':
    case 'stosd':
      fr.clobberCall();
      fr.set('edi', UNK);
      return;
  }
}

/** 操作数 → 值 */
function evalOperand(fr, raw) {
  const s = (raw ?? '').trim();
  const reg = s.toLowerCase();
  if (reg in REG_BYTES) return fr.get(reg);
  let m = /^0([0-9A-Fa-f]+)h$/.exec(s);
  if (m) return aff(0, Number.parseInt(m[1], 16));
  m = /^(-?\d+)$/.exec(s);
  if (m) return aff(0, Number(m[1]));
  if (/^offset\s+/i.test(s)) return UNK; // 立即数地址：不是"碰了某个偏移"
  // ★ **数据段符号**（`mov ecx, ds:dword_55E1BC`）：值是"某个全局槽里装着的东西" ——
  //   既不是常量、也不是 obj0 的成员。**旧写法落到 UNK** ⇒「调用点上这个寄存器来自全局」整类消失
  //   （实测：全库 7212 个调用点里 ecx 解不出类型的有 4256 个，其中最大一撮就是这个形态）。
  //   而 `dword_55E1BC` 正是 Command（= Engine）的**唯一存放点**（WinMain 里 `operator new` → ctor → store，
  //   `0x4baa11` / `0x4baa2e` / `0x4baa3a`）⇒ 少了这一类，"从根正向传播"一步都走不了。
  //   ⛔ 带方括号的（`ds:8[ecx*8]`，位移写在段前缀里）**不是**符号，仍交给 `parseMem`。
  const segm = SEGSYM.exec(s);
  if (segm && !segm[2].includes('[')) return { t: 'glob', seg: segm[1].toLowerCase(), sym: segm[2].trim() };
  const mem = parseMem(s);
  if (mem && mem.stack) {
    // ★ 栈槽**只**在"存过"的时候有值；没存过 ⇒ UNK（⛔ 不许猜成"某个对象"）
    return fr.r.get(slotKey(mem)) ?? UNK;
  }
  if (mem && !mem.stack) {
    const sh = shapeOf(fr, mem);
    const basePath = sh.basePath ?? [];
    // 一跳：基址是 obj0（`this`）或 obj0 的 `lea` ⇒ 值 = `*(obj0+off)`，跳数 = 1
    if (sh.obj === 'this' && !sh.indexSym && !sh.problems.includes('index')) {
      return { t: 'load', obj: 'this', off: sh.off, name: ofThis(sh.off), sym: `*(${ofThis(sh.off)})`, hop: 1, path: [sh.off] };
    }
    // ★★ **多跳**：基址本身是"从某个对象里读出来的指针"（`sh.obj` 形如 `Engine+0x…`）
    //   ⇒ 值 = `*(宿主+元素内偏移)`，路径 = 宿主的路径 + 这一跳的偏移。
    //   ★ 身份仍只能写成**路径字符串**（`Engine+0x5d818+0x10`）给人看；判定器用的是 `path` 数字数组。
    if (typeof sh.obj === 'string' && sh.obj !== 'this' && !sh.indexSym && !sh.problems.includes('index')) {
      const host = sh.obj;
      return {
        t: 'load', obj: host, off: sh.off, name: `${host}+0x${sh.off.toString(16)}`,
        sym: `*(${host}+0x${sh.off.toString(16)})`, hop: basePath.length + 1,
        path: [...basePath, sh.off],
      };
    }
    return UNK;
  }
  return UNK;
}

// ───────────────────────────────────────────────────────────── 原点：**本层不设原点名**
/**
 * ★★ 这里原先有一张 `ORIGINS` 表（`Engine.frames[0]` / `Engine.cur-family`）—— **已删除**。
 * 理由（讨论结论）：`frames` / `cur` 这类名字是**引擎语义**（B 层，归台账），而且它们是我**自造**的；
 * 汇编层的引用只许用 **EA / 地址表达式 / 位次名**（`obj0`），语义名只能出现在不参与判定的 label 里。
 * ⇒ 本层只输出**绝对位移**（`absDisp`，相对对象根）；"它落在哪个原点窗口"是**声明**里的判断（types 的成员表）。
 */

// ───────────────────────────────────────────────────────────── 载入 / 函数区间 / 对象根
export function loadListing() { return fs.readFileSync(LST, 'utf8').split('\n'); }

export function functionSpans(lines) {
  const spans = [];
  let cur = null;
  for (let i = 0; i < lines.length; i += 1) {
    const p = PROC.exec(lines[i]);
    if (p) { cur = { sym: p[3], ea: Number.parseInt(p[2], 16), from: i, to: i }; spans.push(cur); continue; }
    if (cur && ENDP.test(lines[i])) { cur.to = i; cur = null; }
  }
  return spans;
}

/**
 * 对象根（Engine 指针）从哪个寄存器进来 —— 真源是 `.c` 侧的调用约定。
 * ★ `__fastcall` 也在这里：实测 `sub_40ED40` 是 `__fastcall`（`.c` 参数名 `a1`），
 *   但它逐字 `mov esi, ecx` 之后整篇 `[esi+5D8xxh]` ⇒ **工程上它就是 Engine**。
 *   名字（`this` vs `a1`）不在本单；本单只认"进来的那个对象指针"。
 */
export function objRootRegs() {
  const c = fs.readFileSync(CFILE, 'utf8');
  const map = new Map();
  // ★★ 踩过（子代理抓到并给了判据）：约定与函数名之间可能有**修饰符** ——
  //   `void __thiscall __noreturn sub_412290(int this)` ⇒ 旧正则 `__(conv)\s+(\S+?)\s*\(` 抓不到名字，
  //   受害符号 sub_412290 / sub_418E30 / sub_41A160（roots 表里一个都没有）⇒ 下游"0 访问"**假绿**。
  for (const m of c.matchAll(/__(thiscall|cdecl|stdcall|fastcall|usercall|userpurge)\s+((?:__\w+\s+)*)(\S+?)\s*\(([^)]*)/g)) {
    const [, conv, , sym, firstParam] = m;
    if (map.has(sym)) continue;
    if (conv === 'thiscall' || conv === 'fastcall') { map.set(sym, 'ecx'); continue; }
    if (conv === 'usercall' || conv === 'userpurge') {
      const g = /@<([a-z0-9]+)>/.exec(sym) ?? /@<([a-z0-9]+)>/.exec(firstParam ?? '');
      if (g) map.set(sym, g[1]);
    }
  }
  return map;
}

// ───────────────────────────────────────────────────────────── 全语料普查（报数）
export function census(lines, opts = {}) {
  const spans = functionSpans(lines);
  const roots = objRootRegs();
  const t = {
    functions: spans.length, objRootFunctions: 0,
    memSites: 0, memExplicitWidth: 0, memRegisterWidth: 0, memUnknownWidth: 0,
    memReads: 0, memWrites: 0, memRmw: 0, memLea: 0,
    absoluteSites: 0, unresolvedBase: 0, unresolvedIndex: 0, indexedSites: 0,
    indexedOnObjectRoot: 0, callSites: 0,
    linkSame: 0, linkEmbed: 0, linkMemberPtr: 0, linkTail: 0, linkOtherValue: 0, linkDefinedUnclassified: 0, linkNone: 0,
  };
  for (const sp of spans) {
    const rootReg = roots.get(sp.sym) ?? null;
    if (rootReg) t.objRootFunctions += 1;
    const { sites, links } = analyzeLines(lines, sp.from, sp.to, rootReg, opts);
    for (const s of sites) {
      t.memSites += 1;
      if (s.widthHow === 'explicit') t.memExplicitWidth += 1;
      else if (s.widthHow === 'register') t.memRegisterWidth += 1;
      else t.memUnknownWidth += 1;
      if (s.access === 'r') t.memReads += 1;
      else if (s.access === 'w') t.memWrites += 1;
      else if (s.access === 'rw') t.memRmw += 1;
      else t.memLea += 1;
      if (s.baseKind === 'absolute') t.absoluteSites += 1;
      if (s.problems.includes('base')) t.unresolvedBase += 1;
      if (s.problems.includes('index')) t.unresolvedIndex += 1;
      if (s.indexSym) { t.indexedSites += 1; if (s.absDisp !== null) t.indexedOnObjectRoot += 1; }
    }
    for (const l of links) {
      t.callSites += 1;
      if (l.cls === 'same') t.linkSame += 1;
      else if (l.cls === 'embed') t.linkEmbed += 1;
      else if (l.cls === 'memberptr') t.linkMemberPtr += 1;
      else if (l.cls === 'other-value') t.linkOtherValue += 1;
      else if (l.cls === 'defined-unclassified') t.linkDefinedUnclassified += 1;
      else t.linkNone += 1;
      if (l.tail) t.linkTail += 1;
    }
  }
  return t;
}

// ───────────────────────────────────────────────────────────── CLI
const fmtHex = (n) => (n === null || n === undefined ? '?' : `0x${(n >>> 0).toString(16)}`);

function ctx(sym, opts = {}) {
  const lines = loadListing();
  const spans = functionSpans(lines);
  const sp = spans.find((s) => s.sym === sym) ?? spans.find((s) => `0x${s.ea.toString(16)}` === sym.toLowerCase());
  if (!sp) { console.error(`找不到函数 ${sym}`); process.exitCode = 1; return null; }
  const rootReg = objRootRegs().get(sp.sym) ?? null;
  const { sites, links } = analyzeLines(lines, sp.from, sp.to, rootReg, opts);
  console.log(`# ${sp.sym} @0x${sp.ea.toString(16)}  行 ${sp.from + 1}-${sp.to + 1}  对象根寄存器=${rootReg ?? '（无：约定里没有对象指针）'}  调用点策略=${opts.killCallerSavedOnCall === false ? '校准档（ecx 跨调用保留）' : '保守档（call 杀 eax/ecx/edx）'}`);
  console.log('\n## site（非栈内存操作数）');
  for (const s of sites) {
    console.log(
      `  ${fmtHex(s.ea)}  ${s.mn.padEnd(6)} ${s.access}  w=${String(s.width).padEnd(7)} base=${s.baseKind.padEnd(10)} obj=${(s.obj ?? '?').padEnd(18)} off=${fmtHex(s.absDisp)}` +
      `${s.stride ? ` stride=${s.stride}` : ''}${s.indexSym ? ` idx=${s.indexSym}` : ''}` +
      `${o ? `   → ${o.origin}+${fmtHex(o.off)}` : ''}${s.problems.length ? `   ⚠ ${s.problems.join(',')}` : ''}`,
    );
    console.log(`           ${s.raw}`);
  }
  console.log('\n## link（调用点的对象根来源）');
  for (const l of links) console.log(`  ${fmtHex(l.ea)}  ${l.kind.padEnd(4)} ${l.target}  ${l.cls}${l.detail ? ` (${l.detail})` : ''}  ecx=${l.ecx}`);
  return { sp, sites, links };
}

if (process.argv[1]?.endsWith('assist.mjs')) {
  const argv = process.argv.slice(2);
  const cmd = argv[0];
  // ★ 两个**档**都要报：`--keep-ecx` = 校准档（实测本语料的 callee 事实上保留 ecx）
  const opts = argv.includes('--keep-ecx') ? { killCallerSavedOnCall: false } : {};
  const arg = argv.find((a) => !a.startsWith('--') && a !== cmd);
  if (cmd === 'ctx') ctx(arg, opts);
  else if (cmd === 'census') {
    const t0 = Date.now();
    const t = census(loadListing(), opts);
    console.log(JSON.stringify({ ...t, ms: Date.now() - t0 }, null, 2));
  } else console.log('用法：node assist.mjs ctx <sym> | census');
}
