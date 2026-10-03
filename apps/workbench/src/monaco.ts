/**
 * src/monaco.ts — **瘦引入** Monaco（只读编辑器）。
 *
 * ★ 为什么不是 `import * as monaco from 'monaco-editor'`：那个入口（`esm/vs/index.js`）
 *   会把**全部语言**连同它们的语言服务 worker 一起打进来（`ts.worker` 是 MB 级的），
 *   而这里只需要一个"能滚动几万行、能查找、能跳行"的纯文本编辑器。所以：
 *
 *   1. `editor/editor.api.js` —— 只要编辑器 API（不含任何语言）；
 *   2. `features/register.all.js` —— 编辑器**功能**（查找 / 折叠 / 多光标 / 跳行 / 复制粘贴…），
 *      **不含** `languages/**`（那才是语言 worker 的来源）；
 *   3. `editor/editor.worker?worker` —— 编辑器自己的 worker（分词/词级操作）。它很小，
 *      与 ts/css/html/json 那些语言 worker 完全不是一回事。
 *
 * ★ 包名后的子路径按 monaco 0.57 的 `exports` 映射解析：`"./*": "./esm/vs/*.js"`
 *   ⇒ `monaco-editor/editor/editor.api.js` = `esm/vs/editor/editor.api.js`。
 *   （**不是** `monaco-editor/esm/vs/…`：那会被映射成 `esm/vs/esm/vs/…`，解析不到。）
 */
import * as monaco from 'monaco-editor/editor/editor.api.js';
import 'monaco-editor/features/register.all.js';
import EditorWorker from 'monaco-editor/editor/editor.worker?worker';

declare global {
  interface Window {
    MonacoEnvironment?: { getWorker: () => Worker };
  }
}

// 只注册一个 worker 工厂：`features/register.all.js` 不会请求任何语言 worker
window.MonacoEnvironment = { getWorker: () => new EditorWorker() };

export { monaco };

/** 深/浅色跟随页面主题（Monaco 只认 `vs` / `vs-dark`） */
export function setMonacoTheme(dark: boolean) {
  monaco.editor.setTheme(dark ? 'vs-dark' : 'vs');
}

/** 只读反汇编正文的编辑器选项 */
export const VIEWER_OPTIONS: monaco.editor.IStandaloneEditorConstructionOptions = {
  readOnly: true,
  domReadOnly: true,
  automaticLayout: true,
  minimap: { enabled: false },
  scrollBeyondLastLine: false,
  // 大文件（基线根里最大的几支是几万行）优先"能用"：关掉最贵的装饰，保留虚拟滚动 / 行号 / 查找
  largeFileOptimizations: true,
  renderLineHighlight: 'none',
  occurrencesHighlight: 'off',
  selectionHighlight: false,
  stickyScroll: { enabled: false },
  renderWhitespace: 'none',
  wordWrap: 'off',
  smoothScrolling: false,
  folding: true,
  glyphMargin: false,
  fontSize: 12,
  lineNumbersMinChars: 5,
  padding: { top: 6, bottom: 6 },
  fontFamily: "'Cascadia Mono', Consolas, 'Sarasa Mono SC', 'Microsoft YaHei', monospace",
  scrollbar: { verticalScrollbarSize: 12, horizontalScrollbarSize: 12 },
};
