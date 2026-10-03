/**
 * src/router.ts — **hash 路由**（`#/` · `#/req/<ref>` · `#/scripts` · `#/script/<name>`）。
 *
 * 为什么是 hash：它**不发给服务器** ⇒ 不需要 History API、不需要服务器 rewrite，
 * 而每一条列表行都可以是**真 `<a href="#/…">`** ⇒ 中键 / `Ctrl`(`Cmd`)+点击开新 tab 是白送的。
 * 浏览器后退键就是"返回"（旧 `apps/requirements` 的同一口径）。
 *
 * 引用解析（`<ref>`）复用台账自己的规则：完整 id / 唯一前缀 / 唯一后缀都能开
 * （`makeResolver`）。ULID 以时间开头、同批前 10 位相同，所以**短名**（末 8 位）才是人能用的那个句柄。
 */

import { shallowRef } from 'vue';

export type ScriptKind = 'data' | 'src';

export type Route =
  | { view: 'reqs' }
  | { view: 'req'; ref: string }
  | { view: 'scripts' }
  | { view: 'script'; name: string; kind: ScriptKind; line: number | null };

export function parseHash(hash: string): Route {
  const raw = hash.replace(/^#/, '');
  const [pathPart, queryPart] = raw.split('?');
  const seg = pathPart.split('/').filter((s) => s !== '');
  const q = new URLSearchParams(queryPart ?? '');
  const dec = (s: string) => {
    try {
      return decodeURIComponent(s);
    } catch {
      return s;
    }
  };
  if (seg[0] === 'req' && seg[1]) return { view: 'req', ref: dec(seg[1]) };
  if (seg[0] === 'scripts') return { view: 'scripts' };
  if (seg[0] === 'script' && seg[1]) {
    const kind: ScriptKind = q.get('kind') === 'data' ? 'data' : 'src';
    const line = Number(q.get('line'));
    return { view: 'script', name: dec(seg[1]), kind, line: Number.isInteger(line) && line > 0 ? line : null };
  }
  return { view: 'reqs' };
}

export const route = shallowRef<Route>(parseHash(window.location.hash));

window.addEventListener('hashchange', () => {
  route.value = parseHash(window.location.hash);
});

export const hrefReqs = () => '#/';
export const hrefReq = (ref: string) => `#/req/${encodeURIComponent(ref)}`;
export const hrefScripts = () => '#/scripts';
export const hrefScript = (name: string, kind: ScriptKind = 'src', line: number | null = null) =>
  `#/script/${encodeURIComponent(name)}?kind=${kind}` + (line ? `&line=${line}` : '');

/** 导航到某个 hash（只在"外部触发"时用；列表现用真 `<a href>`，不要抢它的默认行为） */
export function go(href: string) {
  window.location.hash = href.replace(/^#/, '');
}
