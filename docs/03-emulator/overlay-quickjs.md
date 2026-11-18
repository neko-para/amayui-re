# overlay 的 QuickJS 绑定（宿主最小重现）

> 本文只写**绑定与运行时**的裁决：QuickJS 怎么嵌进 lift、沙箱与预算长什么样、值怎么过边界。
> 产品面（数据模型 / 路由 / 里程碑）见 `overlay.md`；本文⛔ 不写进度，也⛔ 不把估计当判据。
> 外部依据 = **MaaFramework**（只读参考，不是本仓依赖）：可复用的是**封装形状**，不是源码。

## 1. 从 Maa 的封装里取什么、丢什么

Maa 的 NodeJS binding 不是一份绑定，而是一层与语言无关的薄抽象 + 两个后端：
`source/binding/NodeJS/src/foundation/spec/` 定义形状（`QjsValue` / `JSConvert` / `NativeClass` /
`CallbackInfo`），`spec/nodejs/*` 与 `spec/quickjs/*` 各实现一遍。

| 层 | 符号 | 处置 |
|---|---|---|
| 值容器 | `spec/quickjs/wrapper.h:QjsValue`（拷贝 `JS_DupValue`、move 置 `JS_UNDEFINED`、析构 `JS_FreeValue`、`As<T>()` dup、`take()` 转移）+ 子类型 | ✅ 形状照抄：一个 `JSValue` 一个所有者；只取 bool / number / string / array / object |
| 值转换 | `spec/convert.h:JSConvert<T>` 全特化（22 条：bool/string/int32/uint32/int64/**uint64**/uintptr/vector/array/tuple/variant/optional/OptionalParam/NativeObject…）+ `DumpValue` / `TypeOf` | ✅ 只取 bool / number / string / array / object / function（见 §3）；❌ tuple / variant / optional / nativeobject |
| 参数解包 | `spec/wrapper.h:UnWrapArgs` / `OptionalCount`（错误里追加 `Sig:` / `Got:`）；`spec/traits.h:FuncTraits` | ⚠ 函数少，手写 `argc` 检查更短；只留 `Sig:` 诊断那一招 |
| 函数注册 | `spec/utils.h:MAA_BIND_FUNC` → `WrapFunctionHelper` → `spec/quickjs/instric.impl.h:MakeFunction`（`JS_NewCFunctionData` 把 `std::function` 塞进 `func_data[0]`） | ✅ 机制可借；⚠ 别抄它硬编码 `55 /* JS_ATOM_name */`，我们 `JS_NewAtom("name")` |
| 类注册 / GC | `spec/quickjs/class.h:NativeClass<T>::init`（`JS_NewClassID` / `JS_NewClass`、finalizer=delete、gc_mark、`JS_SetConstructor`…） | ❌ 整套不要（没有 host 句柄类）；✅ 只学原理：宿主把 JSValue 要么每次 `JS_DupValue` 临时保，要么挂在 runtime 内对象上被 mark |
| 模块注册 | `apis/loader.cpp:init_module_maa` 把全局 `maa` 挂 `globalThis`，内部逐命名空间建对象 | ✅ 同样挂三个全局对象；❌ ESM |
| 错误 | `spec/tools.h:MaaError : std::exception`（只在本侧 throw/catch，不过 FFI）；跨到 JS = `JS_ThrowTypeError` + 返 `JS_EXCEPTION` | ✅ 同一分工 |
| 生命周期 | `plugin/runtime.cpp`：`JS_NewRuntime` → `JS_NewContext` → `JS_SetDumpFlags(JS_DUMP_LEAKS)` → `JS_SetRuntimeOpaque`；析构 `JS_FreeContext` → `JS_FreeRuntime`；`JS_Eval(..., "index.js", JS_EVAL_FLAG_STRICT)`；`JS_ExecutePendingJob` | ✅ 骨架；❌ job 泵（无 Promise） |
| 多线程 / NAPI / 业务 | `plugin/entry.cpp` 的 detached `std::thread`、`plugin/runtime.cpp` 的 mutex/cv/`push_task`、`AsyncWork`（实在 `spec/quickjs/async.h:14` 与 `spec/nodejs/async.h:10`；`spec/async.h` 只是转发头）、`apis/` 40+ 文件 | ❌ 全丢 |

**一句话**：拿"值所有权 + 转换表 + 一个 C 函数注册口 + 错误分工"，其余（类体系 / 异步 / 多线程 / 业务）都不进。

## 2. QuickJS-ng v0.10.1：来源、预算、沙箱

- 来源 = **quickjs-ng**（不是 bellard 仓），v0.10.1（Maa 仓里 pin 到该 tag；`git submodule status` 可复现），MIT。
  CMake target `qjs` **缺省静态**（`BUILD_SHARED_LIBS` 缺省 OFF；Maa 没有显式 `STATIC` 字样），
  5 个 C 源：`cutils.c libregexp.c libunicode.c quickjs.c xsum.c`。
