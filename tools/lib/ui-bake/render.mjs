/**
 * tools/lib/ui-bake/render.mjs —— 纯工具：headless Chrome 渲染（把 HTML 渲成透明底 PNG）
 *
 * ## 为什么必须用浏览器
 * 旧仓的产物是 **Chrome 渲的**（字形光栅化、`-webkit-text-stroke` 的描边、`text-shadow` 的模糊、
 * `background-clip:text` 的渐变落字，都是 Chrome 的实现）。要"偏差几乎为 0"，就得用同一个光栅化器
 * —— 换成 node-canvas / 自绘字体都会在边缘像素上差开。
 *
 * ## 已实测的三条口径（不要凭印象改）
 *   ① **`--headless=old`**：`--headless` 与 `--headless=new` 在 Windows 上会**真的开一个窗口**
 *      （旧新 headless 都走完整浏览器进程）；`=old` 不弹窗。三者产物**逐字节相同**（SO002 实测）。
 *   ② `--force-device-scale-factor=1` + `--window-size=W,H` + `--default-background-color=00000000`
 *      ⇒ 输出与画布同尺寸、透明底、无缩放。
 *   ③ 同一份 HTML 重复渲染**产物逐字节相同**（确定性，SO002 连渲 3 次实测）。
 *
 * ## 为什么不捕获子进程输出
 * 受限沙箱里 `stdio: 'pipe'` 要开命名管道 ⇒ `spawn EPERM`（见 `AGENTS.md` §5）。
 * 这里用 `stdio: 'ignore'` + 退出码判断，Chrome 的报错另有 `--enable-logging` 太吵，不取。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { execFileSync } from 'node:child_process';

/** Chrome/Edge 的常见落点（按顺序取第一个存在的） */
const CANDIDATES = [
  'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
  'C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe',
  `${process.env.LOCALAPPDATA ?? ''}\\Google\\Chrome\\Application\\chrome.exe`,
  'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe',
  'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/usr/bin/google-chrome',
  '/usr/bin/chromium',
];

/** 找到浏览器可执行文件；`UI_BAKE_CHROME` 环境变量优先 */
export function findChrome(explicit) {
  const cands = [explicit, process.env.UI_BAKE_CHROME, ...CANDIDATES].filter(Boolean);
  for (const c of cands) {
    try {
      if (fs.statSync(c).isFile()) return c;
    } catch {
      /* 继续找 */
    }
  }
  throw new Error(`找不到 Chrome/Edge；用 --chrome <路径> 或环境变量 UI_BAKE_CHROME 指定。找过：\n  ${cands.join('\n  ')}`);
}

/** HTML 里的本地字体声明（headless 对拉丁字形会 fallback 到系统字体，必须显式 @font-face） */
export function fontFaceCss(ttfRegular, ttfBold, family = 'Sarasa Gothic SC') {
  const url = (p) => `file:///${path.resolve(p).replace(/\\/g, '/')}`;
  const lines = [];
  if (ttfRegular) lines.push(`@font-face { font-family: "${family}"; src: url("${url(ttfRegular)}"); font-weight: 400; }`);
  if (ttfBold) lines.push(`@font-face { font-family: "${family}"; src: url("${url(ttfBold)}"); font-weight: 700; }`);
  return lines.join('\n');
}

/**
 * 渲染一个 HTML 文件为 PNG。
 * @param {object} o
 * @param {string} o.htmlPath   已写好的 HTML 文件
 * @param {number} o.width
 * @param {number} o.height
 * @param {string} o.outPath    输出 PNG
 * @param {string} [o.chrome]   Chrome 路径
 */
export function renderHtml({ htmlPath, width, height, outPath, chrome }) {
  const bin = findChrome(chrome);
  fs.mkdirSync(path.dirname(outPath), { recursive: true });
  if (fs.existsSync(outPath)) fs.rmSync(outPath);
  const url = `file:///${path.resolve(htmlPath).replace(/\\/g, '/')}`;
  const args = [
    '--headless=old',
    '--disable-gpu',
    '--no-sandbox',
    '--hide-scrollbars',
    '--force-device-scale-factor=1',
    '--default-background-color=00000000',
    `--window-size=${width},${height}`,
    `--screenshot=${path.resolve(outPath)}`,
    url,
  ];
  try {
    execFileSync(bin, args, { stdio: 'ignore' });
  } catch (err) {
    throw new Error(`headless 渲染失败（${bin}）：${err.message}`);
  }
  if (!fs.existsSync(outPath)) throw new Error(`headless 渲染没有产出文件：${outPath}`);
  return outPath;
}

/** 透明底 HTML 骨架（画布 W×H，正文即文字层） */
export function htmlShell({ width, height, headCss, body }) {
  return `<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<style>
html, body { margin:0; padding:0; width:${width}px; height:${height}px; background:transparent; overflow:hidden; }
${headCss}
</style>
</head>
<body>
${body}
</body>
</html>
`;
}
