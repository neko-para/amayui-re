/**
 * tools/lib/shape-need.mjs —— **按需补齐**（`shape need <T>[@<off>]`）的纯逻辑：模型 + 观察 → 一份提案。
 *
 * ★ 这个模块回答一个很具体的问题：**"我现在要补这个类型，该往哪写、写什么"**。
 *   三样东西合在一起才有用：① 谁该当载体（`propagate` 已推断、尚无载体，带**调用点 EA**）·
 *   ② 哪些偏移还没声明（观测到的偏移 − 已声明窗口）· ③ 一份能直接粘贴的草稿。
 *
 * ⛔ 三条边界（都是本仓的既有纪律，不是新规矩）：
 *   · **不猜**：草稿里的成员只来自**观测到的偏移**；宽度是观测值（不一致时如实列出，⛔ 不选一个充数），
 *     宽度没观察到的偏移**不进草稿**（否则会写出一条"看起来成立"的假成员）；
 *   · **不写盘**：本域没有任何写操作，草稿只打到 stdout，落盘由人决定；
 *   · **不判类型**：候选只来自 `propagate` 的调用链推断，⛔ 不按偏移命中率猜。
 *
 * 草稿分两份（架构要求）：`members` 进 `types.json`（类型真源），`carriers` 进 `carriers.json`
 * —— ⛔ 不把 `types` 写进族文件。
 */

const num = (v) => Number.parseInt(String(v), 16);
const hex = (n) => `0x${(n >>> 0).toString(16)}`;

/** 类型的成员窗口（闭集合：内联值 / 数组 / 条数未观察）+ 洞不写 */
export function memberWindows(types, type) {
  const T = types?.[type];
  if (!T) return null;
  const closed = [];
  const open = [];
  for (const m of T.members ?? []) {
    const from = num(m.off);
    if (m.width) { closed.push({ m, from, to: from + m.width }); continue; }
    if (m.type && m.count != null) {
      const size = types[m.type]?.size;
      if (!Number.isFinite(size)) { open.push({ m, from, to: Infinity }); continue; }
      closed.push({ m, from, to: from + size * m.count });
      continue;
    }
    if (m.count === null) { open.push({ m, from, to: Infinity }); continue; }
    closed.push({ m, from, to: from + 1 });   // G8 会为这种声明判红；这里按 1 字节槽算窗口
  }
  return { closed, open };
}

/** 偏移落在哪个已声明成员里（没有 ⇒ null） */
export function memberAt(windows, off) {
  if (!windows) return null;
  for (const w of windows.closed) if (off >= w.from && off < w.to) return w;
  for (const w of windows.open) if (off >= w.from) return w;
  return null;
}

/**
 * 提案本体（纯函数；不读语料、不写盘）。
 *
 * @param {object} p
 * @param {object} p.types      已解析的类型表
 * @param {object[]} p.carriers 载体清单（只用来判"哪些函数已经有载体"）
 * @param {Map} p.inferred      `propagate()` 的 inferred（fn → {type, viaEa, viaFrom, hops}）
 * @param {object[]} p.sites    `[{fn, ea, off, width, stride?}]` —— **位移可用**的 obj0 相对站点
 * @param {string} p.type       目标类型 id
 * @param {number|null} p.off   只看这个偏移（`T@0x10`）；null = 全类型
 */
