# 清理 06-window.md 对已收口残差 RES-FRAME-EXIT-BOOT 的引用

- id: REQ-01M699CH5C6KWXECQBK2HQWK9P
- type: req
- status: done
- parent: REQ-01M674XHSX2HSFWSKDWZV2C63D
- done_reason: 三处引用已处理：06-window.md 两处正文改为指向 CORE-OPS-FRAME-EXIT，残差清单里的 RES-FRAME-EXIT-BOOT 条目已删（该残差已并入 0x2 的合同）；node tools/spec.mjs --validate 绿。
- tags: [spec, residual]

# 清理 `06-window.md` 对已收口残差 `RES-FRAME-EXIT-BOOT` 的引用

## 为什么会有这张单

`RES-FRAME-EXIT-BOOT` 登记在 `CORE-OPS-FRAME-EXIT`（`0x2`）名下，本轮的 c1 集群把它**收口成合同**并从 `audit.json` 的该规则 `residual[]` 里删掉了
（合同正文写在 `repo:data/spec/chapters/60-ops-frame.md` 的「结束当前脚本」条「成立条件」里）。
但 `repo:data/spec/chapters/06-window.md` 里还有 **三处**引用它：两处正文「见 `RES-FRAME-EXIT-BOOT`」，一处在本章的残差清单里把它列为 unknown。
c1 集群只允许写 `60-ops-frame.md` / `60-ops-render.md`（并行写者隔离），所以这三处留给本章的写者。

## 已经知道什么（本轮取证，带 EA）

- `0x41A96B`：父帧号 = −1（以及"其余负值"）⇒ 抛**退出型异常**（引擎不区分这些负值）。
- `0x41389A`：主循环保护区的**终止支** —— 主窗口句柄非 0 时向它发**关闭请求**（关闭消息，参数 1 / 0）。
- `0x4138B9`：主窗口句柄 = 0 ⇒ **不发**关闭请求、直接返回。
- 窗口收到关闭请求后由窗口过程清理并销毁窗口，随后 `WM_DESTROY` 投递退出消息（这就是当年"服务窗口清理未取证"的落点）。

## 还缺什么

只缺**引用一致性**，不缺事实：

1. `06-window.md` 第 385 行与第 408 行末尾的「（见 `RES-FRAME-EXIT-BOOT`）」应改指 `CORE-OPS-FRAME-EXIT`（合同已在那里）。
2. 第 412 行那条 `RES-FRAME-EXIT-BOOT`（unknown）清单项应删除。

## 判据（什么算做完）

- `rg -n "RES-FRAME-EXIT-BOOT" data/spec` **无命中**；
- `node tools/spec.mjs --validate` 仍 **exit 0**；
- 第 385 / 408 行的两条陈述仍与 `CORE-OPS-FRAME-EXIT` 的正文一致（有窗口 ⇒ 关闭请求；无窗口 ⇒ 不发）。