- 预算 API 三件（0.10.1 已核：`quickjs.h:440` / `:446` / `:1031`；0.16.2 本机没有 checkout ⇒ **未复核**）：
  `JS_SetMemoryLimit` / `JS_SetMaxStackSize` / `JS_SetInterruptHandler`。
- **Maa 没有沙箱也没有预算**：**预算 / 中断面**零使用 —— 用
  `rg 'JS_SetMemoryLimit|JS_SetMaxStackSize|JS_SetInterruptHandler'` 在 `source/binding/NodeJS/src` 与
  `source/ include/ tools/ sample/ test/` 都零命中（生命周期那四个 API 照旧用；另有 `JS_SetOpaque` /
  `JS_SetContextOpaque` / `JS_SetClassProto` / `JS_SetConstructor(Bit)`，都不涉预算 / 中断）；
  且 `apis/loader.cpp:init_module_sys` 直接给脚本 `print/readFile/writeFile/exit`。
  ⇒ 这两面**没有先例可抄**，必须按下面自己立。

### 2.1 沙箱：`JS_NewContextRaw` + 选择性 intrinsic

`JS_NewContext` 与 `JS_NewContextRaw` 的差集 = Date / eval / RegExp / JSON / Proxy / MapSet / TypedArrays /
Promise / BigInt / WeakRef / `performance` 的来源（`quickjs.c:2318-2345` vs `2283-2315`）——其中 JSON / MapSet 按需单独加。我们的形状：

1. `JS_NewContextRaw`（只建原型与空全局对象，不装任何构造器）。
2. 只加 `JS_AddIntrinsicBaseObjects`（Object / Function / Array / String / Number / Boolean / Error 族 /
   Math / Reflect / Iterator / Symbol / `parseInt` / `isNaN`）——本切片**不**加 `MapSet` / `JSON`。
3. ⛔ 不加 `Date`（墙钟）、`RegExp`、`Proxy`、`TypedArrays`、`Promise`、`BigInt`、`WeakRef`、
   `JS_AddPerformance`（`Date.now` 派生）；本切片连 `JSON` / `MapSet` 也不加（HUD 用不到）。
4. ★ **必须加 `JS_AddIntrinsicEval`**，但只为**宿主**能编译脚本：宿主 `JS_Eval` 走
   `JS_EvalThis2` → `JS_EvalInternal`，而它在 `quickjs.c:35012` 先判 `ctx->eval_internal`
   （只由 `JS_AddIntrinsicEval` 设置）⇒ 不加它宿主自己就 `TypeError: eval is not supported`
   （2026-11-18 实测）。**沙箱的门因此不是"不加 Eval"，而是加完之后删干净**：
   `globalThis.eval`、`globalThis.Function`、**`Function.prototype.constructor`**、
   `Math.random`（种子 = `js__gettimeofday_us()`，逐 **context** 不同 ⇒ 不可复现）。
   ★ 只删 `globalThis.Function` **不够**：实测 `(function(){}).constructor('return 1')()`
   在只删全局 `Function` 后仍可用；删掉 `Function.prototype.constructor` 后才抛 `TypeError`。
   `eval` 全局删掉后是 `ReferenceError: eval is not defined`。
   ⇒ 这一段推翻本节早先的"不加 Eval"写法（同一条实测，2026-11-18）。
5. 无 `std` / `os`：那是 `quickjs-libc.c` 的模块；不设 `JS_SetModuleLoaderFunc` ⇒ `import` 失败。
6. 无 host 反射：不学 `NativeObject<T>` / `NativePointerHolder`，⛔ 不给脚本任何 native 指针 / 句柄。

### 2.2 预算：三条线、三个失败面

| 资源 | 设置 | 失败面 |
|---|---|---|
| 内存 | `JS_SetMemoryLimit(rt, N)`（0 = 关）；超限 ⇒ 分配返 NULL ⇒ 抛错 | **插件致命**（OOM 后 ctx 不可信）⇒ 卸载，旧实例继续 |
| 栈 | `JS_SetMaxStackSize(rt, 512 KiB)`（默认 1 MiB；Windows exe 默认栈 reserve 也是 1 MiB ⇒ 不调小会先打穿宿主栈再报溢出；M2a 实值 512 KiB，见 `script_quickjs.cpp`） | 同上 |
| CPU | `JS_SetInterruptHandler(rt, cb, opaque)`：回调返非 0 ⇒ 上游走 `JS_ThrowInternalError("interrupted")` + `JS_SetUncatchableError` ⇒ 脚本 `try/catch` 吞不掉（★ 0.10.1 的 `JS_ThrowInterrupted` 是 `quickjs.c` 的 static，头里没有，⛔ 不要直接调） | 本帧不上屏 + 一次响亮；连续 3 次 ⇒ 卸载 |

