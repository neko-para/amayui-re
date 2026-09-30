/*
 * apps/requirements/public/app.js — 总览 + 详情。
 *
 * 设计口径：
 *   · **相对跳转为主**：`#/` 总览、`#/req/<ref>` 详情。hash 不发给服务器，
 *     所以不需要 History API、不需要服务器 rewrite；浏览器后退键就是"返回"。
 *     每行是真 <a href="#/req/…"> ⇒ 中键/cmd-click 直接开浏览器新 tab（白送的并排）。
 *   · **与 DSH 无关**：不引任何框架、不引 CDN、不引 DSH 的东西。只 fetch 本服务的两个端点。
 *   · **只用 textContent 填内容**，绝不拼 innerHTML —— 标题与正文是我们自己写的，
 *     但"只有一处能引入 HTML"这个性质值得一直留着。
 *   · 聚合/解析都不在本文件算：`/api/tree` 与 `/api/node` 给的就是**模型算好的**数字
 *     （`plan` 用的同一套函数）。这里只负责摆版。
 */

const $ = (id) => document.getElementById(id);
const el = {
  search: $('search'),
  filter: $('filter'),
  theme: $('theme'),
  reload: $('reload'),
  summary: $('summary'),
  notice: $('notice'),
  main: $('main'),
  dir: $('dir'),
  tplRow: $('tpl-row'),
};

const LIVE = new Set(['open', 'doing', 'blocked']);
const CLOSED = new Set(['done', 'dropped', 'superseded']);
const MARKS = { open: '⬜', doing: '🔜', blocked: '⛔', done: '✅', dropped: '🚫', superseded: '♻️' };

const state = {
  /** Whole ledger payload from `/api/tree`, or null before the first load. */
  tree: null,
  /** Flat node from `/api/tree`, used to render a detail page instantly. */
  detail: null,
  loading: false,
  error: '',
  query: '',
  liveOnly: true,
  /** Node ids the user explicitly toggled, overriding the default fold. */
  overrides: new Map(),
};

// ── theme（浅 / 深 / 跟随系统）────────────────────────────────────────────

const THEME_KEY = 'requirements.theme';
const THEME_CYCLE = ['system', 'light', 'dark'];
const THEME_LABEL = { system: '◐ 跟随系统', light: '☀ 浅色', dark: '☾ 深色' };

function applyTheme(mode) {
  if (mode === 'system') document.documentElement.removeAttribute('data-theme');
  else document.documentElement.setAttribute('data-theme', mode);
  el.theme.textContent = THEME_LABEL[mode];
  el.theme.title = `配色：${mode}（点击切换）`;
}

function readTheme() {
  const saved = localStorage.getItem(THEME_KEY);
  return THEME_CYCLE.includes(saved) ? saved : 'system';
}

// ── 派生（全部来自服务端给的数字，不在这里重算聚合）──────────────────────

const byId = () => new Map((state.tree?.nodes ?? []).map((n) => [n.id, n]));

/** 节点及其祖先里是否还有未收口的东西（决定"只看未收口"要不要留这一行）。 */
function liveBelowSet() {
  const map = byId();
  const set = new Set();
  for (const n of state.tree?.nodes ?? []) {
    if (!LIVE.has(n.status)) continue;
    let cur = n.parent ? map.get(n.parent) : null;
    const guard = new Set();
    while (cur && !guard.has(cur.id)) {
      guard.add(cur.id);
      set.add(cur.id);
      cur = cur.parent ? map.get(cur.parent) : null;
    }
  }
  return set;
}

/** 分支默认展开：预算是 ≤40 节点，本来就该一屏读完。 */
const isOpen = (id) => (state.overrides.has(id) ? state.overrides.get(id) : true);

function keepRow(n, liveBelow) {
  return LIVE.has(n.status) || liveBelow.has(n.id);
}

// ── 渲染小工具 ───────────────────────────────────────────────────────

