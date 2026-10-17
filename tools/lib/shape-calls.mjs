/**
 * tools/lib/shape-calls.mjs —— **调用点的类型一致性**（跨函数；回答"两侧引用的是同一个类型吗"）
 *
 * 为什么要单独一层：`claims` 是**单模型、单函数**视角（载体 = 这个函数里的 ecx 是什么）。
 * "A 把 ecx 传给 B，B 说它的 ecx 是 U*" 这件事**跨函数、跨族文件**，没有任何单一模型能看见。
 *
 * 判据（全部机械）：
 *  ① 调用方：调用点上该寄存器的**来源**（`link.args`，由助手在不动点里算出）
 *     → 用 `types` 解析成类型：`this` ⇒ 调用方自己的载体类型；`member(path)` ⇒ 沿路径逐跳取 `kind:"ptr"` 的 `type`；
 *  ② 被调方：该函数在该槽位上的**载体声明**（`kind:"arg"` 的 `type`）；
 *  ③ 比：都已知 ⇒ same / **mismatch**；只有一侧 ⇒ **单侧**（这正是"另一侧缺声明"的探测器）。
 *
 * ⛔ 本层**不猜**：任一环节解析不出类型就是"给不出"，进"单侧"或"无话说"，⛔ 不许拿"偏移相同"顶替类型。
 * ⛔ 栈上传参（`push`）不在本版：那要先证明 ABI 的"参数位 ↔ push 顺序"映射。
 */
import { analyzeLines, functionSpans, objRootRegs } from './shape-access.mjs';

const num = (v) => (typeof v === 'number' ? v : Number.parseInt(String(v), 16));

/** 全局槽位索引：`${fn}|${reg}` → { type, id, kind }（来自所有族文件的 `kind:"arg"` 载体） */
export function carrierIndex(models) {
  const idx = new Map();
  const dup = [];
  for (const { file, model } of models) {
    for (const c of model.carriers ?? []) {
      if (c.kind !== 'arg' || !c.reg || !c.type) continue;
      const key = `${c.fn}|${c.reg.toLowerCase()}`;
      if (idx.has(key) && idx.get(key).type !== c.type) dup.push({ key, a: idx.get(key), b: { type: c.type, file } });
      else if (!idx.has(key)) idx.set(key, { type: c.type, id: c.id, file });
    }
  }
  return { idx, dup };
}

/** 沿路径把"来源"解析成类型 id（解析不出 ⇒ null） */
export function resolveProvType(prov, selfType, types) {
  if (!prov) return null;
  if (prov.k === 'this') return selfType;
  if (prov.k === 'interior') return prov.obj === 'this' ? selfType : null;
  if (prov.k === 'member') {
    const path = prov.path ?? [];
    if (!path.length) return null;
    let cur = selfType;
    for (const hop of path) {
      const t = types[cur];
      if (!t) return null;
      const m = (t.members ?? []).find((x) => num(x.off) === hop);
      if (!m || m.kind !== 'ptr' || !m.type) return null;
      cur = m.type;
    }
    return cur;
  }
  return null;
}

/** 逐调用点对账。`fns` = 要检查的调用方函数（通常是模型里的载体函数） */
export function auditCalls({ lines, fns, models, types, selfTypeOf, roots: rootsIn = null }) {
  const spanOf = new Map(functionSpans(lines).map((s) => [s.sym, s]));
  // ★ 对象根寄存器默认取真实 `.c` 的约定；注入是为了**合成用例**与非 `__thiscall` 场景
  const roots = rootsIn ?? objRootRegs();
  const { idx, dup } = carrierIndex(models);
  const rows = [];
  for (const fn of fns) {
    const sp = spanOf.get(fn);
    if (!sp) continue;
    const { links } = analyzeLines(lines, sp.from, sp.to, roots.get(fn) ?? null, {});
    const selfType = selfTypeOf(fn);
    for (const l of links) {
      if (l.kind !== 'call') continue;
      const callee = l.target;
      if (!spanOf.has(callee)) continue;              // 只对"有函数体的直接调用"（导入/间接调用另行点名）
      for (const [reg, prov] of Object.entries(l.args ?? {})) {
        const callerType = resolveProvType(prov, selfType, types);
        const calleeDecl = idx.get(`${callee}|${reg}`);
        const calleeType = calleeDecl?.type ? calleeDecl.type.replace(/\*$/, '') : null;
        const ct = callerType ? callerType.replace(/\*$/, '') : null;
        let verdict;
        if (ct && calleeType) verdict = ct === calleeType ? 'same' : 'mismatch';
        else if (ct && !calleeType) verdict = 'calleeUntyped';
        else if (!ct && calleeType) verdict = 'callerUntyped';
        else continue;                                  // 两边都没话说 ⇒ 不入账
        rows.push({
          caller: fn, callee, reg, ea: l.ea,
          callerType: ct, calleeType, verdict,
          callerProv: prov.k, path: prov.path ?? null,
          calleeFrom: calleeDecl?.file ?? null,
        });
      }
    }
  }
  return { rows, dup };
}

export function renderCalls({ rows, dup }, hex = (n) => `0x${n.toString(16)}`) {
  const L = [];
  const by = { same: 0, mismatch: 0, calleeUntyped: 0, callerUntyped: 0 };
  for (const r of rows) by[r.verdict] += 1;
  L.push('# 调用点类型对账（**跨函数**：调用方实参来源 → 被调方的载体声明）');
  L.push(`  槽位对账 ${rows.length} 对：✅ 一致 ${by.same} · ❌ **不一致 ${by.mismatch}**`
    + ` · ⚪ 被调方无类型 ${by.calleeUntyped} · ⚪ 调用方给不出 ${by.callerUntyped}`);
  const show = (k, title, n = 12) => {
    const xs = rows.filter((r) => r.verdict === k);
    if (!xs.length) return;
    L.push(`  ${title}：`);
    for (const r of xs.slice(0, n)) {
      L.push(`    ${hex(r.ea)} ${r.caller}→${r.callee} ${r.reg}: 调用方 ${r.callerType ?? '?'} / 被调方 ${r.calleeType ?? '—'}`
        + `（来源 ${r.callerProv}${r.path ? ` 路径 [${r.path.map((h) => `+${hex(h)}`).join(',')}]` : ''}）`);
    }
    if (xs.length > n) L.push(`    …（共 ${xs.length} 处）`);
  };
  show('mismatch', '❌ 不一致（两侧类型不同 ⇒ 至少一侧的声明是错的）');
  show('calleeUntyped', '⚪ 被调方没有类型（**声明缺口**：这些槽位值得补载体）');
  show('callerUntyped', '⚪ 调用方给不出（助手解不出该实参的来源）');
  if (dup.length) L.push(`  ⚠ 同一槽位有多份冲突声明：${dup.map((d) => `${d.key}(${d.a.type} vs ${d.b.type})`).join(' · ')}`);
  return L.join('\n');
}
