<script setup lang="ts">
/**
 * 复制**完整 id**（不是显示用的短名）。
 *
 * 这一对是刻意的：26 位 ULID 摆在行里对人不提供有效信息，但**粘进终端**要的就是它
 * （`pnpm tools requirements show REQ-…`）。所以行里显示 8 位短名，完整 id 由 `⧉` 一键复制。
 */
import { ref } from 'vue';

const props = withDefaults(defineProps<{ id: string; label?: string; wide?: boolean }>(), { label: '⧉', wide: false });

const state = ref<'idle' | 'ok' | 'fail'>('idle');
let timer = 0;

/** clipboard API 只在安全上下文有；http + 非 localhost（`--host` 暴露到局域网）时用兜底 */
async function copyText(text: string): Promise<boolean> {
  try {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(text);
      return true;
    }
  } catch {
    /* 落到兜底 */
  }
  try {
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.setAttribute('readonly', '');
    ta.style.position = 'fixed';
    ta.style.top = '-1000px';
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

async function onCopy() {
  const ok = await copyText(props.id);
  state.value = ok ? 'ok' : 'fail';
  window.clearTimeout(timer);
  timer = window.setTimeout(() => {
    state.value = 'idle';
  }, 1200);
}
</script>

<template>
  <button
    class="copy"
    :class="{ wide, ok: state === 'ok', fail: state === 'fail' }"
    type="button"
    :title="state === 'fail' ? '复制失败 —— 请手动选中 id（悬停短名可见完整 id）' : `复制完整 id：${id}`"
    @click.stop.prevent="onCopy"
  >
    {{ state === 'ok' ? '✓' : state === 'fail' ? '✗' : label }}
    <span v-if="wide"> 复制 id</span>
  </button>
</template>
