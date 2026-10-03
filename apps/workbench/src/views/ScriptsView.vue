<script setup lang="ts">
/**
 * 脚本：左列表（**基线根里所有能反汇编的 AGE 脚本**，「旧管线标注过」只是标签）+ 右 **Monaco 只读正文**
 * （`data` / `src` 切换、行数）。
 *
 * ★ 正文**只有一份真源**：服务端 `buildView` 的输出，客户端一个字都不加工
 *   （`data` = 基线的反汇编；`src` = 基线 + patch，字符串取 patch 里的中文）。
 *   **没有 patch 条目**的脚本用空叠加层 ⇒ `src` 与 `data` 相同（不是错误，页头会写明）。
 * ★ `data` / `src` 是**真链接**（`#/script/<名字>?kind=…`）：中键开新 tab、后退键都对。
 * ★ 名单可能上千行 ⇒ **只渲染前 `RENDER_MAX` 行**（并在列表里显式写出来），滚动不卡。
 */
import { computed, onMounted, ref, watch } from 'vue';

import MonacoViewer from '../components/MonacoViewer.vue';
import { loadScriptView, loadScripts, scriptsError, scriptsIndex, scriptsLoading } from '../data';
import { hrefScript, type ScriptKind } from '../router';
import type { ScriptPayload } from '../api';

const props = defineProps<{ name: string | null; kind: ScriptKind; line: number | null }>();

/** 列表一次最多渲染多少行（上千行的 DOM 会让滚动发涩；要全看就用搜索/筛选收窄） */
const RENDER_MAX = 300;
/** 筛选口径：全部 / 只看标注过的 / 只看有变更（都是**标签**，与名单来源无关） */
type Scope = 'all' | 'annotated' | 'changed';

const filter = ref('');
const scope = ref<Scope>('all');
const view = ref<ScriptPayload | null>(null);
const viewError = ref('');
const viewLoading = ref(false);
const jump = ref('');
const viewer = ref<InstanceType<typeof MonacoViewer> | null>(null);

onMounted(() => void loadScripts());

/** 筛选后的**全部**命中行（服务端已按名字排好序，这里不再重排） */
const rows = computed(() => {
  const q = filter.value.trim().toLowerCase();
  const all = scriptsIndex.value?.scripts ?? [];
  return all.filter((s) => {
    if (scope.value === 'annotated' && !s.annotated) return false;
    if (scope.value === 'changed' && !s.hasPatch) return false;
    return !q || s.name.toLowerCase().includes(q);
  });
});
/** 真正进 DOM 的那一段（剩余的只报数字，用户用搜索/筛选收窄） */
const visible = computed(() => rows.value.slice(0, RENDER_MAX));
const hidden = computed(() => rows.value.length - visible.value.length);

const SCOPES: { key: Scope; label: string }[] = [
  { key: 'all', label: '全部' },
  { key: 'annotated', label: '只看标注过的' },
  { key: 'changed', label: '只看有变更' },
];

const human = (n: number) => (n >= 1048576 ? `${(n / 1048576).toFixed(2)} MB` : n >= 1024 ? `${(n / 1024).toFixed(1)} KB` : `${n} B`);
/** 基线来源：散装 / 哪个归档的哪一段（`from` 由 `bin-source.mjs` 给出） */
const sourceOf = (from: string) => {
  if (!from) return '✖ 基线缺失';
  if (from.startsWith('loose:')) return '散装';
  const m = /^alf:([^→]+)→([^@]+)@(\d+)\+(\d+)$/.exec(from);
  return m ? `${m[1]} → ${m[2]} @ ${m[3]}` : from;
};

async function open(name: string | null, kind: ScriptKind) {
  if (!name) {
    view.value = null;
    viewError.value = '';
    return;
  }
  viewLoading.value = true;
  viewError.value = '';
  try {
    view.value = await loadScriptView(name, kind);
  } catch (err) {
    view.value = null;
    viewError.value = (err as Error).message;
  } finally {
    viewLoading.value = false;
  }
}

watch(
  () => [props.name, props.kind] as const,
  ([name, kind]) => {
    void open(name, kind);
    if (props.line) window.setTimeout(() => viewer.value?.goToLine(props.line as number), 300);
  },
  { immediate: true },
);

function doJump() {
  const n = Number(jump.value);
  if (Number.isInteger(n) && n > 0) viewer.value?.goToLine(n);
}
</script>

