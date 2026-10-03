<script setup lang="ts">
/**
 * 工作台外壳：导航（需求 / 脚本）+ 主题切换 + 路由出口。
 *
 * 路由是 **hash**（见 `src/router.ts`）：hash 不发给服务器 ⇒ 不需要 History API、不需要服务器 rewrite，
 * 而每一条列表行都是真 `<a href="#/…">` ⇒ 中键开新 tab 是白送的。
 *
 * ★ **UI 组件库 = naive-ui**（通用控件：按钮 / 输入 / 下拉 / 表单 / 对话框 / 消息条），
 *   深浅色由**同一个** `themeMode` 驱动：`<html data-theme>` 管我们自己的 CSS 变量，`darkTheme` 管
 *   naive-ui 的 token ⇒ 两者不会一个深一个浅。**不引 GlobalStyle**：全局排版（字体 / body 底色）
 *   仍然只有 `styles.css` 那一份，免得两套全局样式互相盖。
 *
 * ★ `<n-config-provider abstract>`：`abstract` 让它**不渲染包裹 div** —— 没了这个，
 *   `#app` 的 flex 链会多一层、`height: 100%` 那条链就断在中间。`n-dialog-provider` / `n-message-provider`
 *   本来就是 Fragment（不产生盒子），所以布局与"没有组件库时"逐字节相同。
 */
import { computed } from 'vue';
import { NConfigProvider, NDialogProvider, NMessageProvider, darkTheme } from 'naive-ui';

import { route, type ScriptKind } from './router';
import { THEME_LABEL, cycleTheme, isDark, themeMode } from './theme';
import RequirementsOverview from './views/RequirementsOverview.vue';
import RequirementDetail from './views/RequirementDetail.vue';
import ScriptsView from './views/ScriptsView.vue';

const view = computed(() => route.value.view);
const nav = computed(() => (view.value === 'scripts' || view.value === 'script' ? 'scripts' : 'reqs'));
const themeLabel = computed(() => THEME_LABEL[themeMode.value]);
/** naive-ui 的明暗：与我们那份 `data-theme` 同源，切换永远同步 */
const naiveTheme = computed(() => (isDark.value ? darkTheme : null));

const reqRef = computed(() => (route.value.view === 'req' ? route.value.ref : ''));
const scriptName = computed(() => (route.value.view === 'script' ? route.value.name : null));
const scriptKind = computed<ScriptKind>(() => (route.value.view === 'script' ? route.value.kind : 'src'));
const scriptLine = computed(() => (route.value.view === 'script' ? route.value.line : null));
</script>

<template>
  <n-config-provider :theme="naiveTheme" abstract>
    <n-dialog-provider>
      <n-message-provider>
        <header class="bar">
          <div class="bar-main">
            <h1 class="brand">项目<span class="brand-dim">工作台</span></h1>
            <a class="btn" href="#/" :class="{ active: nav === 'reqs' }">需求</a>
            <a class="btn" href="#/scripts" :class="{ active: nav === 'scripts' }">脚本</a>
            <span class="grow"></span>
            <button class="btn" type="button" title="浅色 / 深色 / 跟随系统" @click="cycleTheme()">
              {{ themeLabel }}
            </button>
          </div>
        </header>

        <main class="main">
          <RequirementsOverview v-if="view === 'reqs'" />
          <RequirementDetail v-else-if="view === 'req'" :node-ref="reqRef" :key="reqRef" />
          <ScriptsView v-else :name="scriptName" :kind="scriptKind" :line="scriptLine" />
        </main>
      </n-message-provider>
    </n-dialog-provider>
  </n-config-provider>
</template>
