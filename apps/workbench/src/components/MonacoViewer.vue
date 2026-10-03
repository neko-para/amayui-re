<script setup lang="ts">
/**
 * MonacoViewer — 只读正文的编辑器（虚拟滚动 + 行号 + 查找 + 跳行）。
 *
 * 它只认 `text`：**不在客户端加工指令行**（那份字节由 `buildView` 在服务端产出）。
 * `text` 变化时换 model 的内容；卸载时一起 dispose（model 不 dispose 会一直占内存）。
 *
 * ★ **Monaco 是懒加载的**（`await import('../monaco')`）：它连编辑器带 worker 有几 MB，
 *   而"查看需求"那半边根本用不到它。做成动态 import 之后 Vite 会把它切成单独的 chunk，
 *   只有真的打开"脚本"页时才下载 —— 总览页因此不必为一个可能用不到的编辑器付几 MB。
 *   ⇒ 同理，**不要**把这个 import 提到模块顶层（那会退化成"每个页面都先下 Monaco"）。
 */
import { onBeforeUnmount, onMounted, ref, watch } from 'vue';

import { isDark } from '../theme';

type Monaco = typeof import('../monaco')['monaco'];
type Editor = import('monaco-editor/editor/editor.api.js').editor.IStandaloneCodeEditor;
type Model = import('monaco-editor/editor/editor.api.js').editor.ITextModel;

const props = defineProps<{ text: string }>();

const host = ref<HTMLElement | null>(null);
const lineCount = ref(0);
const loading = ref(true);
const failed = ref<string | null>(null);

let mod: typeof import('../monaco') | null = null;
let monacoRef: Monaco | null = null;
let editor: Editor | null = null;
let model: Model | null = null;

onMounted(async () => {
  try {
    mod = await import('../monaco');
    monacoRef = mod.monaco;
    if (!host.value) return;
    model = monacoRef.editor.createModel(props.text);
    editor = monacoRef.editor.create(host.value, { ...mod.VIEWER_OPTIONS, model });
    mod.setMonacoTheme(isDark.value);
    lineCount.value = model.getLineCount();
  } catch (err) {
    failed.value = err instanceof Error ? err.message : String(err);
  } finally {
    loading.value = false;
  }
});

watch(
  () => props.text,
  (text) => {
    if (!model) return;
    if (model.getValue() !== text) model.setValue(text);
    lineCount.value = model.getLineCount();
    editor?.setScrollPosition({ scrollTop: 0, scrollLeft: 0 });
  },
);

watch(isDark, (dark) => mod?.setMonacoTheme(dark));

onBeforeUnmount(() => {
  editor?.dispose();
  model?.dispose();
  editor = null;
  model = null;
});

/** 给外部工具栏用的"跳到第 N 行"（Monaco 自己也有 Ctrl+G） */
function goToLine(n: number) {
  if (!editor || !model) return;
  const line = Math.min(Math.max(1, n), model.getLineCount());
  editor.revealLineInCenter(line);
  editor.setPosition({ lineNumber: line, column: 1 });
  editor.focus();
}

defineExpose({ goToLine, lineCount, loading, failed });
</script>

<template>
  <div class="monaco-wrap">
    <div v-if="loading" class="monaco-hint">正在加载编辑器…</div>
    <div v-else-if="failed" class="monaco-hint monaco-hint-bad">编辑器加载失败：{{ failed }}</div>
    <div ref="host" class="monaco-host"></div>
  </div>
</template>

<style scoped>
/* 布局归组件自己管（`styles.css` 不再重复一份）：`.viewer` 是 flex column，
   所以"占满剩余高度"这件事必须落在这个 wrapper 上，而不是里面的 host。 */
.monaco-wrap {
  position: relative;
  flex: 1;
  min-height: 0;
  height: 100%;
}
.monaco-host {
  height: 100%;
  min-height: 0;
}
.monaco-hint {
  position: absolute;
  inset: 0;
  display: grid;
  place-items: center;
  font-size: 13px;
  opacity: 0.7;
  pointer-events: none;
}
.monaco-hint-bad {
  opacity: 1;
  color: var(--bad, #c0392b);
}
</style>