★ 中断粒度 = 每 `JS_INTERRUPT_COUNTER_INIT`（10000）条字节码 ⇒ 回调里**只查 deadline**，
⛔ 不要在回调里数 op。

### 2.3 错误：`JS_EXCEPTION` 必须清掉

`JS_Eval` / `JS_Call` 返回 `JS_EXCEPTION` ⇒ 必须 `JS_GetException(ctx)` 取走（读 `message` 与 `stack`），
否则脏异常留在 ctx 上，**下一次**调用会读到上一次的异常。这是本绑定唯一必须背的引用 / 状态纪律。
位置由引擎给：`<file>:<line>:<col>`，`file` 就是我们传的 `origin`。
宿主 C 函数不接 C++ 异常：`catch (...)` → `JS_ThrowTypeError` + 返 `JS_EXCEPTION`；
边界上**没有** longjmp、没有 setjmp。

## 3. `ScriptRuntime` 与值转换

落点 `src/ui/script_quickjs.h`（PIMPL，**不** include `quickjs.h`）+ `src/ui/script_quickjs.cpp`
（**唯一** include `quickjs.h` 的 TU）。接口语言无关，只有值：

```cpp
struct ScriptRuntime {
    virtual std::unique_ptr<UiScript> load(const std::string& src, const std::string& origin,
                                           UiHostApi* api, std::string* err) = 0;
    virtual std::unique_ptr<UiScript> reload(/* 同上 */) = 0;   // 失败不动旧实例
};
class UiScript {   // init / update(dt,frame) / render(UiBuilder&) / on_click / on_pointer
                   // shutdown / setBudget(deadline_ns, op_limit) / lastCallOverran()
};
```

| 接口 | QuickJS 侧 |
|---|---|
| `load` | `JS_NewRuntime` → 三个预算设置 → `JS_NewContextRaw` → 白名单 intrinsic → 注入三表 → `JS_Eval(src,len,origin,JS_EVAL_FLAG_STRICT)` → 存 `render` 句柄（`JS_DupValue`） |
| `callUpdate/Render/Click/Pointer` | `JS_Call(ctx, fn, JS_UNDEFINED, argc, argv)`；`render` 期间 `Impl::builder` 指向本帧 `UiBuilder` |
| `reload` | 先在**新** runtime 上 load 成功再换旧（失败**不动**旧的，`overlay.md` R2） |
| `shutdown` | `JS_FreeContext` → `JS_FreeRuntime`（`JS_DUMP_LEAKS` 只在开发构建开） |
| `setBudget` | deadline / op_limit 写进 `Impl`，interrupt 回调读它 |

**值转换最小集**（全 UTF-8；每步都查返回码）：

| JS | 读 | 写 |
|---|---|---|
| bool | `JS_IsBool` + `JS_VALUE_GET_BOOL` | `JS_NewBool` |
| number | `JS_ToInt32` / `JS_ToFloat64`（-1 = 异常已挂 ctx 上） | `JS_NewInt32` / `JS_NewFloat64` |
| string | `JS_ToCStringLen2(ctx,&len,v,false)` + `JS_FreeCString` | `JS_NewStringLen` |
| array | `length` + `JS_GetPropertyUint32` | `JS_NewArray` + `JS_SetPropertyUint32` |
| object | `JS_GetPropertyStr`（未知键 = `undefined` ⇒ 用默认值，不报错） | `JS_NewObject` + `JS_SetPropertyStr` |
| function | 只**取**不造：只对 `init/update/render/on_click` 存 `JSValue` 句柄 | ⛔ 不给脚本 native 函数指针 / 句柄 |

- **宿主跨调用持有的 JSValue 只在 runtime 内活**：跨调用要 `JS_DupValue`；⛔ 绝不把 `JSValue` 存进 C++ 全局
  （热重载 = 换 runtime ⇒ UAF）。
- ⛔ 无 Promise / async / job 泵：`render` 必须同步出 display list（`overlay.md` D10）。
- `game` 定死为 **getter 字段**：带下标的只能是函数（`local_int(3)` 等）；⛔ 字段与函数两种形态不并存。

## 4. JS 作者体验（M2a 实况）

- 默认 HUD = **外置** `apps/emulator-lift/scripts/ui/hud.js`（UTF-8、单文件、无 `import`、strict；
  入口必须**显式** `globalThis.render = function render() { … }`；`render` 每次只往 display list 追加）。
- 随发行分发 `scripts/ui/{hud.js, ui.d.ts, README.md}`。`--overlay` 的解析顺序：
  ① `<exe 目录>/scripts/ui/hud.js`（发行布局）→ ② `LIFT_UI_SCRIPT_DIR/hud.js`（开发回退，
  CMake 把源码目录以 `target_compile_definitions(... PUBLIC ...)` 注入）→ ③ 硬错（err 点名两个路径）。
