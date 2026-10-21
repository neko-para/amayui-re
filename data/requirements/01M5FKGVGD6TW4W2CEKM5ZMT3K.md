# CORE-OBJECTS 内容批次：对象身份 / 拥有关系 / 1000 槽表

- id: REQ-01M5FKGVGD6TW4W2CEKM5ZMT3K
- type: req
- status: open
- parent: REQ-01M59B6Y8VGCZJN79XVQGHB390
- tags: [spec, objects]

## 一句话

机制表里的 `CORE-OBJECTS` 至今**没有需求子单**，内容也没成篇。本单只做第一块可判定的合同：**对象是什么 / 谁拥有谁 / 可观察身份**，不追求一次写完对象模型。

## 闭集（先回答这些）

* 对象身份：一个对象在引擎里由什么**可观察**的东西确定（不是"它占哪块内存"）。
* 拥有关系：谁分配 / 谁释放 / 生命周期跟着谁。
* 1000 槽表：它回答什么、谁读写它；表项身份未定就先记残差。

## 判据

* 至少 3 条合同（构造 / 生命周期 / 可观察身份），每条有 `shape:` 指针与 EA 站点；
* 1000 槽表要么给合同，要么进 `audit` 残差（带 next）；
* `pnpm tools spec validate` 13/13；新增规则保持 `draft`（除非有 accepted 观察）。

## 怎么查

`pnpm tools shape claims --sample` · `pnpm tools shape census` · `pnpm tools spec chapters`。
