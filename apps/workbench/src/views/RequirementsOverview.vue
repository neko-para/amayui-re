<script setup lang="ts">
/**
 * 需求总览：整棵树 + 进度 + 搜索 + 只看未收口 + **新建一张需求单**。
 *
 * ★ **聚合不在这里算**：行里的 `[已收口/子孙总数]`、标记、深度（缩进）、父、聚合告警
 *   全部来自 `/api/tree`（服务端 `flatten` / `rollup`，与 `pnpm tools requirements plan` 同一份函数）。
 *   本文件只负责"谁显示、谁隐藏"这两件纯视图的事。
 * ★ **能不能建单也不在这里判断**：来自 `/api/health` 的 `writes`（服务端按监听地址派生）。
 *   建单本身由 `components/NewRequirementDialog.vue` 走 `POST /api/nodes`。
 * ★ 通用控件（搜索 / 按钮）用 naive-ui；树形行仍是自己的 markup —— 每行都是**真 `<a href="#/req/…">`**
 *   （中键开新 tab 是白送的，`n-tree` 给不了这一点）。
 */
import { computed, defineAsyncComponent, onMounted, ref } from 'vue';
import { NButton, NInput, NTooltip } from 'naive-ui';

import CopyId from '../components/CopyId.vue';
import { canCreate, cannotCreateWhy, health, loadHealth, loadTree, tree, treeError, treeLoading } from '../data';
import { go, hrefReq } from '../router';
import type { TreeRow } from '../api';

/**
 * ★ 建单对话框**懒加载**：它带着表单 / 下拉 / 弹窗那几组组件（naive-ui 里最重的一块），
 *   而"打开总览"这件事用不到它们。`defineAsyncComponent` 让 Vite 把它切成单独的 chunk
 *   —— 与 Monaco 那条纪律同一个道理（见 `README.md`「Monaco 还是懒加载的」）。
 */
const NewRequirementDialog = defineAsyncComponent(() => import('../components/NewRequirementDialog.vue'));

const LIVE = new Set(['open', 'doing', 'blocked']);
const CLOSED = new Set(['done', 'dropped', 'superseded']);
const MARKS: Record<string, string> = {
  open: '⬜',
  doing: '🔜',
  blocked: '⛔',
  done: '✅',
  dropped: '🚫',
  superseded: '♻️',
};

const query = ref('');
/** 默认开（与旧版一致）：只看看没到位的，需要时一键放全 */
const liveOnly = ref(true);
/** 分支默认展开：预算是 ≤40 节点，本来就该一屏读完 */
const overrides = ref<Record<string, boolean>>({});

const isOpen = (id: string) => overrides.value[id] ?? true;
const toggle = (id: string) => {
  overrides.value = { ...overrides.value, [id]: !isOpen(id) };
};

onMounted(() => {
  void loadTree();
  void loadHealth();
});

/** 新建对话框：`writes` 与父节点候选都由服务端数据喂给它（组件自己不做任何裁决） */
const showCreate = ref(false);
/**
 * ★ **首次打开后才挂载**那个组件：`defineAsyncComponent` 只是把它的代码切成单独 chunk，
 *   但"模板里一直有它"会让这次 `import()` 在**打开页面时**就发生 ⇒ 分块的意义就没了。
 *   挂上之后**不再卸**（关掉对话框时保持挂载 ⇒ 关闭动画与滚动锁由 naive-ui 正常收尾）。
 */
const dialogReady = ref(false);
const openCreate = () => {
  dialogReady.value = true;
  showCreate.value = true;
};

/** 建完单：刷新整棵树（新节点要立刻出现在树上），然后跳到它 —— 与"点某一行"走同一条 hash 路由 */
async function onCreated(payload: { short: string }) {
  await loadTree(true);
  go(hrefReq(payload.short));
}

const nodes = computed(() => tree.value?.nodes ?? []);
const byId = computed(() => new Map(nodes.value.map((n) => [n.id, n])));

/** 节点及其祖先里是否还有未收口的东西（决定"只看未收口"要不要留这一行） */
const liveBelow = computed(() => {
  const map = byId.value;
  const set = new Set<string>();
  for (const n of nodes.value) {
    if (!LIVE.has(n.status)) continue;
    let cur = n.parent ? map.get(n.parent) : undefined;
    const guard = new Set<string>();
    while (cur && !guard.has(cur.id)) {
      guard.add(cur.id);
      set.add(cur.id);
      cur = cur.parent ? map.get(cur.parent) : undefined;
    }
  }
  return set;
});

