# TypeScript 插件：quickts E1 移植 + erasableSyntaxOnly + 实施分片

- id: REQ-01M8EKFSVE4RF8WZW60C5QHV59
- type: req
- status: open
- parent: REQ-01M8EKFAP4QJY9G12K7KB9YEQB
- tags: [emulator-lift, plugin, qjs, typescript, quickts]

## 目标

用户直接分发 `.ts` 插件；运行时不依赖 Node/tsc。做法 = 把 quickts 的 **E1 纯类型擦除** 移植到 vendored quickjs-ng，配 `erasableSyntaxOnly` + `tsc --noEmit` 门禁。

## quickts 是什么

- Bellard QuickJS 2026-06-04 + 少量 commit；parser 里直接解析 TS/JSX；类型边读边擦，enum/参数属性/JSX 直接 lower 成 bytecode；无 transpiler、无 source map。
- 全量补丁 45 hunk / +272 / −6；`__text` +15.8 KB（+3.3%）；parse +3–5%；peak heap +304 B；MIT。
- 支持 E1（type/interface/as/satisfies/import type/泛型/declare/重载/类修饰符等）；不支持装饰器、取值 namespace、accessor、`import =`、legacy `<T>`。

## E1 → quickjs-ng 移植（已验证）

- `quickjs.c` 33 hunk / +187 / −6；`quickjs.h` +18；新文件 `quickts_e1.h` 1,092 行。
- 从干净 vendored 副本 `patch -p1` 可直接套用（dry-run 与真套用都 exit 0）。
- JS 字节码恒等：upstream 5 个 JS + 本仓 `scripts/**.js` 16/16 IDENTICAL；同批 `.js` 在 flags-on 下也 16/16。
- 本仓 `scripts/plugins/examples/*.ts` 9/9 module 求值成功；cases 01–05/07/08/14 与 quickts qjs 输出一致；E2 case 按设计失败。
- 代价：`quickjs.o __text` +8,564 B（+1.66%）；`JSFunctionDef` 零新增字段；flags-off parse 与未移植版相同；行号保真。
- 实际改写点：`static readonly` 的成员修饰符、ng 的 `js_parse_function_decl2`/箭头多传 line/col、ng 类解析的 `define_var`、`BOOL→bool`。

## 宿主集成

- 入口按扩展名喂 `JS_EVAL_FLAG_TYPESCRIPT`；`.js` 不加（避免 `f<a>(b)` 歧义改变 JS 语义）。
- 自定义 `moduleLoad` 每个模块也要加同一 flag；否则 `lib/*.ts` 被当 JS 解析。
- “classic 先试 / 再 `JS_DetectModule`” 的探测要带 flag；`interface` 开头的 `.ts` 才能探出 module。
- `resolvePluginSpec` 加 `.ts`/`.mts` 候选；`locateSpecifier` 加 `.ts` 后缀；`--plugin ./x.ts` 路径形式可直接用。
- 保留 `tsc --noEmit --erasableSyntaxOnly` 门禁（tsc 5.9.3；现有 examples exit 0）；quickts 只 parse，不查类型。

## 边界 / 未证

- enum / 参数属性 / JSX / 装饰器不支持；要 enum 再移植 `quickts_enum.h`（约 +80 行），要 JSX 再补 `quickts_jsx.h` + `js_parse_error_pos`（约 +600 行）。
- 尚未接 CMake/CTest；未在 16 MiB / 512 KiB 栈 / 2 ms 中断的插件沙箱下实测；win32/MSVC 未证。
- `ts_next_gt` 多行 `>` 拆分、深嵌套类型、超长泛型链未压测；错误文案/`TS_MAX_DEPTH 96` 未证。

## 实施分片

- S0 缺陷：hook 泄漏、patch CAS、`--no-s7` 返回 code、render 期 hook mutator 加 `inCallback` 门、probe owner 门。
- S1 引擎：E1 patch 入库 `third_party/quickjs-ng`；CMake 只多一个头；loader 三处 flag；`.ts` 候选。
- S2 API v2：`RunCtx`/`Tick`、`at/once/task`、drain-to-quiescence、timeout reject、Run 互斥。
- S3 local/UI/log/config：`ctx.local` + stale 保护、`ui` 只进 RenderCtx、`log` 全局、config 直读。
- S4 迁移/文档：examples 迁 `.ts`；`plugin.d.ts`/`plugin-runtime.md` 更新；`tsconfig` 开 `erasableSyntaxOnly`。
- S5 平台：win32/MSVC + FIELD 实测；补 enum/JSX 只在有产品需求时。

## 验收（按“有限测试”）

- JS 字节码在 flags off/on 下逐字节相同（关键不变量）。
- 一个 `.ts` 插件 smoke（type/interface/as/satisfies/import type + `start()`）。
- 现有 9 个 examples 全绿；`tsc --noEmit --erasableSyntaxOnly` 绿。
- 1 条 hook ≤1.5%（20 轮 CI 上界）；cheap key 档 ≤ 今天读点 poll；JS 回调有每帧上限。
- 不要求给每个 API 配测试；只保留字节码恒等 + `.ts` smoke 两条关键守卫。
