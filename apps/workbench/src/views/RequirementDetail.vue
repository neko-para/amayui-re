<script setup lang="ts">
/**
 * 需求详情：字段 + 正文（按 `## ` 切小节，逐节 **Markdown 渲染**）+ 面包屑 + 直接子节点。
 *
 * ★ 数据一律走 `/api/node/<ref>`（服务端 `describeNode`）：小节的切法、父链、直接子
 *   **都是模型算的**，这里不重写"什么是一节"。这样"冷启动直达 `#/req/X`"与"总览里点进来"走同一条路。
 * ★ 渲染是**客户端**的事（`src/markdown.ts`，naive-ui 不管 Markdown）：服务端照旧只给文本，
 *   `sections[].text` 一个字节都不改。`v-html` 的内容来自我们自己的渲染器且 `html: false`
 *   ⇒ 正文里的原始 HTML 只会以文本出现（不需要 sanitizer，见 markdown.ts 的口径）。
 */
import { computed, defineAsyncComponent, onMounted, ref, watch } from 'vue';
import { NButton } from 'naive-ui';

import CopyId from '../components/CopyId.vue';
import { canCreate, cannotCreateWhy, health, loadHealth, loadNode } from '../data';
import { renderMarkdown } from '../markdown';
import { go, hrefReq, hrefReqs } from '../router';
import type { ChainItem, NodePayload } from '../api';

/** ★ 懒加载：与总览同一份理由（表单 / 下拉 / 弹窗是 naive-ui 里最重的一组，不进外壳 chunk） */
const NewRequirementDialog = defineAsyncComponent(() => import('../components/NewRequirementDialog.vue'));

const props = defineProps<{ nodeRef: string }>();

const payload = ref<NodePayload | null>(null);
const error = ref('');
const loading = ref(false);

const MARKS: Record<string, string> = {
  open: '⬜',
  doing: '🔜',
  blocked: '⛔',
  done: '✅',
  dropped: '🚫',
  superseded: '♻️',
};
const LIVE = new Set(['open', 'doing', 'blocked']);
const CLOSED = new Set(['done', 'dropped', 'superseded']);

async function load() {
  loading.value = true;
  error.value = '';
  payload.value = null;
  try {
    payload.value = await loadNode(props.nodeRef);
  } catch (err) {
    error.value = (err as Error).message;
  } finally {
    loading.value = false;
  }
}

watch(() => props.nodeRef, load, { immediate: true });

const node = computed(() => payload.value?.node ?? null);
const prefix = computed(() => (node.value?.type === 'bug' && node.value.severity ? `🐞${node.value.severity} ` : ''));
const window_ = (x: { total: number; done: number }) => (x.total > 0 ? `[${x.done}/${x.total}]` : '');

/** 子节点沿用总览的"未收口"口径：自身活着，或子树里还有活的 */
const keep = (k: ChainItem) => LIVE.has(k.status) || k.rollup.live > 0;
const children = computed(() => (payload.value?.children ?? []).filter(keep));
const hiddenChildren = computed(() => (payload.value?.children ?? []).length - children.value.length);
const showAllChildren = ref(false);
const shownChildren = computed(() => (showAllChildren.value ? payload.value?.children ?? [] : children.value));

/** 新建**子**需求单：父节点预选成当前这条（能不能按由服务端的 `writes` 说了算） */
const showCreate = ref(false);
/** 首次打开后才挂载对话框（理由与总览一致：不让它在打开页面时就下载那个 chunk） */
const dialogReady = ref(false);
const openCreate = () => {
  dialogReady.value = true;
  showCreate.value = true;
};
onMounted(() => void loadHealth());
/** 建完直接跳过去（本组件按 `nodeRef` 做 key，换一条会重挂） */
const onCreated = (created: { short: string }) => go(hrefReq(created.short));

/** 小节正文 → HTML（同一套渲染器，详情页与"新建"里的预览逐字节一致） */
const sectionHtml = (text: string) => renderMarkdown(text);
</script>