function badge(status) {
  const span = document.createElement('span');
  span.className = `mark s-${status}`;
  span.textContent = MARKS[status] ?? '·';
  span.title = status;
  return span;
}

function shortLink(node, label) {
  const a = document.createElement('a');
  a.className = 'short';
  a.href = `#/req/${encodeURIComponent(node.short)}`;
  a.title = node.id;
  a.textContent = label ?? node.short;
  return a;
}

function window_(node) {
  return node.total > 0 ? `[${node.done}/${node.total}]` : '';
}

// ── 复制完整 id ──────────────────────────────────────────────────────

/**
 * 复制**完整 id**（不是显示用的短名）。
 *
 * 页面上给人读的是 8 位短名（26 位 ULID 对人不提供有效信息），但**粘进终端**要的是完整 id：
 * `pnpm tools requirements show REQ-…`。所以"显示短名 + 复制完整 id"才是对的那一对。
 */
async function copyText(text) {
  try {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(text);
      return true;
    }
  } catch {
    /* 落到下面的兜底：非安全上下文或权限被拒 */
  }
  // 兜底：http 下的非 localhost（例如 `--host` 暴露到局域网）没有 clipboard API。
  try {
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.setAttribute('readonly', '');
    ta.style.position = 'fixed';
    ta.style.top = '-1000px';
    ta.style.opacity = '0';
    document.body.append(ta);
    ta.select();
    ta.setSelectionRange(0, text.length);
    const ok = document.execCommand('copy');
    ta.remove();
    return ok;
  } catch {
    return false;
  }
}

/** 短暂的"已复制"反馈；把原文案存进 dataset，避免连点后残留错字。 */
function flash(btn, text) {
  if (btn.dataset.orig === undefined) btn.dataset.orig = btn.textContent;
  btn.textContent = text;
  btn.classList.add('copied');
  clearTimeout(Number(btn.dataset.timer || 0));
  btn.dataset.timer = String(
    setTimeout(() => {
      btn.textContent = btn.dataset.orig;
      btn.classList.remove('copied');
    }, 1200),
  );
}

/** `id` 是要复制的完整 id；`elt` 可选 —— 高亮它来表示"复制的是这一条"。 */
function wireCopy(btn, id, elt) {
  btn.title = `复制完整 id：${id}`;
  btn.addEventListener('click', async (ev) => {
    ev.preventDefault(); // 别把按钮点击变成跳转
    const ok = await copyText(id);
    flash(btn, ok ? '✓' : '✗');
    if (!ok) btn.title = '复制失败 —— 请手动选中 id（悬停短名可见完整 id）';
    else if (elt) {
      elt.classList.add('copied');
      setTimeout(() => elt.classList.remove('copied'), 700);
    }
  });
}

// ── 总览 ─────────────────────────────────────────────────────────────

function renderSummary() {
  const t = state.tree?.totals ?? {};
  el.summary.replaceChildren();
  const cells = [
    ['doing', t.doing],
    ['blocked', t.blocked],
    ['open', t.open],
    ['done', t.done],
    ['dropped', t.dropped],
  ];
  for (const [status, n] of cells) {
    const pill = document.createElement('span');
    pill.className = 'pill';
    const dot = document.createElement('span');
    dot.className = `dot s-${status}`;
    pill.append(dot, document.createTextNode(status));
    const b = document.createElement('b');
    b.textContent = String(n ?? 0);
    pill.append(b);
    el.summary.append(pill);
  }
  if (t.bugs) {
    const pill = document.createElement('span');
    pill.className = 'pill';
    pill.append(document.createTextNode('🐞'), Object.assign(document.createElement('b'), { textContent: String(t.bugs) }));
    el.summary.append(pill);
  }
  const budget = state.tree?.budget;
  if (budget) {
    const pill = document.createElement('span');
    pill.className = 'pill';
    pill.textContent = `${t.count ?? 0} / ${budget.maxNodes} 节点`;
    el.summary.append(pill);
  }
}