export function needProposal({ types, carriers = [], inferred = new Map(), sites = [], type, off = null, limit = 200 }) {
  const T = types?.[type];
  if (!T) throw new Error(`types 里没有类型 ${type}（⛔ 类型名写错会让整份提案变成空表）`);
  const windows = memberWindows(types, type);
  const declared = carriers
    .filter((c) => c.fn && String(c.type ?? '').replace(/\*$/, '') === type)
    .map((c) => ({ id: c.id ?? null, fn: c.fn, reg: c.reg ?? null, kind: c.kind ?? null, witness: c.witness ?? [] }))
    .sort((a, b) => String(a.fn).localeCompare(String(b.fn)));
  const declaredFns = new Set(declared.map((c) => c.fn));
  const candidates = [...inferred.entries()]
    .filter(([fn, v]) => v.type === type && !declaredFns.has(fn))
    .map(([fn, v]) => ({ fn, viaEa: v.viaEa ?? null, viaFrom: v.viaFrom ?? null, hops: v.hops ?? null }))
    .sort((a, b) => a.fn.localeCompare(b.fn));

  // 观测偏移（按偏移归组：宽度的**不一致**要如实留着，⛔ 不选一个充数）
  const byOff = new Map();
  for (const s of sites) {
    if (typeof s.off !== 'number') continue;
    if (!byOff.has(s.off)) byOff.set(s.off, { off: s.off, widths: new Set(), strides: new Set(), eas: new Set(), fns: new Set() });
    const e = byOff.get(s.off);
    if (s.width !== undefined && s.width !== null && s.width !== 'unknown') e.widths.add(Number(s.width));
    if (s.stride) e.strides.add(Number(s.stride));
    e.eas.add(s.ea);
    e.fns.add(s.fn);
  }
  const observed = [...byOff.values()]
    .sort((a, b) => a.off - b.off)
    .map((e) => ({ off: e.off, widths: [...e.widths].sort((a, b) => a - b), strides: [...e.strides].sort((a, b) => a - b), eas: [...e.eas].sort((a, b) => a - b), fns: [...e.fns].sort() }));

  const focusWin = off != null ? memberAt(windows, off) : null;
  const target = off != null ? observed.filter((e) => e.off === off) : observed;
  const undeclared = target.filter((e) => !memberAt(windows, e.off));
  // 宽度没观察到的偏移**不进草稿**（⛔ 不许编宽度），单列出来让人读
  const noWidth = undeclared.filter((e) => e.widths.length === 0);
  const withWidth = undeclared.filter((e) => e.widths.length > 0);

  const members = withWidth.slice(0, limit).map((e) => {
    const w = Math.max(...e.widths);
    const m = { off: hex(e.off), width: w };
    if (e.widths.length > 1) m.note = `观测宽度不一致：${e.widths.join(' / ')}（取最大，⛔ 需人读确认）`;
    if (e.strides.length === 1 && e.strides[0] !== w) m.note = `${m.note ? `${m.note}；` : ''}观测步长 ${e.strides[0]}（**可能是数组**，本稿按标量写）`;
    return m;
  });
  const stub = {
    members,
    carriers: candidates.slice(0, limit).map((c, i) => ({
      id: `n${String(i + 1).padStart(2, '0')}`,
      kind: 'arg',
      fn: c.fn,
      reg: 'ecx',
      type: `${type}*`,
      ...(c.viaEa != null ? { witness: [hex(c.viaEa)] } : {}),
    })),
  };
  if (!stub.carriers.length) delete stub.carriers;
  if (!stub.members.length) delete stub.members;

  return {
    type, size: Number.isFinite(T.size) ? T.size : null, off,
    declaredAt: focusWin ? { off: focusWin.from, to: focusWin.to === Infinity ? null : focusWin.to, member: focusWin.m } : null,
    declared, candidates, observed, undeclared, noWidth, withWidth, stub,
    counts: {
      members: (T.members ?? []).length, declaredCarriers: declared.length, candidates: candidates.length,
      observedOffsets: observed.length, observedSites: sites.length, undeclared: undeclared.length,
    },
  };
}

