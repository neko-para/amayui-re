# macOS 上插件/发行 pure 测试 11 条红：路径校验按 POSIX 语义跑 Windows 盘符输入

- id: REQ-01M59BKHSJT0JR7HKZVN825TTN
- type: bug
- status: done
- parent: REQ-01M3TCXAQ0BZEJ1MMM0N9ZMB8Y
- verify: plugins/deploy/test/ops.test.mjs#★ 路径校验
- repro: plugins/deploy/test/ops.test.mjs#★ 路径校验
- severity: S3
- tags: [macos compat plugins]

## 现象

在 macOS 上 `pnpm test`（@env pure 档）有 11 条失败，全部落在**未改动的插件/发行测试**里：

* `plugins/deploy/test/ops.test.mjs`（2 条）与 `plugin.test.mjs`（1 条）
* `plugins/pnpm-priv/test/ops.test.mjs`（7 条）
* `tools/test/release.test.mjs`（1 条，`install` 落盘中途出错的 mock）

失败形态：测试用 Windows 盘符路径（`E:\repo\...`）做**路径校验**的输入，而实现用 `node:path` 的平台默认实现；
在 POSIX 上 `path.isAbsolute('E:\\...')` 为 false、`path.resolve` 也不按盘符解析 ⇒ 校验分支走不到，断言失败。
判据：`pnpm test` 的失败文件清单 + 直接跑 `node --test plugins/deploy/test/ops.test.mjs`。

## 影响

* 这些失败**不涉及** spec / shape / 语料；它们只影响"macOS 上默认门禁是否全绿"。
* 按项目口径 **win32 优先**、macOS 只做兼容性验证、不阻塞任何一轮 ⇒ 不把它们当阻塞项，但要让它**可见**（本单）。

## 判据（修好）

* `pnpm test` 在 macOS 上 0 条失败；且 Windows 侧行为不变（盘符根 / 相对路径 / 越界仍各自报对。
* 实现侧的选择：要么显式用 `path.win32` / 平台适配层，要么把测试输入改成平台无关形态并保留等价断言。

## 非目标

不改变插件的闭接口 / 安全口径；不为了绿而放宽路径校验。
