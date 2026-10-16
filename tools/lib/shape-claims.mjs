/**
 * tools/lib/shape-claims.mjs —— **校验器功能②**：给定模型产出，检查汇编是否匹配
 *
 * ## 契约（模型只写判断；能从 EA + 语料机械读出的东西一律不进产物）
 * 模型文件（\`amayui.claims/1\`）的分节：
 *   \`types\`（**按类型 id 共享一份**：每条成员 = 偏移 + 宽度/类型+count；洞不写）
 *   \`carriers\`（机器槽 → 类型：\`arg\`(reg/栈偏移) / \`local\` / \`global\` —— **字段不是载体**，它是类型的成员）
 *   \`roles\`（段落角色：\`span\` **必须逐字等于** \`shape-regions\` 导出的一个段落）
 *   \`domains\` / \`escapes\` / \`identities\`（可选；本版只做结构检查）
 *   \`typesFrom\`（可选：引用另一个 claims 文件共享类型；**同名类型按成员增量合并**）
 *
 * ## 两道守卫（把"不耦合业务"与"不写转录"变成机械判据）
 * * **词汇守卫**：出现 \`opcode\` / \`argc\` / \`operand\` / \`lengthWord\` … ⇒ 红（那是 B 层：\`data/ledger/\`）
 * * **冗余守卫**：出现 \`mn\` / \`defines\` / \`callee\` / \`const\` … 这些字段名 ⇒ 红（能从 EA 读出）
 *
 * ## 义务（红了都要有理由；⛔ 没有一档是"静默通过"）
 * G1 类型自洽（不重叠 / 不越 size）· G2 覆盖（两级：容器的成员 + **数组成员的元素内成员**）
 * G3 步长（数组形态：观测步长 == size(元素类型)）· G4 数组边界（末尾地址必须在语料里被引用）
 * G5/G5' **二级对象下钻**（宿主是指针 + 声明了目标类型 ⇒ 用目标类型的成员表再跑一遍；
 *   目标类型为 null ⇒ **不透明桶**单列；宿主没声明 ptr ⇒ 红）
 * G6 未观察成员（用**形状层观察集**判定，它能解折叠形态；字面量扫描只是下界）
 * J3 角色（span == 导出段落 且 段内全 ±const/init）· 守恒①（obj0 访问）· 守恒②（二级对象三档和）
 * **超出解析深度**：**对象层级 ≥3** 的访问单列（判定器只下钻到层级 2）—— ⛔ 不许静默丢
 *
 * 真源：\`.lst\`（算术 + 锚）· \`.c\`（调用约定 / 结构第二意见）。⛔ 不读 \`apps/**\`（同层产物）。
 */
import fs from 'node:fs';
import path from 'node:path';

import { analyzeLines, functionSpans, loadListing, objRootRegs } from './shape-access.mjs';
import { deriveRegions } from './shape-regions.mjs';

/** 词汇守卫：**引擎 / 业务概念词**（B 层的东西混进汇编层 ⇒ 红） */
export const VOCAB = ['opcode', 'argc', 'operand', 'lengthWord', 'op1', 'op2', '操作数', '长度字'];
/** 冗余守卫：这些是**能从 EA + 语料机械读出**的字段名 ⇒ 不许当字段出现 */
export const REDUNDANT = ['mn', 'defines', 'callee', 'argImm', 'text', 'line', 'const'];

const num = (v) => (typeof v === 'number' ? v : Number.parseInt(String(v), 16));
const hex = (n) => (typeof n === 'number' ? `0x${n.toString(16)}` : String(n));
const pad = (s, n) => String(s).padEnd(n);

/**
 * **对象层级**（level）：1 = 对象就是 obj0；2 = 对象是"obj0 的某个成员里读出的指针"；3 = 再一跳……
 * ★ 踩过：`baseHop` 是**基址值的来路**（1 = 它的来源是 obj0 的成员），**对象层级 = baseHop + 1**。
 *   把两者混为一谈，"层级 3"整类（实测 812 处）就会漏出"超出解析深度"桶 ——
 *   而上限冻结在层级 2 时，那一类**正是必须被发现的东西**。
 */