function renderTree() {
  const nodes = state.tree?.nodes ?? [];
  const liveBelow = liveBelowSet();
  const map = byId();

  // 搜索：命中 + 其祖先（作为上下文）。
  const q = state.query.trim().toLowerCase();
  const bare = q.startsWith('req-') ? q.slice(4) : q;
  const matched = new Set();
  if (q) {
    for (const n of nodes) {
      if (
        n.id.toLowerCase().includes(q) ||
        n.name.toLowerCase().includes(bare) ||
        String(n.title ?? '').toLowerCase().includes(q)
      ) {
        if (keepRow(n, liveBelow)) matched.add(n.id);
      }
    }
  }
  const context = new Set();
  for (const id of matched) {
    let cur = map.get(id);
    const guard = new Set();
    while (cur?.parent && map.has(cur.parent) && !guard.has(cur.parent)) {
      guard.add(cur.parent);
      context.add(cur.parent);
      cur = map.get(cur.parent);
    }
  }

  const frag = document.createDocumentFragment();
  const tree = document.createElement('div');
  tree.className = 'tree';

  let shown = 0;
  for (const n of nodes) {
    // 祖先折叠 ⇒ 隐藏（除非它是命中项的上下文/FOCUS 链）。
    let hidden = false;
    let cur = n;
    const guard = new Set();
    while (cur.parent && map.has(cur.parent) && !guard.has(cur.parent)) {
      guard.add(cur.parent);
      if (!isOpen(cur.parent) && !context.has(cur.parent)) { hidden = true; break; }
      cur = map.get(cur.parent);
    }
    if (hidden) continue;

    const isHit = matched.has(n.id);
    const isContext = context.has(n.id);
    if (q) {
      if (!isHit && !isContext) continue;
    } else if (state.liveOnly && !keepRow(n, liveBelow)) {
      continue;
    }

    const row = el.tplRow.content.firstElementChild.cloneNode(true);
    // ★ 横向偏移：树状必须看得出来。纯纵向折叠只表达"展开/收起"，表达不了**从属**。
    //   depth 由服务端 flatten 给出（与 plan 的缩进同源），这里只负责摆版。
    row.style.setProperty('--depth', String(Math.max(n.depth, 0)));
    const ctxOnly = q && isContext && !isHit;
    if (ctxOnly) row.dataset.dim = '1';

    const twist = row.querySelector('.twist');
    if (n.kids > 0) {
      const open = isOpen(n.id);
      twist.textContent = open ? '▾' : '▸';
      twist.setAttribute('aria-expanded', String(open));
      twist.setAttribute('aria-label', open ? '收起分支' : '展开分支');
      twist.addEventListener('click', () => {
        state.overrides.set(n.id, !isOpen(n.id));
        renderTree();
      });
    } else {
      twist.disabled = true;
    }

    row.querySelector('.mark').replaceWith(badge(n.status));

    const title = row.querySelector('.title');
    title.href = `#/req/${encodeURIComponent(n.short)}`;
    title.title = n.id;
    title.textContent = `${n.type === 'bug' && n.severity ? `🐞${n.severity} ` : ''}${n.title || '(无标题)'}`;

    const flag = row.querySelector('.flag');
    if (n.aggregated) flag.textContent = '⚠ 聚合不一致';
    else if (n.orphan) flag.textContent = '❓ 从根走不到';

    const short = shortLink(n);
    row.querySelector('.short').replaceWith(short);
    const copyBtn = row.querySelector('.copy');
    if (copyBtn) wireCopy(copyBtn, n.id, short);
    row.querySelector('.prog').textContent = window_(n);
    if (n.blockedBy?.length) row.querySelector('.blocked').textContent = `前置 ${n.blockedBy.join(', ')}`;

    tree.append(row);
    shown += 1;
  }

  if (!shown) {
    const p = document.createElement('p');
    p.className = 'empty';
    p.textContent = q ? `没有匹配「${state.query}」的条目` : '没有需要显示的节点';
    tree.append(p);
  }

  if (state.tree?.decisions?.length) {
    const box = document.createElement('div');
    const h = document.createElement('div');
    h.className = 'empty';
    h.textContent = `决策（${state.tree.decisions.length}，不属进度树）`;
    box.append(h);
    for (const d of state.tree.decisions) {
      const row = document.createElement('div');
      row.className = 'row';
      const mark = document.createElement('span');
      mark.className = 'mark';
      mark.textContent = d.status === 'superseded' ? '♻️' : '📌';
      const main = document.createElement('div');
      main.className = 'row-main';
      const t = document.createElement('div');
      t.className = 'title';
      t.textContent = d.title;
      const meta = document.createElement('div');
      meta.className = 'meta-line';
      meta.append(shortLink(d));
      main.append(t, meta);
      row.append(mark, main);
      box.append(row);
    }
    tree.append(box);
  }

  frag.append(tree);
  el.main.replaceChildren(frag);
}

