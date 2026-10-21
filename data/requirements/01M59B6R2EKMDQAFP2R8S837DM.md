# spec 层 P0 · 可校验绑定基建 + global/local 池与操作数首批合同

- id: REQ-01M59B6R2EKMDQAFP2R8S837DM
- type: req
- status: done
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- verify: tools/test/spec.test.mjs#★ 真源：data/spec/{spec,bindings,audit}.json + chapters/*.md 过全部不变量，且试点未登记 = 0
- tags: [engine spec]

## 背景

方法单（REQ-01M5851Z51V1MTSWH414M7WN5Q）的 A–F 尚未定案；本轮只落**第一个可判定试点**：
把"精化绑定 + 处理状态 + 残差 / 未登记义务"做成可执行基建，并用**引擎 global 池 / 局部池 / 操作数原语**这一簇已有有效证据的内容做首批正文。

## 判据

1. `pnpm tools spec validate` 的不变量全绿（条数见 `pnpm tools spec describe`）；`pilot` 的 **effect 级未登记 = 0**（effect 由语料机械枚举；内部省略与残差都必须有理由 / `next`）。
2. 语料侧对账（assets 档守卫）：`pilot.sites` 的块入口 EA 集合等于五个操作数原语跳转表的 case + default 块；`covers` 是 effect 选择器，effect 从 `.lst` 机械枚举并被 covered/internal/residual 全部归属；每条规则的站点 EA 落在指令行首；分派指令 / default 标签 / 函数真的存在。
3. 守卫有判别力：每个不变量都有一条负例样本会红；`tools/mutate-check.mjs` 里有会红的变异（`--list` 现查）。
4. 首批正文覆盖：global 值族 / 引用族、local 六族、整型编解码、字符串 SSO、不支持类型抛异常、容量与越界口径；每条都有状态对应 + 证据（`KN:` / `guard:`）+ 残差。
5. effect 级账：`pilot.internal` 的寄存器 / 栈 / 控制流省略逐类给理由；`pilot.residuals` 覆盖 type 0/1/2 的读语义；报告不给百分数。
6. 规范正文**自包含**：`repo:data/spec/chapters/*.md` 不出现汇编引用 / 台账 id / 守卫 / 本仓路径；复核信息只进 `bindings.json` / `audit.json`，不进入分发正文。

## 非目标（另单跟踪）

* type 0/1/2（立即数 / 浮点立即数 / 镜像内联串）的读语义：已登记残差，未写合同。
* 容器句柄 0x8003/5/9/B：整章 `none`。
* 其余章节骨架：只立问题与残差。

## 怎么查

`pnpm tools spec validate` · `report` · `effects` · `chapters` · `show <id>` · `describe`；落点 `data/spec/`（manifest + `repo:data/spec/chapters/*.md` + 内部 bindings/audit；不打包单文件）。