const keepRow = (n: TreeRow, below: Set<string>) => LIVE.has(n.status) || below.has(n.id);

/** 搜索：命中 + 其祖先（作为上下文，压暗显示）。口径照旧：短名 / 名字 / 标题 / 完整 id 都能搜。 */
const search = computed(() => {
  const q = query.value.trim().toLowerCase();
  const bare = q.startsWith('req-') ? q.slice(4) : q;
  const hit = new Set<string>();
  const context = new Set<string>();
  if (!q) return { q, hit, context };
  const map = byId.value;
  for (const n of nodes.value) {
    const matched =
      n.id.toLowerCase().includes(q) ||
      n.name.toLowerCase().includes(bare) ||
      String(n.title ?? '').toLowerCase().includes(q);
    if (matched && keepRow(n, liveBelow.value)) hit.add(n.id);
  }
  for (const id of hit) {
    let cur = map.get(id);
    const guard = new Set<string>();
    while (cur?.parent && map.has(cur.parent) && !guard.has(cur.parent)) {
      guard.add(cur.parent);
      context.add(cur.parent);
      cur = map.get(cur.parent);
    }
  }
  return { q, hit, context };
});

/** 真正要摆出来的行：先按搜索/未收口筛，再按折叠隐藏（命中项的祖先链始终可见） */
const visible = computed(() => {
  const { q, hit, context } = search.value;
  const map = byId.value;
  const out: { row: TreeRow; dim: boolean }[] = [];
  for (const n of nodes.value) {
    let hidden = false;
    let cur = n;
    const guard = new Set<string>();
    while (cur.parent && map.has(cur.parent) && !guard.has(cur.parent)) {
      guard.add(cur.parent);
      if (!isOpen(cur.parent) && !context.has(cur.parent)) {
        hidden = true;
        break;
      }
      cur = map.get(cur.parent)!;
    }
    if (hidden) continue;
    const isHit = hit.has(n.id);
    const isContext = context.has(n.id);
    if (q) {
      if (!isHit && !isContext) continue;
    } else if (liveOnly.value && !keepRow(n, liveBelow.value)) {
      continue;
    }
    out.push({ row: n, dim: Boolean(q) && isContext && !isHit });
  }
  return out;
});

const pills = computed(() => {
  const t = tree.value?.totals ?? {};
  const out: { status: string; n: number }[] = ['doing', 'blocked', 'open', 'done', 'dropped'].map((s) => ({
    status: s,
    n: (t[s] as number) ?? 0,
  }));
  return out;
});

const prefix = (n: TreeRow) => (n.type === 'bug' && n.severity ? `🐞${n.severity} ` : '');
const window_ = (n: TreeRow | { total: number; done: number }) => (n.total > 0 ? `[${n.done}/${n.total}]` : '');
</script>