<template>
  <div class="scripts">
    <aside class="slist">
      <div class="toolbar">
        <input v-model="filter" class="search" type="search" placeholder="按名字过滤（如 SC19 / SN0000 / CONFIG）" aria-label="过滤脚本" />
        <button class="btn" type="button" :disabled="scriptsLoading" @click="loadScripts(true)">刷新</button>
      </div>
      <div class="toolbar">
        <button
          v-for="s in SCOPES"
          :key="s.key"
          class="btn"
          :class="{ active: scope === s.key }"
          type="button"
          :aria-pressed="scope === s.key"
          @click="scope = s.key"
        >
          {{ s.label }}
        </button>
      </div>
      <div class="slist-meta">
        <span v-if="scriptsIndex">{{ rows.length }} / {{ scriptsIndex.count }} 支</span>
        <span v-if="scriptsIndex" class="dim">
          名单：基线根里所有可反汇编的 .BIN（旧管线标注过 {{ scriptsIndex.annotated }} · 有变更 {{ scriptsIndex.hasPatch }} ·
          非脚本 {{ scriptsIndex.nonScript }} 个不在名单里）
        </span>
      </div>
      <p v-if="hidden > 0" class="notice dim">
        <strong>列表只渲染前 {{ RENDER_MAX }} 行</strong>（当前筛选命中 {{ rows.length }} 行）—— 还有 {{ hidden }} 行没渲染：
        请用上面的搜索 / 筛选收窄（这个上限是刻意的，上千行 DOM 会让滚动发涩）。
      </p>
      <p v-if="scriptsError" class="notice err">{{ scriptsError }}</p>
      <p v-else-if="!scriptsIndex" class="empty">正在读取…</p>
      <nav v-else class="srows">
        <!-- 每一行都是真 <a href>：中键 / Ctrl+点击直接开新 tab -->
        <a
          v-for="s in visible"
          :key="s.name"
          class="srow"
          :class="{ active: s.name === name }"
          :href="hrefScript(s.name, kind)"
          :title="`${s.name} · ${sourceOf(s.baseFrom)}${s.annotated ? ' · 旧管线标注过' : ''}`"
        >
          <span class="srow-name mono">{{ s.name }}</span>
          <span class="srow-badges">
            <span v-if="s.hasPatch" class="badge ok">有变更 {{ s.opCount }} op</span>
            <span v-else-if="s.annotated" class="badge">旧管线标注过</span>
          </span>
          <span class="srow-src">{{ sourceOf(s.baseFrom) }}</span>
          <span class="srow-bytes">{{ human(s.baseBytes) }}</span>
        </a>
      </nav>
    </aside>

    <section class="viewer">
      <div v-if="!name" class="empty pad">
        从左边的列表里选一支脚本（<strong>全部可反汇编的 AGE 脚本</strong>，「旧管线标注过」只是标签）。<br />
        <span class="dim">
          每一行都是真链接（中键 / Ctrl+点击可以并排开两支）。<code>data</code> 是基线的反汇编全文，
          <code>src</code> 是基线 + patch（字符串取 patch 里的中文）；<strong>没有 patch 条目</strong>的脚本
          <code>src</code> 与 <code>data</code> 相同。
        </span>
      </div>

      <template v-else>
        <div class="toolbar vbar">
          <span class="mono strong">{{ name }}</span>
          <span class="seg">
            <a :href="hrefScript(name, 'data')" :class="{ active: kind === 'data' }">data</a>
            <a :href="hrefScript(name, 'src')" :class="{ active: kind === 'src' }">src</a>
          </span>
          <span v-if="view" class="dim">
            {{ view.rows }} 行 · {{ human(view.bytes) }} ·
            {{ view.hasPatch ? `patch ${view.stats.stringSubstitutions ?? 0} 处换字符串` : '无条目（src 与 data 相同）' }} ·
            基线 {{ sourceOf(view.baseFrom) }}
          </span>
          <span v-else-if="viewLoading" class="dim">正在算视图…</span>
          <span class="grow"></span>
          <input
            v-model="jump"
            class="search tiny"
            type="text"
            inputmode="numeric"
            placeholder="跳到行"
            aria-label="跳到行"
            @keyup.enter="doJump"
          />
          <button class="btn" type="button" @click="doJump">跳</button>
        </div>

        <p v-if="viewError" class="notice err pad">算不出正文：{{ viewError }}</p>
        <MonacoViewer v-else-if="view" ref="viewer" :text="view.text" />
        <p v-else class="empty pad">正在读取…</p>
      </template>
    </section>
  </div>
</template>
