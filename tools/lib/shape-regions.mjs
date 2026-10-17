/**
 * tools/lib/shape-regions.mjs —— **校验器功能①**：机械导出拆分范围（段落）
 *
 * ★ 段落划分**不是模型给的**，是这里算出来的；模型只能**引用**它。
 * 判据（全在行序上，不需要 CFG）：
 *   · 一处 `init` 形态（`xor r,r` / `mov r,imm`）**开启**一个新段落；
 *   · 段内**形态类别必须单一**（`const` / `ptr` / `load` / `bit`）；类别一变就切；
 *   · 退出路径上的 `pop r`（后面紧跟收尾）不算段落；
 *   · caller-saved（`eax/ecx/edx`）的段落里出现 `call` ⇒ 标 `brokenByCall`（连续性无依据）。
 */
import { loadListing, functionSpans } from './shape-access.mjs';

const WRITE_MN = new Set(['mov', 'movzx', 'movsx', 'lea', 'add', 'sub', 'adc', 'sbb', 'and', 'or', 'xor', 'shl', 'shr', 'sar', 'sal', 'rol', 'ror', 'imul', 'pop', 'inc', 'dec', 'neg', 'not']);
const CALLER_SAVED = new Set(['eax', 'ecx', 'edx']);

function forms(reg) {
  return {
    init: new RegExp(`^(xor\\s+${reg},\\s*${reg}|mov\\s+${reg},\\s*(0x)?[0-9a-f]+h?)$`, 'i'),
    const: new RegExp(`^(inc\\s+${reg}|dec\\s+${reg}|add\\s+${reg},\\s*(0x)?[0-9a-f]+h?|sub\\s+${reg},\\s*(0x)?[0-9a-f]+h?)$`, 'i'),
    ptr: new RegExp(`^lea\\s+${reg},`, 'i'),
    load: new RegExp(`^mov(?:zx|sx)?\\s+${reg},\\s*\\[`, 'i'),
    bit: new RegExp(`^(?:and|or|xor)\\s+${reg},\\s*`, 'i'),
    restore: new RegExp(`^pop\\s+${reg}$`, 'i'),
  };
}

/** 一条指令的类别（不是这个寄存器的写点 ⇒ null） */
export function classify(ins, reg) {
  const dst = (ins.ops[0] ?? '').trim().toLowerCase();
  if (dst !== reg || !WRITE_MN.has(ins.mn)) return null;
  const body = `${ins.mn} ${ins.ops.join(', ')}`;
  const F = forms(reg);
  if (F.restore.test(body)) return 'restore';
  if (F.init.test(body)) return 'init';
  if (F.const.test(body)) return 'const';
  if (F.ptr.test(body)) return 'ptr';
  if (F.load.test(body)) return 'load';
  if (F.bit.test(body)) return 'bit';
  return 'other';
}

/** 函数体的指令表 */
export function insnsOf(lines, sym) {
  const sp = functionSpans(lines).find((s) => s.sym === sym);
  if (!sp) return null;
  const out = [];
  for (let i = sp.from; i <= sp.to; i += 1) {
    const m = /^[A-Za-z_.][\w.]*:([0-9A-Fa-f]{8})\s+([a-z][a-z0-9]*)\s*(.*)$/.exec(lines[i]);
    if (!m) continue;
    const rest = m[3].replace(/\s+;.*$/, '').trim();
    out.push({ i, ea: Number.parseInt(m[1], 16), mn: m[2], ops: rest ? rest.split(',').map((x) => x.trim()) : [], text: lines[i].trimEnd() });
  }
  return { sp, out };
}

/** 退出路径上的 `pop r`（后面 ≤5 条内出现收尾） */
function isExitRestore(insns, k) {
  for (let j = k + 1; j <= Math.min(k + 5, insns.length - 1); j += 1) {
    const b = `${insns[j].mn} ${insns[j].ops.join(', ')}`.trim();
    if (/^mov\s+esp,\s*ebp$/i.test(b) || /^leave$/i.test(b) || /^retn?\b/i.test(b)) return true;
  }
  return false;
}

/** 机械导出：某寄存器的段落（**唯一的真源**；模型只能引用） */
export function deriveRegions(lines, sym, reg) {
  const f = insnsOf(lines, sym);
  if (!f) return null;
  const writes = [];
  for (let k = 0; k < f.out.length; k += 1) {
    const kind = classify(f.out[k], reg);
    if (!kind) continue;
    if (kind === 'restore' && isExitRestore(f.out, k)) continue; // 退出路径不算段落
    writes.push({ k, ea: f.out[k].ea, line: f.out[k].i + 1, kind, text: f.out[k].text.replace(/^\S+:\S+\s+/, '') });
  }
  const regions = [];
  for (const w of writes) {
    const cat = w.kind === 'init' ? 'const' : w.kind;
    const cur = regions[regions.length - 1];
    if (!cur || w.kind === 'init' || cur.cat !== cat) regions.push({ cat, writes: [w] });
    else cur.writes.push(w);
  }
  for (const [i, r] of regions.entries()) {
    r.from = r.writes[0].ea;
    r.to = r.writes[r.writes.length - 1].ea;
    r.lines = [r.writes[0].line, r.writes[r.writes.length - 1].line];
    r.openedByInit = r.writes[0].kind === 'init';
    r.allConstOrInit = r.writes.every((w) => w.kind === 'init' || w.kind === 'const');
    // ★ 段落**活着的窗口** = 本段第一处写点 → 下一段第一处写点（或函数尾）。
    //   caller-saved 的寄存器只要在这个窗口里被 `call` 碰过，连续性就没有依据。
    //   实测踩过：只数到"本段最后一处写点"为止，会漏掉"写完 → call → 再用"这种形态（该标却没标）。
    const next = regions[i + 1];
    const winEnd = next ? next.writes[0].k - 1 : f.out.length - 1;
    r.brokenByCall = CALLER_SAVED.has(reg) && f.out.some((x, k) => x.mn === 'call' && k >= r.writes[0].k && k <= winEnd);
    // ★ 段落的**活着的窗口**（机械导出，供"局部类型载体"引用）：本段第一处写点 → 下一段写点前的最后一条指令。
    //   为什么要它：`regions` 的 `[from, to]` 只框住**写点**，而这个寄存器在 `to` 之后、下一次写之前**仍然持有那个值**
    //   （实测：`sub_421450` 的 `edi` 段 `[0x421544..0x421576]`，可 `[edi+30h]` 的用法落在 `0x421588`——在 `to` 之后）。
    r.liveTo = f.out[winEnd]?.ea ?? r.to;
  }
  return { f, writes, regions };
}

