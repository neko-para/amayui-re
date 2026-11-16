# shim 层 · 图形侧

- id: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- type: req
- status: doing
- parent: REQ-01M7JHSCSEX4Q58YPY462RNJQW
- tags: [org]

## 收什么

`shim 层 · 图形侧`：`src/shims/` 的 d3d9 / ddraw / d3dx9 / user32(GDI) / gdi32，以及 `src/runtime/` 里属于图形路径的
GPU 光栅、sprite 合成、frame 记录、呈现汇点。

- **缺陷**：画面错、像素缺、合成状态错、对象生命周期（按 EA 认对象 / 引用计数 / 表面回收）。
- **接口保真**：与微软文档不符的返回值 / caps / 状态位。
- **效率**：**图形 shim 的实现成本**（例如 `M-lift-3`：`DrawIndexedPrimitive` 175 µs / `Clear` 5 ms / `Present` 8.4 ms）。

## 不收

- 输入 / 配方（→ `输入与窗口 / 配方`）。
- 音频（→ `音频侧`）。
- 纯"两份文档不一致"（→ `工具 / 测试 / 文档口径`）。
- 发射器 / IR 本身的效率（→ `lift 层 · 效率`）。