<template>
  <div class="view-scroll">
    <div class="crumbs">
      <a :href="hrefReqs()">← 返回总览</a>
      <template v-for="p in payload?.parentChain ?? []" :key="p.id">
        <span class="crumb-sep">›</span>
        <a :href="hrefReq(p.short)" :title="p.id">{{ p.short }}</a>
      </template>
    </div>

    <p v-if="error" class="notice err">打开失败：{{ error }}</p>
    <p v-else-if="loading || !node" class="empty">正在读取…</p>

    <template v-else>
      <div class="dhead">
        <h2 class="dtitle">{{ prefix }}{{ node.title || '(无标题)' }}</h2>
        <div class="dsub">
          <span class="mark" :class="`s-${node.status}`">{{ MARKS[node.status] ?? '·' }}</span>
          <span>{{ node.status }}</span>
          <span v-if="node.total > 0"> · 子树 {{ window_(node) }}</span>
          <span v-if="node.aggregated" class="flag warn"> · ⚠ 聚合状态与自身不一致</span>
          <span class="grow"></span>
          <n-button
            size="tiny"
            secondary
            type="primary"
            :disabled="!canCreate"
            :title="canCreate ? '在这条下面新建一张子需求单' : cannotCreateWhy"
            @click="openCreate"
          >
            ＋ 子需求单
          </n-button>
        </div>
      </div>

      <new-requirement-dialog
        v-if="dialogReady"
        v-model:show="showCreate"
        :writes="health?.writes ?? null"
        :nodes="[]"
        :parent-id="node.id"
        @created="onCreated"
      />

      <section class="card">
        <h3>字段</h3>
        <div class="fields">
          <div class="fk">id</div>
          <div class="fv mono">{{ node.id }}</div>
          <div class="fk"></div>
          <div class="fv"><CopyId :id="node.id" wide /></div>
          <div class="fk">类型</div>
          <div class="fv">{{ node.type }}</div>
          <template v-if="node.blockedBy.length">
            <div class="fk">前置</div>
            <div class="fv">
              <template v-for="(ref_, i) in node.blockedBy" :key="ref_"
                ><span v-if="i"> </span><a :href="hrefReq(ref_)">{{ ref_ }}</a></template
              >
            </div>
          </template>
          <template v-if="node.tags.length">
            <div class="fk">标签</div>
            <div class="fv">{{ node.tags.join(', ') }}</div>
          </template>
          <template v-if="node.verify">
            <div class="fk">收口凭据</div>
            <div class="fv mono">{{ node.verify }}</div>
          </template>
          <template v-if="node.repro">
            <div class="fk">复现</div>
            <div class="fv mono">{{ node.repro }}</div>
          </template>
          <template v-if="node.doneReason">
            <div class="fk">收口说明</div>
            <div class="fv">{{ node.doneReason }}</div>
          </template>
          <template v-if="node.droppedReason">
            <div class="fk">关单理由</div>
            <div class="fv">{{ node.droppedReason }}</div>
          </template>
          <template v-if="node.supersedes.length">
            <div class="fk">取代</div>
            <div class="fv">{{ node.supersedes.join(', ') }}</div>
          </template>
          <div class="fk">源文件行数</div>
          <div class="fv">{{ node.lines ?? '?' }} 行</div>
        </div>
      </section>

      <section class="card">
        <h3>正文</h3>
        <div class="prose">
          <template v-for="(s, i) in node.sections" :key="i">
            <h4 v-if="s.heading" class="md-h">{{ s.heading }}</h4>
            <div v-if="s.text" class="md" v-html="sectionHtml(s.text)"></div>
          </template>
        </div>
      </section>

      <section class="card">
        <h3>
          子节点<span v-if="shownChildren.length">（{{ shownChildren.length }}）</span>
          <n-button v-if="hiddenChildren" class="tiny" size="tiny" quaternary @click="showAllChildren = !showAllChildren">
            {{ showAllChildren ? '只看未收口' : `显示全部（还有 ${hiddenChildren} 个已收口）` }}
          </n-button>
        </h3>
        <div v-if="shownChildren.length" class="kids">
          <div v-for="k in shownChildren" :key="k.id" class="kid" :data-dim="CLOSED.has(k.status) ? '1' : undefined">
            <span class="mark" :class="`s-${k.status}`" :title="k.status">{{ MARKS[k.status] ?? '·' }}</span>
            <a class="short" :href="hrefReq(k.short)" :title="k.id">{{ k.short }}</a>
            <a class="title" :href="hrefReq(k.short)" :title="k.id">{{ k.title || '(无标题)' }}</a>
            <span class="prog">{{ window_(k) }}</span>
          </div>
        </div>
        <p v-else class="empty">没有（未收口的）子节点</p>
      </section>
    </template>
  </div>
</template>
