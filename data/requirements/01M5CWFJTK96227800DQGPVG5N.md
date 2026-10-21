# CORE-FS 成篇（取件 5 条）+ CORE-BOOT 章节骨架（残差显式）

- id: REQ-01M5CWFJTK96227800DQGPVG5N
- type: req
- status: done
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- verify: tools/test/spec.test.mjs#★ 真源：data/spec/{spec,bindings,audit}.json + chapters/*.md 过全部不变量，且试点未登记 = 0
- tags: [engine spec, fs, boot]

## 结果（2026-10-21 落盘）

* `repo:data/spec/chapters/25-fs.md`：5 条 `CORE-FS-*` —— id 两区 / 松散优先（长度 = 实际文件字节数）/ 归档回退（长度 = 登记长度）/ 取件失败一种表现（报消息 + 中断，不返回空）/ 索引装载失败返回标志。
* `repo:data/spec/chapters/05-boot.md`：5 条 `CORE-BOOT-*` —— 入口 / 索引优先 / 索引失败 / 根脚本 / 中止形态；4 条章级残差（闸门判什么、主循环是否返回、定时器回调缺锚、宿主阶段域外）。
* `repo:data/spec/spec.json` 顺序：overview → boot → format → frame → alf → fs → agf → pools → engine-state → config → ops → vm。
* `repo:data/spec/audit.json`：chapters +2（均 bootstrap）、rules +10、`planned` 删 `files`（`repo:data/spec/core/README.md` §2.1 同批改）。

## 边界（别当已核验）

* 10 条全部 `draft`：FS 的 F1/F3/F4/F5 与 BOOT 的 ENTRY / INDEX-FAIL / ABORT-SHAPE 的证据只到"取件形状 / 失败不静默"档，没有真机或第二实现核验。
* `bindings.storage` 全空：`repo:data/shape/types.json` 没有对应类型，⛔ 没编。

## 怎么查

`pnpm tools spec chapters` · `pnpm tools spec show <规则 id>` · `pnpm tools spec validate`。
