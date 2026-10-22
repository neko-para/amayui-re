/// <reference types="vite/client" />

/**
 * Vite 的 `?url` 资产导入（字体文件在随仓字体资产目录，不在 renderer 的 root 内）。
 * `vite/client` 已声明大多数后缀；这里显式收窄到 ttf，避免依赖它的版本细节。
 */
declare module '*.ttf?url' {
  const src: string;
  export default src;
}

/** 影片解码器（pl_mpeg 包装）的 WASM 资产；用 `?url` 让 Vite 复制到 dist。 */
declare module '*.wasm?url' {
  const src: string;
  export default src;
}