// ── 详情 ─────────────────────────────────────────────────────────────

/** 正文只在 `## ` 处切小节（台账自己的写法）；不引 Markdown 库。 */
function fillProse(container, sections, fallbackText) {
  const list = sections ?? null;
  if (list?.length) {
    for (const s of list) {
      if (s.heading) {
        const h = document.createElement('h3');
        h.textContent = s.heading;
        container.append(h);
      }
      if (s.text) container.append(document.createTextNode(s.text + '\n'));
    }
    return;
  }
  container.textContent = fallbackText ?? '';
}

function fieldRow(label, valueNode) {
  const k = document.createElement('div');
  k.className = 'fk';
  k.textContent = label;
  const v = document.createElement('div');
  v.className = 'fv';
  if (typeof valueNode === 'string') v.textContent = valueNode;
  else v.append(valueNode);
  return [k, v];
}

/** 详情页 id 那一行的"复制完整 id"。 */
function copyIdButton(id, label = '⧉ 复制 id') {
  const btn = document.createElement('button');
  btn.type = 'button';
  btn.className = 'copy wide';
  btn.textContent = label;
  wireCopy(btn, id);
  return btn;
}

function renderDetail(node) {
  const liveBelow = liveBelowSet();
  const map = byId();
  const wrap = document.createElement('div');
  wrap.className = 'detail';

  // 面包屑：向上每一跳都可点（父链由服务端算好，不在这里重走 parent）。
  const crumbs = document.createElement('div');
  crumbs.className = 'crumbs';
  const back = document.createElement('a');
  back.href = '#/';
  back.textContent = '← 返回总览';
  crumbs.append(back);
  for (const p of node.parentChain ?? []) {
    crumbs.append(document.createTextNode(' › '));
    const a = document.createElement('a');
    a.href = `#/req/${encodeURIComponent(p.short)}`;
    a.title = p.id;
    a.textContent = p.short;
    crumbs.append(a);
  }
  wrap.append(crumbs);

  const head = document.createElement('div');
  head.className = 'dhead';
  const h = document.createElement('div');
  h.className = 'dtitle';
  h.textContent = `${node.type === 'bug' && node.severity ? `🐞${node.severity} ` : ''}${node.title || '(无标题)'}`;
  const sub = document.createElement('div');
  sub.className = 'dsub';
  sub.append(badge(node.status), document.createTextNode(node.status));
  if (node.total > 0) sub.append(document.createTextNode(` · 子树 ${window_(node)}`));
  if (node.aggregated) {
    const w = document.createElement('span');
    w.className = 'flag';
    w.textContent = '⚠ 聚合状态与自身不一致';
    sub.append(document.createTextNode(' · '), w);
  }
  head.append(h, sub);
  wrap.append(head);

  // 字段
  const fields = document.createElement('div');
  fields.className = 'card';
  const fh = document.createElement('h2');
  fh.textContent = '字段';
  const grid = document.createElement('div');
  grid.className = 'fields';
  const push = (label, value) => { for (const n of fieldRow(label, value)) grid.append(n); };
  push('id', node.id);
  // 复制完整 id —— 显示的就是它，这里再给一个一键复制（粘进终端用）。
  const idCell = document.createElement('div');
  idCell.className = 'fv';
  idCell.append(copyIdButton(node.id));
  grid.append(document.createElement('div'), idCell);
  push('类型', node.type ?? '');
  if (node.blockedBy?.length) {
    const box = document.createElement('span');
    node.blockedBy.forEach((ref, i) => {
      if (i) box.append(document.createTextNode(' '));
      const target = map.get(ref);
      if (!target) {
        box.append(document.createTextNode(ref));
        return;
      }
      const a = document.createElement('a');
      a.href = `#/req/${encodeURIComponent(target.short)}`;
      a.title = target.id;
      a.textContent = target.short;
      box.append(a);
    });
    push('前置', box);
  }
  if (node.tags?.length) push('标签', node.tags.join(', '));
  if (node.verify) push('收口凭据', node.verify);
  if (node.repro) push('复现', node.repro);
  if (node.doneReason) push('收口说明', node.doneReason);
  if (node.droppedReason) push('关单理由', node.droppedReason);
  if (node.supersedes?.length) push('取代', node.supersedes.join(', '));
  push('源文件行数', `${node.lines ?? '?'} 行`);
  fields.append(fh, grid);
  wrap.append(fields);

  // 正文
  if (node.body || node.sections?.length) {
    const body = document.createElement('div');
    body.className = 'card';
    const bh = document.createElement('h2');
    bh.textContent = '正文';
    const prose = document.createElement('div');
    prose.className = 'prose';
    fillProse(prose, node.sections, node.body);
    body.append(bh, prose);
    wrap.append(body);
  }

  // 子节点（与总览同一套"未收口"默认）
  const kids = (node.children ?? []).filter((k) => LIVE.has(k.status) || liveBelow.has(k.id));
  const kidsCard = document.createElement('div');
  kidsCard.className = 'card';
  const kh = document.createElement('h2');
  kh.textContent = `子节点${kids.length ? `（${kids.length}）` : ''}`;
  kidsCard.append(kh);
  if (kids.length) {
    const list = document.createElement('div');
    list.className = 'kids';
    for (const k of kids) {
      const row = document.createElement('div');
      row.className = 'kid';
      row.dataset.depth = '1'; // 子节点缩进一级（与总览同一套视觉语言）
      if (CLOSED.has(k.status)) row.dataset.dim = '1';
      const a = document.createElement('a');
      a.className = 'title';
      a.href = `#/req/${encodeURIComponent(k.short)}`;
      a.title = k.id;
      a.textContent = k.title || '(无标题)';
      const mark = badge(k.status);
      const s = shortLink(k);
      const prog = document.createElement('span');
      prog.className = 'prog';
      prog.textContent = k.total > 0 ? `[${k.done}/${k.total}]` : '';
      row.append(mark, s, a, prog);
      list.append(row);
    }
    kidsCard.append(list);
  } else {
    const p = document.createElement('p');
    p.className = 'empty';
    p.textContent = '没有（未收口的）子节点';
    kidsCard.append(p);
  }
  wrap.append(kidsCard);

  el.main.replaceChildren(wrap);
}

