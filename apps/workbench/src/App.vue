<script setup lang="ts">
/**
 * 工作台外壳：导航（需求 / 脚本）+ 主题切换 + 路由出口。
 *
 * 路由是 **hash**（见 `src/router.ts`）：hash 不发给服务器 ⇒ 不需要 History API、不需要服务器 rewrite，
 * 而每一条列表行都是真 `<a href="#/…">` ⇒ 中键开新 tab 是白送的。
 */
import { computed } from 'vue';

import { route, type ScriptKind } from './router';
import { THEME_LABEL, cycleTheme, themeMode } from './theme';
import RequirementsOverview from './views/RequirementsOverview.vue';
import RequirementDetail from './views/RequirementDetail.vue';
import ScriptsView from './views/ScriptsView.vue';

const view = computed(() => route.value.view);
const nav = computed(() => (view.value === 'scripts' || view.value === 'script' ? 'scripts' : 'reqs'));
const themeLabel = computed(() => THEME_LABEL[themeMode.value]);

const reqRef = computed(() => (route.value.view === 'req' ? route.value.ref : ''));
const scriptName = computed(() => (route.value.view === 'script' ? route.value.name : null));
const scriptKind = computed<ScriptKind>(() => (route.value.view === 'script' ? route.value.kind : 'src'));
const scriptLine = computed(() => (route.value.view === 'script' ? route.value.line : null));
</script>

<template>
  <header class="bar">
    <div class="bar-main">
      <h1 class="brand">项目<span class="brand-dim">工作台</span></h1>
      <a class="btn" href="#/" :class="{ active: nav === 'reqs' }">需求</a>
      <a class="btn" href="#/scripts" :class="{ active: nav === 'scripts' }">脚本</a>
      <span class="grow"></span>
      <button class="btn" type="button" title="浅色 / 深色 / 跟随系统" @click="cycleTheme()">{{ themeLabel }}</button>
    </div>
  </header>

  <main class="main">
    <RequirementsOverview v-if="view === 'reqs'" />
    <RequirementDetail v-else-if="view === 'req'" :node-ref="reqRef" :key="reqRef" />
    <ScriptsView v-else :name="scriptName" :kind="scriptKind" :line="scriptLine" />
  </main>
</template>
