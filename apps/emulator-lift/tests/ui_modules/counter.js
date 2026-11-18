// 夹具：被两条路径导入的同一模块 —— 顶层副作用只应发生一次（缓存命中）。
export const A = 'counter';
globalThis.__module_evals = (globalThis.__module_evals || 0) + 1;