// ── 数据 ─────────────────────────────────────────────────────────────

async function fetchJson(url) {
  const res = await fetch(url, { headers: { accept: 'application/json' }, cache: 'no-store' });
  const data = await res.json().catch(() => null);
  if (!res.ok || !data) throw new Error(data?.error ?? `HTTP ${res.status}`);
  return data;
}

async function loadTree(force = false) {
  if (state.loading) return;
  if (state.tree && !force) return;
  state.loading = true;
  state.error = '';
  setNotice('');
  try {
    state.tree = await fetchJson('/api/tree');
    el.dir.textContent = state.tree.dir ? `数据 ${state.tree.dir}` : '';
    renderSummary();
    renderRoute();
  } catch (err) {
    state.error = err.message;
    setNotice(`读取台账失败：${err.message}`, true);
    el.main.replaceChildren(Object.assign(document.createElement('p'), { className: 'empty', textContent: '读取失败。' }));
  } finally {
    state.loading = false;
  }
}

/**
 * Detail data for one node.
 *
 * 总览已经拿了整棵树（含正文），所以页面内跳转时直接就地渲染、**再等一次请求**。
 * `/api/node` 只在"冷启动直达 `#/req/X`"时用 —— 那时树还没到，而且它额外给出
 * 服务端切好的 `sections` 与 `parentChain`。
 */
