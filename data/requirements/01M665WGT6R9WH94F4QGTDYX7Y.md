# 🐞 ADV 点击大量丢失：等待位在长动画期间长时间阻塞派发（80 击→13 推进）

- id: REQ-01M665WGT6R9WH94F4QGTDYX7Y
- type: bug
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: apps/emulator-cxx/tests/core_selftest.cpp#颜色窗还在走
- repro: docs/02-engine/repro/adv-click-loss.md#观测点
- severity: S1
- tags: [emulator-cxx, adv, input]

## 根因（已取证）

`0x21c` 的等待位放行条件，本仓一直实现成「场景里**没有**任何带 duration 的动画窗」。语料里不是这样：

- **自然放行**（`0x413232`）：消息 / 文本子系统**没有未完成的工作**，**且** 场景里没有
  "状态字 `&5==4`"的对象（`0x412648` 扫 1000 槽 + `sub_4050E0`）。
- **输入跳过**（`0x413331`）：配置 `system:EffectSkipOnClick` 非 0 + 输入字 bit 0x10（**确认键**，
  默认 Enter；该支路的输入刷新不轮询 `VK_LBUTTON`）。
- **颜色窗不在任何一路里**（`0x413210` 那条支路没有窗口 / 插值字段）。

于是每句台词 1800 ms 的窗口淡入把脚本卡住：等待位一直不放行 ⇒ 不 arm `0x72`、不读 `0x108`
⇒ 这段时间的点击**必然全丢**。实测 80 次点击（间隔 300 ms、按住 100 ms）只推进 **13** 次。

## 修法

门的等待位服务改成"消息子系统没工作"：**没有待画格、且不在打字态**就放行；颜色窗不再挡脚本
（`scene_animating_` 仍用于渲染标脏，不再参与门）。

## 验证

同一段时间线（`--install links/install --tick-ms 16`）：正文追加 **34 → 126** 次；ADV 画面正常。
反向守卫：`core_selftest` 里那段「颜色窗还在走时等待位必须已放行」的断言（旧版断言的是反面，
改新语义后旧断言当场红 → 已翻转）。

## 残留

- `CORE-OPS-WAIT-TIMER`：那个计时器的精确递减方式未取证；本版用"待画格 / 打字态"近似。
- `apps/emulator-new` 的等待门仍是旧条件（`scene.animating === 0`），**未同步** —— 下一轮跟进。

## 追加守卫（2026-10-30，第五批）

- **`0x21c` 的位语义**仍由 `core_selftest` 里"颜色窗还在走也必须已放行"那条断言钉住（翻转前
  那条断言会红，已实测）。
- 新增 **`emulator-core-adv-click-rate`**（资产档，需要安装）：同一段时间线里 **80 次点击
  （每 300 ms、按住 100 ms）必须推进 ≥ 60 次正文**。实测：修好 ≈126 次；把等待位退回
  "场景无动画" ⇒ **35 次**，本条红。
- 新增 4 条**路由** CTest（`route-title` / `-gamestart` / `-gamestart-newgame` /
  `-newgame-chapter0`）：`data/routes/screens.json` 里那些脚本化点击时间线以前**从没真跑过**，
  现在每条都要求真的到达目标脚本。它们不单独区分"点击被吞"（点击稀疏时旧实现也能过），
  所以点击节奏另由上一条守。
