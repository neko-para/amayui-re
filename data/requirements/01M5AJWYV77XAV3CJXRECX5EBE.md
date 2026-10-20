# spec 分类与文档整理：失效指针 / 矛盾口径 / 迁移表

- id: REQ-01M5AJWYV77XAV3CJXRECX5EBE
- type: req
- status: done
- parent: REQ-01M5AJWPM4DXWG3XR6W7JR6XHD
- order: 5
- done_reason: 文档类：4 个 doc 已改（失效指针/口径统一/废弃标注），spec validate + pnpm test 全绿；章节正文的口径矛盾由外层本轮修掉；剩余基建项转二批单
- tags: [engine spec docs]

## 目标

整理 spec 的分类与文档（**只动文档，不动规范正文内容与 JSON 数据**）：

1. **失效指针**：`chapters/README.md` 的 `20-pools.md`（实为 `40-pools.md`）；分发物引用 `../audit.json`（不分发）；
   `spec.md` 里关于 `bindings.json` 键名的说明（实际是 `bindings[].rule`）。
2. **矛盾口径**：`chapters/README.md` 说指令表由 `age-format` 规定 vs `10-format.md` 说规范自持 —— 统一成一种。
3. **迁移表**：现有 6 章 → `data/spec/core/README.md` 的机制目录（只给表，不挪文件）。
4. `data/spec/README.md` 加 `core/` 指针；`apps/emulator/README.md` 加"废弃 / 仅架构参考"标注。
5. 报告需求树里引用了不存在文件的节点（例：`data/spec/engine.json`），交外层修（不直接改需求单）。

## 判据

* `pnpm test` 绿；`pnpm tools spec validate` 绿。
* 被整理的文件里不再有"指向不存在的文件 / 键"的句子；矛盾口径只剩一种。
* 迁移表覆盖现有 6 章，每章恰好落一个机制（`00-overview` 明确标为"约定"，不属于任何机制）。

## 非目标

不改 `chapters/*.md` 的规范内容；不动 `bindings.json` / `audit.json`；不重命名 / 移动文件。