export const objectLevel = (s) => (typeof s.pointeeHost === 'number' ? (s.baseHop ?? 1) + 1 : 1);
/** 下钻上限：**层级 2**（只下钻一层）。层级 ≥3 一律进"超出解析深度"桶，带 EA，⛔ 不静默丢 */
export const DRILL_MAX_LEVEL = 2;

/** 内置样本：13 个函数的载体 + 两个共享类型（**数据即代码**，不新增入库 JSON ⇒ 不欠同名 .md） */
export function sampleModel() {
  const fns = [
    'sub_42C5E0', 'sub_42C620', 'sub_42CA00', 'sub_42CEC0', 'sub_430A50',
    'sub_42C660', 'sub_42C750', 'sub_42C790',
    'sub_42C6A0', 'sub_42C6E0', 'sub_42C720',
    'sub_42CA50', 'sub_40ED40',
  ];
  return {
    schema: 'amayui.claims/1',
    obj0: { isGlobal: true },
    types: {
      T0: {
        size: null,
        complete: false,
        members: [
          { off: '0x5d880', width: 4 }, { off: '0x5d884', width: 4 }, { off: '0x5d888', width: 4 },
          { off: '0x5d88c', width: 4 }, { off: '0x5d890', width: 4 },
          { off: '0x5d894', type: 'T1', count: 40 },
        ],
      },
      T1: {
        size: 120,
        complete: true,
        members: Array.from({ length: 30 }, (_, i) => ({ off: `0x${(i * 4).toString(16).padStart(2, '0')}`, width: 4 })),
      },
    },
    carriers: fns.map((fn, i) => ({ id: `c${String(i + 1).padStart(2, '0')}`, kind: 'arg', fn, reg: 'ecx', type: 'T0*' })),
  };
}

/** 词汇守卫：概念词（键与值都查）。返回违规词列表（空 = 通过） */
export function vocabViolations(model) {
  const text = JSON.stringify(model);
  return VOCAB.filter((w) => new RegExp(w, 'i').test(text));
}

/** 冗余守卫：字段名。返回违规字段名列表（空 = 通过） */
export function redundantKeys(model) {
  const keys = new Set();
  (function walk(o) {
    if (Array.isArray(o)) o.forEach(walk);
    else if (o && typeof o === 'object') for (const [k, v] of Object.entries(o)) { keys.add(k); walk(v); }
  })(model);
  return REDUNDANT.filter((k) => keys.has(k));
}

/** `typesFrom` 解析：同名类型按**成员**合并（同 off 以本文件为准）—— 增量补成员不该逼人抄整张表 */
export function resolveTypes(model, baseDir) {
  if (!model.typesFrom) return model;
  const ref = JSON.parse(fs.readFileSync(path.join(baseDir, model.typesFrom), 'utf8'));
  const ids = [...new Set([...Object.keys(ref.types ?? {}), ...Object.keys(model.types ?? {})])];
  const types = Object.fromEntries(ids.map((k) => {
    const a = ref.types?.[k] ?? {};
    const b = model.types?.[k] ?? {};
    const byOff = new Map((a.members ?? []).map((m) => [String(m.off), m]));
    for (const m of b.members ?? []) byOff.set(String(m.off), m);
    return [k, { ...a, ...b, members: [...byOff.values()] }];
  }));
  return { ...ref, ...model, types, obj0: model.obj0 ?? ref.obj0 };
}

/**
 * 跑一遍全部义务。
 * @param {object} model  claims 对象（已解析 typesFrom 与否都行 —— 传 baseDir 即自行解析）
 * @param {{lines?: string[], baseDir?: string, observed?: Set<number>}} opts
 * @returns {{findings: object[], notes: string[], perCarrier: object[], cons: object}}
 */