<template>
  <div class="view-scroll">
    <div class="toolbar">
      <n-input
        v-model:value="query"
        size="small"
        clearable
        style="width: 17rem"
        placeholder="搜索 短名 / 标题 / id"
        aria-label="搜索需求"
      />
      <n-button size="small" :type="liveOnly ? 'primary' : 'default'" :ghost="liveOnly" @click="liveOnly = !liveOnly">
        {{ liveOnly ? '只看未收口' : '全部' }}
      </n-button>
      <n-button size="small" :loading="treeLoading" @click="loadTree(true)">刷新</n-button>
      <span class="grow"></span>
      <!-- 写路径关掉时**明写为什么**：悬停提示 + 一行可见的小字（禁用的按钮不该是"死"的） -->
      <span v-if="health && !canCreate" class="dim writes-off" :title="cannotCreateWhy">建单已关：{{ cannotCreateWhy }}</span>
      <n-tooltip :disabled="canCreate" trigger="hover">
        <template #trigger>
          <span class="tip-wrap">
            <n-button size="small" type="primary" secondary :disabled="!canCreate" @click="openCreate">
              ＋ 新建需求单
            </n-button>
          </span>
        </template>
        {{ cannotCreateWhy }}
      </n-tooltip>
    </div>

    <new-requirement-dialog
      v-if="dialogReady"
      v-model:show="showCreate"
      :writes="health?.writes ?? null"
      :nodes="nodes"
      @created="onCreated"
    />

    <div class="summary">
      <span v-for="p in pills" :key="p.status" class="pill">
        <span class="dot" :class="`s-${p.status}`"></span>{{ p.status }}<b>{{ p.n }}</b>
      </span>
      <span v-if="tree?.totals.bugs" class="pill">🐞<b>{{ tree.totals.bugs }}</b></span>
      <span v-if="tree" class="pill">{{ tree.totals.count }} / {{ tree.budget.maxNodes }} 节点</span>
    </div>

    <p v-if="treeError" class="notice err">读取台账失败：{{ treeError }}</p>
    <p v-else-if="!tree" class="empty">正在读取…</p>

    <div v-else class="tree">
      <div
        v-for="item in visible"
        :key="item.row.id"
        class="row"
        :data-depth="Math.min(Math.max(item.row.depth, 0), 3)"
        :style="{ '--depth': Math.max(item.row.depth, 0) }"
      >
        <button
          v-if="item.row.kids > 0"
          class="twist"
          type="button"
          :aria-expanded="isOpen(item.row.id)"
          :aria-label="isOpen(item.row.id) ? '收起分支' : '展开分支'"
          @click="toggle(item.row.id)"
        >
          {{ isOpen(item.row.id) ? '▾' : '▸' }}
        </button>
        <span v-else class="twist" aria-hidden="true"></span>

        <span class="mark" :class="`s-${item.row.status}`" :title="item.row.status">{{ MARKS[item.row.status] ?? '·' }}</span>

        <div class="row-main">
          <div class="title-line">
            <a class="title" :href="hrefReq(item.row.short)" :title="item.row.id" :data-dim="item.dim ? '1' : undefined">
              {{ prefix(item.row) }}{{ item.row.title || '(无标题)' }}
            </a>
            <span v-if="item.row.aggregated" class="flag warn">⚠ 聚合不一致</span>
            <span v-else-if="item.row.orphan" class="flag warn">❓ 从根走不到</span>
          </div>
          <div class="meta-line">
            <a class="short" :href="hrefReq(item.row.short)" :title="item.row.id">{{ item.row.short }}</a>
            <CopyId :id="item.row.id" />
            <span class="prog">{{ window_(item.row) }}</span>
            <span v-if="item.row.blockedBy.length" class="blocked">前置 {{ item.row.blockedBy.join(', ') }}</span>
            <span v-for="t in item.row.tags" :key="t" class="tag">{{ t }}</span>
          </div>
        </div>
      </div>

      <p v-if="!visible.length" class="empty">
        {{ query ? `没有匹配「${query}」的条目` : '没有需要显示的节点' }}
      </p>

      <div v-if="tree.decisions.length" class="decisions">
        <p class="empty">决策（{{ tree.decisions.length }}，不属进度树）</p>
        <div v-for="d in tree.decisions" :key="d.id" class="row">
          <span class="twist" aria-hidden="true"></span>
          <span class="mark">{{ d.status === 'superseded' ? '♻️' : '📌' }}</span>
          <div class="row-main">
            <div class="title-line"><span class="title">{{ d.title }}</span></div>
            <div class="meta-line">
              <a class="short" :href="hrefReq(d.short)" :title="d.id">{{ d.short }}</a>
              <CopyId :id="d.id" />
              <span class="tag">{{ d.status }}</span>
            </div>
          </div>
        </div>
      </div>
    </div>

    <p class="foot-hint">
      GET 只读（列表里的数字全部现算）；建单走 <code>POST /api/nodes</code> —— 与
      <code>pnpm tools requirements add</code> 共用同一个 <code>planAdd()</code>；改已有节点仍只有
      <code>pnpm tools requirements set --write</code>。
      <span v-if="tree"> · 数据 {{ tree.dir }}</span>
    </p>
  </div>
</template>

<style scoped>
.decisions {
  margin-top: 1.2rem;
  border-top: 1px dashed var(--line);
  padding-top: 0.6rem;
}
/* 触发器的宿主：`n-tooltip` 要一个**可悬停**的元素 —— 禁用的按钮本身不派发鼠标事件 */
.tip-wrap {
  display: inline-flex;
}
.writes-off {
  font-size: 11px;
  max-width: 26rem;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
</style>
