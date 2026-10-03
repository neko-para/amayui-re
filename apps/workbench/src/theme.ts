/**
 * src/theme.ts — 配色：**跟随系统 + 手动切换**（light / dark / system），存在 localStorage。
 *
 * 与 DSH 的主题 token **无关**（刻意如此：两边不会一起打开，偶合版本只会互相拖累）。
 * 浅/深色由 `<html data-theme="light|dark">` 驱动，CSS 变量在 `styles.css` 里各写一份；
 * `isDark` 额外把"当前实际上是不是深色"告诉 Monaco（它只认 `vs` / `vs-dark`）。
 */
import { computed, ref, watch } from 'vue';

export type ThemeMode = 'system' | 'light' | 'dark';

const KEY = 'workbench.theme';
const ORDER: ThemeMode[] = ['system', 'light', 'dark'];
export const THEME_LABEL: Record<ThemeMode, string> = {
  system: '◐ 跟随系统',
  light: '☀ 浅色',
  dark: '☾ 深色',
};

const media = window.matchMedia('(prefers-color-scheme: dark)');
const systemDark = ref(media.matches);
media.addEventListener('change', (e) => {
  systemDark.value = e.matches;
});

function read(): ThemeMode {
  const saved = localStorage.getItem(KEY) ?? '';
  return (ORDER as string[]).includes(saved) ? (saved as ThemeMode) : 'system';
}

export const themeMode = ref<ThemeMode>(read());
export const isDark = computed(() => (themeMode.value === 'system' ? systemDark.value : themeMode.value === 'dark'));

function apply(mode: ThemeMode) {
  const root = document.documentElement;
  if (mode === 'system') root.removeAttribute('data-theme');
  else root.setAttribute('data-theme', mode);
}

watch(themeMode, apply);
apply(themeMode.value);

export function cycleTheme() {
  const next = ORDER[(ORDER.indexOf(themeMode.value) + 1) % ORDER.length];
  themeMode.value = next;
  localStorage.setItem(KEY, next);
}