async function loadDetail(ref) {
  const flat = (state.tree?.nodes ?? []).find(
    (n) => n.short === ref || n.id === ref || n.name === ref,
  );
  if (flat) {
    const liveBelow = liveBelowSet();
    const node = {
      ...flat,
      parentChain: chainOf(flat),
      children: (state.tree.nodes ?? []).filter((n) => n.parent === flat.id),
    };
    renderDetail(node);
    return;
  }
  el.main.replaceChildren(Object.assign(document.createElement('p'), { className: 'empty', textContent: '正在读取…' }));
  try {
    const payload = await fetchJson(`/api/node/${encodeURIComponent(ref)}`);
    renderDetail(payload.node);
  } catch (err) {
    setNotice(`打开失败：${err.message}`, true);
    el.main.replaceChildren(Object.assign(document.createElement('p'), { className: 'empty', textContent: `找不到「${ref}」。` }));
  }
}

function chainOf(node) {
  const map = byId();
  const chain = [];
  let cur = node;
  const guard = new Set([node.id]);
  while (cur?.parent && map.has(cur.parent) && !guard.has(cur.parent)) {
    guard.add(cur.parent);
    cur = map.get(cur.parent);
    chain.unshift(cur);
  }
  return chain;
}

// ── 路由 ─────────────────────────────────────────────────────────────

function currentRef() {
  const m = /^#\/req\/(.+)$/.exec(location.hash);
  return m ? decodeURIComponent(m[1]) : null;
}

function renderRoute() {
  const ref = currentRef();
  el.search.hidden = ref !== null;
  el.filter.hidden = ref !== null;
  if (ref === null) {
    if (!state.tree) return;
    renderSummary();
    renderTree();
    return;
  }
  if (!state.tree) return;
  loadDetail(ref);
}

function setNotice(text, isError = false) {
  el.notice.textContent = text;
  el.notice.className = isError ? 'notice err' : 'notice';
  el.notice.hidden = !text;
}

// ── 启动 ─────────────────────────────────────────────────────────────

applyTheme(readTheme());

el.theme.addEventListener('click', () => {
  const now = readTheme();
  const next = THEME_CYCLE[(THEME_CYCLE.indexOf(now) + 1) % THEME_CYCLE.length];
  localStorage.setItem(THEME_KEY, next);
  applyTheme(next);
});

el.search.addEventListener('input', () => {
  state.query = el.search.value;
  if (state.tree) renderTree();
});

el.filter.addEventListener('click', () => {
  state.liveOnly = !state.liveOnly;
  el.filter.setAttribute('aria-pressed', String(state.liveOnly));
  el.filter.textContent = state.liveOnly ? '只看未收口' : '全部';
  if (state.tree) renderTree();
});

el.reload.addEventListener('click', () => loadTree(true));
window.addEventListener('hashchange', renderRoute);

loadTree();