export function checkClaims(model0, { lines = loadListing(), baseDir = process.cwd(), observed = null } = {}) {
  const findings = [];
  const notes = [];
  const red = (where, why) => findings.push({ where, why });

  for (const w of vocabViolations(model0)) red('守卫·词汇', `出现业务词「${w}」—— 汇编层不许有它（那是 data/ledger/ 的事）`);
  for (const k of redundantKeys(model0)) red('守卫·冗余', `字段名「${k}」是能从 EA + 语料机械读出的转录 ⇒ 不许进产物`);

  const M = resolveTypes(model0, baseDir);
  const types = M.types ?? {};

  // ── 语料侧的两个 oracle
  const occurrences = new Map();
  for (const m of lines.join('\n').matchAll(/\b([0-9A-F]{4,8})h\b/gi)) {
    const v = Number.parseInt(m[1], 16);
    occurrences.set(v, (occurrences.get(v) ?? 0) + 1);
  }
  const refs = (abs) => occurrences.get(abs) ?? 0;
  const spans = functionSpans(lines);
  const roots = objRootRegs();
  /** 观察集（形状层）：能解折叠形态 ⇒ G6 的正确判据；**必须显式传入**（本模块不写缓存文件） */
  const OBS = observed;
  if (!OBS) notes.push('G6：未传观察集（observed）⇒ 只用字面量出现次数，**折叠形态会被误报未观察**（下界）');

  const memberExtent = (t, mem) => {
    if (mem.type && mem.count) {
      const et = types[mem.type];
      return et?.size != null ? et.size * mem.count : null;
    }
    return mem.width != null ? mem.width : null;
  };
  const membersOf = (t) => (t?.members ?? []).map((m) => ({ ...m, offN: num(m.off), ext: memberExtent(t, m) })).sort((a, b) => a.offN - b.offN);
  const membersOfId = (tid) => membersOf(types[tid]);

  // ── G1 类型自洽
  for (const [tid, t] of Object.entries(types)) {
    const ms = membersOf(t);
    for (let i = 1; i < ms.length; i += 1) {
      const prev = ms[i - 1];
      if (prev.ext == null) continue;
      if (ms[i].offN < prev.offN + prev.ext) red(`G1 ${tid}`, `成员 ${hex(ms[i].offN)} 与 ${hex(prev.offN)}(+${prev.ext}) 重叠`);
    }
    if (t.size != null) for (const m of ms) if (m.ext != null && m.offN + m.ext > t.size) red(`G1 ${tid}`, `成员 ${hex(m.offN)}+${m.ext} 超出 size ${t.size}`);
  }

  /** 成员的**绝对地址**（元素 0）：obj0 是全局单例时位移即地址；经数组成员到达 ⇒ 数组基址 + 元素内偏移 */
  const ABS = new Map();
  if (M.obj0?.isGlobal) {
    const walk = (tid, base) => {
      for (const m of membersOfId(tid)) {
        const abs = base + m.offN;
        ABS.set(`${tid}:${m.offN}`, abs);
        if (m.type && m.count) walk(m.type, abs);
      }
    };
    for (const tid of Object.keys(types)) if ((M.carriers ?? []).some((c) => c.type.replace(/\*|\[\]$/, '') === tid)) walk(tid, 0);
  }

  // ── 逐载体
  const perCarrier = [];
  const cons = { obj0Acc: 0, obj0Explained: 0, obj0Residue: 0, pointee: 0, pointeeOk: 0, pointeeOpaque: 0, pointeeBad: 0, deep: 0 };
  for (const c of M.carriers ?? []) {
    const tid = c.type.replace(/\*|\[\]$/, '');
    const t = types[tid];
    if (!t) { red(`载体 ${c.id}`, `类型 ${c.type} 未定义`); continue; }
    const sp = spans.find((s) => s.sym === c.fn);
    if (!sp) { red(`载体 ${c.id}`, `找不到函数 ${c.fn}`); continue; }
    const conv = roots.get(c.fn) ?? null;
    if (!conv) { red(`载体 ${c.id}`, `${c.fn} 的 .c 约定里查不到对象根寄存器 ⇒ 这条载体**无法检查**（不许当绿）`); continue; }
    if (c.reg !== conv) { red(`载体 ${c.id}`, `声明的载体寄存器 ${c.reg} 与 .c 约定（${conv}）不符`); continue; }

    const { sites } = analyzeLines(lines, sp.from, sp.to, conv, {});
    const isPartialLea = (s) => s.access === 'a' && !!s.indexSym;
    const obj0 = sites.filter((s) => s.obj === 'Engine' && s.absDisp !== null && !isPartialLea(s));
    const partial = sites.filter((s) => s.obj === 'Engine' && s.absDisp !== null && isPartialLea(s));
    const ms = membersOf(t);
    const explained = [];
    const residue = [];
    for (const s of obj0) {
      const host = ms.find((m) => s.absDisp >= m.offN && (m.ext == null ? s.absDisp < m.offN + 1 : s.absDisp < m.offN + m.ext));
      if (!host) {
        if (t.complete) red(`G2 ${c.id} @${hex(s.ea)}`, `访问 ${hex(s.absDisp)} 不落在 ${c.type} 的任何已声明成员上（且该类型声明为 complete）`);
        else residue.push({ ea: s.ea, abs: s.absDisp, kind: 'obj0', why: `访问 ${hex(s.absDisp)} 不落在 ${c.type} 的任何已声明成员上` });
        continue;
      }
      let leaf = host;
      if (host.type && host.count) {              // ★ 两层级：数组 ⇒ 还要落到元素类型的成员上
        const et = types[host.type];
        const elemOff = s.absDisp - host.offN;
        const em = membersOf(et).find((m) => elemOff >= m.offN && (m.ext == null ? elemOff < m.offN + 1 : elemOff < m.offN + m.ext));
        if (!em) {
          if (et?.complete) red(`G2 ${c.id} @${hex(s.ea)}`, `元素内偏移 ${hex(elemOff)} 不是 ${host.type} 的成员（${host.type} 声明为 complete）`);
          else residue.push({ ea: s.ea, abs: s.absDisp, kind: 'obj0', why: `元素内偏移 ${hex(elemOff)} 不是 ${host.type} 的成员` });
          continue;
        }
        leaf = em;
        if (s.indexSym && et?.size != null && s.stride !== et.size) { red(`G3 ${c.id} @${hex(s.ea)}`, `数组形态步长 ${s.stride} != size(${host.type})=${et.size}`); continue; }
      }
      if (leaf.width != null && s.width !== 'unknown' && s.width !== leaf.width) { red(`G2 ${c.id} @${hex(s.ea)}`, `宽度 ${s.width} != 成员声明 ${leaf.width}`); continue; }
      explained.push({ ea: s.ea, abs: s.absDisp, member: host, leaf });
    }

    // ── G5/G5'：二级对象三档（解释 / 不透明 / 拒绝）
    const pointee = sites.filter((s) => typeof s.pointeeHost === 'number' && typeof s.pointeeOff === 'number');
    let pointeeOk = 0; let pointeeOpaque = 0; let pointeeBad = 0;
    const opaqueEAs = [];
    for (const s of pointee) {
      const host = ms.find((m) => m.offN === s.pointeeHost);
      if (!host) {
        pointeeBad += 1;
        if (t.complete) red(`G5 ${c.id} @${hex(s.ea)}`, `二级对象的宿主 ${hex(s.pointeeHost)} 不是 ${c.type} 的成员`);
        else residue.push({ ea: s.ea, abs: s.pointeeHost, kind: 'pointee', why: `二级对象宿主 ${hex(s.pointeeHost)} 未声明为成员` });
        continue;
      }
      if (host.kind !== 'ptr') { pointeeBad += 1; red(`G5' ${c.id} @${hex(s.ea)}`, `宿主 ${hex(host.offN)} 被当指针用（读它的值再以之为基址），但成员声明里没有 kind:"ptr"`); continue; }
      if (!host.type) { pointeeOpaque += 1; opaqueEAs.push(s.ea); continue; }   // ★ 不透明：明确记账，不算通过
      const tu = types[host.type];
      if (!tu) { pointeeBad += 1; red(`G5' ${c.id} @${hex(s.ea)}`, `宿主声明的目标类型 ${host.type} 未定义`); continue; }
      const inner = membersOf(tu).find((m) => s.pointeeOff >= m.offN && (m.ext == null ? s.pointeeOff < m.offN + 1 : s.pointeeOff < m.offN + m.ext));
      if (!inner) {
        pointeeBad += 1;
        if (tu.complete) red(`G5' ${c.id} @${hex(s.ea)}`, `二级偏移 ${hex(s.pointeeOff)} 不是 ${host.type} 的成员（该类型 complete）`);
        else residue.push({ ea: s.ea, abs: s.pointeeHost, kind: 'pointee', why: `二级偏移 ${hex(s.pointeeOff)} 不是 ${host.type} 的成员` });
        continue;
      }
      if (inner.width != null && s.width !== 'unknown' && s.width !== inner.width) { pointeeBad += 1; red(`G5' ${c.id} @${hex(s.ea)}`, `二级宽度 ${s.width} != ${host.type} 成员 ${hex(inner.offN)} 声明 ${inner.width}`); continue; }
      if (tu.size != null && s.pointeeOff + (s.width === 'unknown' ? 1 : s.width) > tu.size) { pointeeBad += 1; red(`G5' ${c.id} @${hex(s.ea)}`, `二级偏移 ${hex(s.pointeeOff)}+${s.width} 超出 ${host.type}.size ${tu.size}`); continue; }
      pointeeOk += 1;
    }
    // ★ 超出解析深度：**对象层级 ≥3**（上限冻结在 2）—— 单列 + 打 EA，⛔ 不许静默丢
    const deep = sites.filter((s) => objectLevel(s) > DRILL_MAX_LEVEL);

    cons.obj0Acc += obj0.length;
    cons.obj0Explained += explained.length;
    cons.obj0Residue += residue.filter((r) => r.kind !== 'pointee').length;
    cons.pointee += pointeeOk + pointeeBad + pointeeOpaque;
    cons.pointeeOk += pointeeOk;
    cons.pointeeOpaque += pointeeOpaque;
    cons.pointeeBad += pointeeBad;
    cons.deep += deep.length;
    perCarrier.push({ c, explained, residue, total: obj0.length, pointee: pointeeOk + pointeeBad + pointeeOpaque, pointeeOk, pointeeOpaque, deep: deep.length, deepEAs: deep.map((s) => s.ea), opaqueEAs, partial: partial.length, partialEAs: partial.map((s) => s.ea) });
  }

  // ── G4 数组边界（末尾地址必须在语料里被引用）
  for (const [tid, t] of Object.entries(types)) {
    for (const m of membersOf(t)) {
      if (!(m.type && m.count)) continue;
      const et = types[m.type];
      if (et?.size == null) { notes.push(`G4 ${tid}+${hex(m.offN)}: 元素 size 未知 ⇒ 边界判不了`); continue; }
      const end = m.offN + et.size * m.count;
      if (refs(end) === 0) red(`G4 ${tid}+${hex(m.offN)}`, `数组末尾 ${hex(end)} 在语料里零引用 ⇒ count=${m.count} 无依据`);
      else notes.push(`G4 ${tid}+${hex(m.offN)}: 末尾 ${hex(end)} 有 ${refs(end)} 处引用 ⇒ count=${m.count} 站得住`);
    }
  }

  // ── G6 未观察成员（观察集优先；没有观察集就退成字面量下界）
  for (const [tid, t] of Object.entries(types)) {
    for (const m of membersOf(t)) {
      const abs = ABS.get(`${tid}:${m.offN}`);
      if (abs === undefined) { notes.push(`G6 ${tid}: 成员 ${hex(m.offN)} 无绝对锚 ⇒ 未观察判不了`); continue; }
      if (OBS ? !OBS.has(abs) : refs(abs) === 0) notes.push(`G6 ${tid}: 成员 ${hex(m.offN)}（绝对 ${hex(abs)}）零观察 ⇒ unobserved（不是红，是待观察）`);
    }
  }

  // ── J3 区域角色（本版最小实现：span 必须逐字等于导出段落）
  for (const r of M.roles ?? []) {
    const d = deriveRegions(lines, r.fn, r.reg);
    if (!d) { red(`J3 ${r.id}`, `找不到 ${r.fn}`); continue; }
    const a = num(r.span?.[0]); const b = num(r.span?.[1]);
    const hit = d.regions.find((x) => x.from === a && x.to === b);
    if (!hit) red(`J3 ${r.id}`, `span [${hex(a)},${hex(b)}] 不等于 ${r.fn}·${r.reg} 的任何导出段落`);
    else { notes.push(`J3 ${r.id}: 与导出段落一致（类别 ${hit.cat}，段首${hit.openedByInit ? '是' : '不是'} init）`); if (!hit.allConstOrInit) red(`J3 ${r.id}`, '该段落含非常量形态 ⇒ 不能声明成推进链'); }
  }

  if (cons.obj0Acc !== cons.obj0Explained + cons.obj0Residue) red('守恒①', `${cons.obj0Acc} != ${cons.obj0Explained}+${cons.obj0Residue}`);
  if (cons.pointee !== cons.pointeeOk + cons.pointeeOpaque + cons.pointeeBad) red('守恒②', `${cons.pointee} != ${cons.pointeeOk}+${cons.pointeeOpaque}+${cons.pointeeBad}`);
  return { findings, notes, perCarrier, cons };
}