- `--ui <path>` 只吃 `.js`；显式优先且隐含 `--overlay`。启动期校验"文件在 + 编译过 + `render` 是函数"，
  失败 = `exit 2` + `<path>:<行>:<列>`。
- `scripts/ui/ui.d.ts` 是作者声明（`declare const ui` / `game`，含 `game.local_frames()`）；
  **不参与构建、不进 `pnpm typecheck`**；本切片**没有** tsconfig / `tsc` 产物 / 漂移守卫
  （`tools/test/ui-dts.test.mjs` 未做，见 `## 未做`）。
- 模板：`scripts/ui/hud.js` 本身就是模板（版式常量 PAD/SIZE/BG/FG 在文件头）。

## 5. 静态链接与发行

- QuickJS **vendor** 进 `apps/emulator-lift/third_party/quickjs-ng`（tag **v0.10.1** / commit
  `3c9afc99…`；MIT；5 个 C 源 + 头 + `LICENSE` + 本仓 `README.md` + 本仓 `CMakeLists.txt` 建 `lift-qjs`）。
  ⛔ 不是 git submodule（本仓 `third_party/` 的惯例 = 原样副本 + README 记录来源 / 清单 / 怎么升级）。
- `lift-qjs` **静态链进 exe**（`emulator-lift-runtime` PUBLIC 链接）⇒ 没开 `--ui` 也在 exe 里。
  ★ **默认 HUD 的 JS 是外置文件**（不编进 exe）：发行时 `scripts/ui/` 与 exe 一起放。
- **版本 pin**：`script_quickjs.cpp` 对 `QJS_VERSION_MAJOR/MINOR/PATCH == 0/10/1` 做 `static_assert`；
  升级要同时改 `third_party/quickjs-ng/README.md`。
- **LTO**：`lift-qjs` 是独立 target，未加 `-flto`（与 `LIFT_AOT_LTO` 的 runtime / AOT 目标分开；
  "vendored C 一起 LTO"是原始设想，本切片**未验证 / 未启用**）。
- **体积 / 首帧开销未实测**（`## 未做`）；⛔ 不把估计写进判据。

## 待核

1. clang-cl 编译：上游只给 MSVC / ClangCL 的 `/wd…` 与 `/experimental:c11atomics`；`check_c_compiler_flag`
   不认它时**静默不生效**（C11 atomics 走 `__STDC_NO_ATOMICS__` 分支）。首次构建要看实际生效了什么。
2. mac：`quickjs/CMakeLists.txt` 对非 MSVC 非 IOS 开 `-Werror`（**目录级 `add_compile_options`**：
   宏 `xcheck_add_c_compiler_flag` 在 `quickjs/CMakeLists.txt:32-38`，`-Werror` 在 `if(NOT MSVC AND NOT IOS)`
   的 `:41-42`；⛔ 不是 target 级）⇒ 新 clang 一条新告警就整仓红；
   放宽只能 `set_property(TARGET qjs APPEND PROPERTY COMPILE_OPTIONS -Wno-error)`（target 选项排在
   目录选项**之后** ⇒ 会生效）。**未实测**。
3. `JSValue` 泄漏：唯一正确形状 = 每个 `JSValue` 有唯一所有者；开发构建开 `JS_DUMP_LEAKS`，产品构建关。
4. GC 与宿主生命周期：宿主只在 runtime 内持句柄，跨调用 `JS_DupValue`；热重载换 runtime 后旧句柄必须失效。
5. 体积 / 首帧开销：仍是估计；实测落 `observations.md`，阈值由读数定。
6. 白名单 intrinsic 是否够：`Array.prototype.map` / `Object.entries` 在 `BaseObjects` 里（已核）；
   `JSON` 是否要加，等 HUD 样例定。

## 复现命令

```powershell
$maa = 'E:\Projects\MAA\MaaFramework'   # 只读参考仓，不在本仓内
git -C $maa submodule status 3rdparty/quickjs              # 3c9afc99… (v0.10.1)
rg -n 'JS_SetMemoryLimit|JS_SetMaxStackSize|JS_SetInterruptHandler' $maa/3rdparty/quickjs/quickjs.h
rg -n 'JS_SetMemoryLimit|JS_SetMaxStackSize|JS_SetInterruptHandler' $maa/source/binding/NodeJS/src   # 零命中
rg -n 'JS_AddIntrinsic' $maa/3rdparty/quickjs/quickjs.h
rg -n 'js_random_init|void JS_AddPerformance|JSContext \*JS_NewContextRaw' $maa/3rdparty/quickjs/quickjs.c
```
