/**
 * src/markdown.ts — 需求正文的 **Markdown → HTML**（★ 工作台里唯一一处渲染）。
 *
 * 为什么要引库：台账正文本来就是 Markdown（`**粗体**` / 表格 / 列表 / 行内代码），而之前页面是把
 * 小节文本原样塞进 `<pre>` —— 于是满屏 `**` 与 `|`。渲染是纯显示层的事，所以放在客户端，
 * 服务端照旧只给**文本**（`/api/node` 的 `sections[].text` 一个字节都不改）。
 *
 * ★ **口径（三条，都是刻意的，改这里等于改契约 —— `smoke.ts` 直接断言它们）**：
 *   ① `html: false`：正文里的原始 HTML **按文本显示**。这一条同时解决了"要不要 sanitize"：
 *      **不开 HTML 就不需要 sanitizer**（markdown-it 只在文本节点上转义 `<`/`>`/`&`，
 *      `javascript:` 之类的 href 它自己也挡）。
 *   ② `linkify: false`：不把裸文本自动变成链接（正文里 `1.2.3` / `BIN` 这类写法太多，
 *      自动链接只会制造假链接）。
 *   ③ **仓库内路径不做成可点的链接**：正文里的 `docs/…md` 是**仓库里的文件**，而工作台
 *      **不服务仓库文件**，做成 `<a href>` 只会点出一个 404。所以相对路径去掉 `href`
 *      （没有 `href` 的 `<a>` 不可点、也进不了 tab 序，语义上就是个带样式的 span），
 *      外链（`http(s):` / `mailto:`）才做成新 tab 打开的真链接。
 *   ④ 小节正文里的小标题**降两级**：页面把小节标题渲染成 `<h4>`（卡片标题是 `<h3>`），
 *      正文里的 `#`/`##`/`###` 于是落到 `h4`/`h5`/`h6` —— 否则正文标题会比卡片标题还大。
 *
 * 副作用：这是**同构**的（不碰 DOM），所以 `smoke.ts` 能在 Node 里直接 import 这个文件断言上面四条。
 */
import MarkdownIt, { type MarkdownIt as MarkdownItInstance } from 'markdown-it';

/** 渲染口径（见文件头 ①②） */
export const MD_OPTIONS = { html: false, linkify: false, breaks: false } as const;

/** 真链接（新 tab 打开）：只有带协议的那些；其余一律当"仓库内路径"处理 */
const EXTERNAL = /^(https?:|mailto:)/i;

/** 小节正文里的小标题降两级（文件头 ④） */
const HEADING_SHIFT = 2;

function build(): MarkdownItInstance {
  const md = new MarkdownIt({ ...MD_OPTIONS });

  md.core.ruler.push('shift_headings', (state) => {
    for (let i = 0; i < state.tokens.length; i += 1) {
      const open = state.tokens[i];
      if (open.type !== 'heading_open') continue;
      const tag = `h${Math.min(Number(String(open.tag).slice(1)) + HEADING_SHIFT, 6)}`;
      open.tag = tag;
      const close = state.tokens[i + 2]; // heading_open / inline / heading_close
      if (close && close.type === 'heading_close') close.tag = tag;
    }
    return true;
  });

  // 覆盖 `link_open`（**不是**新增规则）：默认实现就是 `self.renderToken(...)`，这里只多改两个属性
  md.renderer.rules.link_open = (tokens, idx, options, _env, self) => {
    const token = tokens[idx];
    const href = String(token.attrGet('href') ?? '');
    if (EXTERNAL.test(href)) {
      token.attrSet('target', '_blank');
      token.attrSet('rel', 'noreferrer noopener');
    } else {
      token.attrs = (token.attrs ?? []).filter(([k]) => k !== 'href');
      token.attrSet('class', 'md-ref');
      token.attrSet('title', '仓库内路径 —— 工作台不服务仓库文件，请在编辑器里打开');
    }
    return self.renderToken(tokens, idx, options);
  };

  return md;
}

const md = build();

/** 渲染一段 Markdown（空输入给空串；调用方自己决定要不要套容器） */
export function renderMarkdown(text: string | null | undefined): string {
  const src = String(text ?? '');
  return src.trim() === '' ? '' : md.render(src);
}