/** 渲染报告（终端用；测试断言 findings / cons 即可） */
export function renderReport(rep, { title = 'claims-check' } = {}) {
  const L = [`# ${title}`, `  类型声明 / 载体 / 角色：见模型文件；本报告只给**判定**与**残差**`, ''];
  L.push('## 逐载体');
  for (const { c, explained, residue, total, pointee, pointeeOk, pointeeOpaque, deep, deepEAs, opaqueEAs, partial } of rep.perCarrier) {
    L.push(`  ${pad(c.id, 12)} ${pad(c.fn, 12)} ${c.reg}→${pad(c.type, 6)} 访问 ${total} 处：解释 ${explained.length} · 残差 ${residue.length}`
      + `｜二级 ${pointee}（解释 ${pointeeOk} · 不透明 ${pointeeOpaque}）`
      + (deep ? `｜**超出解析深度 ${deep} 处**` : '')
      + (opaqueEAs.length ? `｜不透明 EA：${opaqueEAs.slice(0, 3).map(hex).join(' ')}` : '')
      + (partial ? `｜lea 中间量 ${partial} 处` : ''));
    for (const e of explained) L.push(`        ✅ ${hex(e.ea)} abs=${hex(e.abs)} ⇒ 成员 ${hex(e.member.offN)}`);
    for (const e of residue) L.push(`        ⚪ ${hex(e.ea)} ${e.why}`);
  }
  L.push('', '## 提示（不算红）');
  for (const n of rep.notes) L.push(`  · ${n}`);
  const c1 = rep.cons.obj0Acc === rep.cons.obj0Explained + rep.cons.obj0Residue ? '✅' : '❌';
  const c2 = rep.cons.pointee === rep.cons.pointeeOk + rep.cons.pointeeOpaque + rep.cons.pointeeBad ? '✅' : '❌';
  L.push('', `## 守恒①（obj0 相对访问）：${rep.cons.obj0Acc} = 解释 ${rep.cons.obj0Explained} + 残差 ${rep.cons.obj0Residue}  ${c1}`);
  L.push(`## 守恒②（二级对象）：${rep.cons.pointee} = 目标类型解释 ${rep.cons.pointeeOk} + 不透明 ${rep.cons.pointeeOpaque} + 拒绝/未声明 ${rep.cons.pointeeBad}  ${c2}`);
  L.push(`## 超出解析深度（对象层级 ≥3）：${rep.cons.deep} 处  ${rep.cons.deep ? '⚠ 已单列（上限冻结在层级 ）' : '✅ 无'}`);
  L.push('', rep.findings.length ? `❌ ${rep.findings.length} 条红：\n  - ${rep.findings.map((x) => `${x.where}：${x.why}`).join('\n  - ')}` : '✅ 全部义务通过（守卫 / G1..G6 / J3 / 守恒）');
  return `${L.join('\n')}\n`;
}