/** 终端渲染（提案是**草稿**，不是判定） */
export function renderNeed(p, { limit = 40 } = {}) {
  const c = p.counts;
  const L = [];
  L.push(`# shape need ${p.type}${p.off != null ? `@${hex(p.off)}` : ''}${p.size != null ? `（size ${p.size}）` : '（size 未知）'}`);
  L.push(`  已声明成员 ${c.members} · 已有载体 ${c.declaredCarriers} · 传播候选 ${c.candidates} · 观测 ${c.observedOffsets} 个偏移 / ${c.observedSites} 处站点 · **未声明 ${c.undeclared}**`);
  if (p.off != null && p.declaredAt) {
    L.push('');
    L.push(`## 该偏移**已声明**`);
    L.push(`  ${hex(p.declaredAt.off)}..${p.declaredAt.to === null ? '∞' : hex(p.declaredAt.to)} ⇒ 成员 ${hex(num(p.declaredAt.member.off))}（${p.declaredAt.member.width ? `w${p.declaredAt.member.width}` : p.declaredAt.member.type ? `${p.declaredAt.member.type}×${p.declaredAt.member.count ?? '?'}` : '?'}）`);
    L.push('  ⇒ 不用补；若这是"spec 指向它却解析不到"，先查那条 storage 的类型名 / 偏移。');
    return `${L.join('\n')}\n`;
  }
  L.push('');
  L.push(`## 已有载体（${p.declared.length}）`);
  if (!p.declared.length) L.push('  （无）');
  for (const d of p.declared.slice(0, limit)) L.push(`  ${d.id ?? '-'}  ${d.fn}  ${d.reg ?? '-'}　witness ${(d.witness ?? []).join(' ') || '（无）'}`);
  L.push('');
  L.push(`## 候选载体（${p.candidates.length}）—— witness = **调用点 EA**（G16 要的就是它；⛔ 不许按"看起来像"编）`);
  if (!p.candidates.length) L.push('  （无：传播没给这个类型推出新函数 ⇒ 要么已有载体够用，要么它是参数/返回值来的，需要 local 载体）');
  for (const [i, x] of p.candidates.slice(0, limit).entries()) {
    L.push(`  n${String(i + 1).padStart(2, '0')}  ${x.fn}  ← ${x.viaFrom ?? '?'}${x.viaEa != null ? `@${hex(x.viaEa)}` : ''}${x.hops != null ? `（${x.hops} 跳）` : ''}`);
  }
  L.push('');
  L.push('## 未声明偏移（宽度是**观测值**；步长提示 ≠ "这是数组"）');
  if (!p.undeclared.length) L.push('  （无：观测到的偏移都落在已声明窗口里）');
  for (const e of p.undeclared.slice(0, limit)) {
    L.push(`  ${hex(e.off).padEnd(10)} ${String(e.eas.length).padStart(3)} 处 · ${e.widths.length ? `w${e.widths.join('/')}` : '**宽度未观察**'}${e.strides.length ? ` · 步长 ${e.strides.join('/')}` : ''} · EA ${e.eas.slice(0, 4).map(hex).join(' ')}${e.eas.length > 4 ? ' …' : ''}`);
  }
  if (p.undeclared.length > limit) L.push(`  …（另有 ${p.undeclared.length - limit} 个；--json 取全量）`);
  if (p.noWidth.length) L.push(`  ⛔ 其中 ${p.noWidth.length} 个偏移**没有宽度观察** ⇒ 不进草稿（先读站点确认宽度，⛔ 不许按 4 猜）`);
  L.push('');
  L.push('## 草稿① → `data/shape/types.json` 的 `types.' + p.type + '.members`（**追加**）');
  L.push(JSON.stringify(p.stub.members ?? [], null, 2));
  L.push('');
  L.push('## 草稿② → `data/shape/carriers.json` 的 `carriers`（**追加**）');
  L.push(JSON.stringify(p.stub.carriers ?? [], null, 2));
  L.push('');
  L.push('★ 本命令**不落盘**（本域没有写操作）：粘贴后跑 `pnpm tools shape claims data/shape/types.json` 与');
  L.push('  `pnpm tools shape claims data/shape/carriers.json` —— G1/G2/G4/G8/G16 才是最后一道闸。');
  return `${L.join('\n')}\n`;
}
