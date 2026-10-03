/**
 * src/data.ts — 取数的小仓库（模块级单例）。
 *
 * 只做两件事：**缓存一次**（需求树 / 脚本一览在一次会话里不会变，变了按"刷新"重取）
 * 与**把错误变成可显示的状态**。聚合数字一律不在这里算 —— 那是 `tools/lib/requirements.mjs`
 * 在服务端算好的（与 `pnpm tools requirements plan` 同一份）。
 */
import { ref } from 'vue';

import {
  getNode,
  getScript,
  getScripts,
  getTree,
  type NodePayload,
  type ScriptKind,
  type ScriptPayload,
  type ScriptsPayload,
  type TreePayload,
} from './api';

// ── 需求 ─────────────────────────────────────────────────────────────

export const tree = ref<TreePayload | null>(null);
export const treeError = ref('');
export const treeLoading = ref(false);

export async function loadTree(force = false): Promise<void> {
  if (treeLoading.value) return;
  if (tree.value && !force) return;
  treeLoading.value = true;
  treeError.value = '';
  try {
    tree.value = await getTree();
  } catch (err) {
    treeError.value = (err as Error).message;
  } finally {
    treeLoading.value = false;
  }
}

/** 详情：**一律走 `/api/node`**（小节由服务端按模型的口径切好，客户端不重写"什么是一节"）。 */
export const nodeLoading = ref(false);

export async function loadNode(ref: string): Promise<NodePayload | null> {
  nodeLoading.value = true;
  try {
    return await getNode(ref);
  } finally {
    nodeLoading.value = false;
  }
}

// ── 脚本 ─────────────────────────────────────────────────────────────

export const scriptsIndex = ref<ScriptsPayload | null>(null);
export const scriptsError = ref('');
export const scriptsLoading = ref(false);

export async function loadScripts(force = false): Promise<void> {
  if (scriptsLoading.value) return;
  if (scriptsIndex.value && !force) return;
  scriptsLoading.value = true;
  scriptsError.value = '';
  try {
    scriptsIndex.value = await getScripts();
  } catch (err) {
    scriptsError.value = (err as Error).message;
  } finally {
    scriptsLoading.value = false;
  }
}

/**
 * 正文缓存：最大的一支 ~2 MB，只留最近几支。
 * ★ 服务端也有缓存（按 patch 与基线 BIN 的 mtime+size 失效），这里只是省一次 HTTP。
 */
const VIEW_KEEP = 4;
const views = new Map<string, ScriptPayload>();

export async function loadScriptView(name: string, kind: ScriptKind): Promise<ScriptPayload> {
  const key = `${name}\u0000${kind}`;
  const hit = views.get(key);
  if (hit) {
    views.delete(key);
    views.set(key, hit);
    return hit;
  }
  const payload = await getScript(name, kind);
  views.set(key, payload);
  while (views.size > VIEW_KEEP) views.delete(views.keys().next().value as string);
  return payload;
}
