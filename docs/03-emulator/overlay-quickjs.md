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
| 模块注册 | `apis/loader.cpp:init_module_maa` 把全局 `maa` 挂 `globalThis`，内部逐命名空间建对象 | ✅ 同样挂三个全局对象 `ui` / `game` / `host`；✅ ESM（module loader 按入口目录解析相对 specifier，见 §3） |
| 错误 | `spec/tools.h:MaaError : std::exception`（只在本侧 throw/catch，不过 FFI）；跨到 JS = `JS_ThrowTypeError` + 返 `JS_EXCEPTION` | ✅ 同一分工 |
| 生命周期 | `plugin/runtime.cpp`：`JS_NewRuntime` → `JS_NewContext` → `JS_SetDumpFlags(JS_DUMP_LEAKS)` → `JS_SetRuntimeOpaque`；析构 `JS_FreeContext` → `JS_FreeRuntime`；`JS_Eval(..., "index.js", JS_EVAL_FLAG_STRICT)`；`JS_ExecutePendingJob` | ✅ 骨架；✅ job 泵（`JS_ExecutePendingJob` drain，只服务 module 求值 / 关闭；`Promise` 加而不删，见 §2.1） |
| 多线程 / NAPI / 业务 | `plugin/entry.cpp` 的 detached `std::thread`、`plugin/runtime.cpp` 的 mutex/cv/`push_task`、`AsyncWork`（实在 `spec/quickjs/async.h:14` 与 `spec/nodejs/async.h:10`；`spec/async.h` 只是转发头）、`apis/` 40+ 文件 | ❌ 全丢 |

**一句话**：拿"值所有权 + 转换表 + 一个 C 函数注册口 + 错误分工"，其余（类体系 / 多线程 / 业务）都不进；异步只留 module 求值的 job drain（⛔ 不做跨帧协程，见 §3）。

## 2. QuickJS-ng v0.18.0：来源、预算、沙箱

- 来源 = **quickjs-ng**（不是 bellard 仓），**v0.18.0**（tag `v0.18.0` / commit
  `d7215b0ad3fc2c562e8317fd9bca058fd4863804`；本仓是它的逐字节副本，来源 / 清单 / 怎么升级见
  `third_party/quickjs-ng/README.md`），MIT。
  CMake target `qjs` **缺省静态**（`BUILD_SHARED_LIBS` 缺省 OFF），
  4 个 C 源：`dtoa.c libregexp.c libunicode.c quickjs.c`。★ 0.10.1 的 `cutils.c` 在 0.18.0 **没有了**
  （实现以 `static inline` 落进 `cutils.h`）；`xsum.c` / `xsum.h` 上游在 0.18.0 **删掉了**
  （tag 树里没有，raw 上 404）⇒ 我们也删；`dtoa.c` 是 0.18.0 新拆出的独立 TU。
- 预算 API 三件（0.18.0 已核：`quickjs.h:515` / `:521` / `:1193`）：
  `JS_SetMemoryLimit` / `JS_SetMaxStackSize` / `JS_SetInterruptHandler`。
- **Maa 没有沙箱也没有预算**：**预算 / 中断面**零使用 —— 用
  `rg 'JS_SetMemoryLimit|JS_SetMaxStackSize|JS_SetInterruptHandler'` 在 `source/binding/NodeJS/src` 与
  `source/ include/ tools/ sample/ test/` 都零命中（生命周期那四个 API 照旧用；另有 `JS_SetOpaque` /
  `JS_SetContextOpaque` / `JS_SetClassProto` / `JS_SetConstructor(Bit)`，都不涉预算 / 中断）；
  且 `apis/loader.cpp:init_module_sys` 直接给脚本 `print/readFile/writeFile/exit`。
  ⇒ 这两面**没有先例可抄**，必须按下面自己立。

### 2.1 沙箱：`JS_NewContextRaw` + 选择性 intrinsic（★ **不是安全边界**）

★ **沙箱不是安全边界**（用户裁决 2026-11-18：JS 没有宿主权限 —— 它拿到的能力只来自我们注入的
`ui` / `game` / `host`，所以**信任脚本**）。本节的口径因此是：

1. **保留的删除只有功能目的**：删 `eval` / `Function` / `Function.prototype.constructor`
   = 禁**动态编译**；删 `Math.random` = 去掉不可复现的随机源（种子是宿主时间）。
   ⛔ 它们**不是**为了防逃逸。
2. **不追着补删除**：上游把新内建并进 `JS_AddIntrinsicBaseObjects` 时（如 0.18.0 把 `BigInt`
   并进来），**不**为它加新的删除 —— 只记录当前表面（`tests/ui_script_quickjs.cpp` 的 sandbox
   用例同理：那张表记录**表面**，不是合同）。
3. **宿主边界不在这里**：脚本能做什么只由注入的 `ui` / `game` / `host` 决定（无文件 / 网络 / 进程 / native 指针）；
   用 `JS_NewContextRaw` 少装 intrinsic 另有功能价值（体积小、没有 `Date` 等不确定面；`Promise` 是
   显式例外，见下），但它**不是**权限边界。

`JS_NewContext` 与 `JS_NewContextRaw` 的差集 = Date / eval / RegExp / JSON / Proxy / MapSet / TypedArrays /
Promise / WeakRef / `performance` 的来源（`quickjs.c:2894-2918` vs `2848-2892`）——本仓从那组里只加了
`Eval`（宿主编译）与 `Promise`（module 求值；裁决 2026-11-19），其余不加。
★ 0.18.0 起 `BigInt` **不在**这个差集里：`JS_AddIntrinsicBaseObjects` 自己就会调
`JS_AddIntrinsicBigInt`（`quickjs.c:59170`），所以"只加 BaseObjects"的形状**会带进 `BigInt`**：
实测 `typeof BigInt === "function"`，且 `1n` 字面量照常可用（`typeof 1n === "bigint"`；
全局构造器在 ⇒ `1n` 在；删了全局构造器 `<1n>` 也仍在，所以⛔ 不拿"删 BigInt"当收紧手段）。
我们的形状：

1. `JS_NewContextRaw`（只建原型与空全局对象，不装任何构造器）。
2. 加 `JS_AddIntrinsicBaseObjects`（Object / Function / Array / String / Number / Boolean / Error 族 /
   Math / Reflect / Iterator / Symbol / **BigInt** / `parseInt` / `isNaN`）——本切片**不**加 `MapSet` / `JSON`。
   ★ 另外**必加** `JS_AddIntrinsicEval`（宿主编译，见 4）与 `JS_AddIntrinsicPromise`（module 求值返回
   Promise，见 §3 / §5；用户裁决 2026-11-19：**加而不删** —— 沙箱不是安全边界，先加再删只是
   以后要用还得改回来的额外特殊处理）。
3. ⛔ 不加 `Date`（墙钟）、`RegExp`、`Proxy`、`TypedArrays`、`WeakRef`、
   `JS_AddPerformance`（`Date.now` 派生）；本切片连 `JSON` / `MapSet` 也不加（HUD 用不到）。
   ★ `Promise` **不在**"不加"之列（见 2）；`typeof Promise === "function"` 是新的**表面事实**
   （`tests/ui_script_quickjs.cpp` 记它，⛔ 不当合同），它**不是**给脚本的跨帧异步（`render` 仍同步，见 §3）。
   （`BigInt` 是 `BaseObjects` 自带的，见上；⛔ 不为它补删除。）
4. ★ **必须加 `JS_AddIntrinsicEval`**，但只为**宿主**能编译脚本：宿主 `JS_Eval` 走
   `JS_EvalThis2` → `JS_EvalInternal`，而它在 `quickjs.c:38338` 先判 `ctx->eval_internal`
   （只由 `JS_AddIntrinsicEval` 设置）⇒ 不加它宿主自己就 `TypeError: eval is not supported`
   （2026-11-18 实测）。**沙箱的门因此不是"不加 Eval"，而是加完之后删干净**：
   `globalThis.eval`、`globalThis.Function`、**`Function.prototype.constructor`**、
   `Math.random`（种子 = `js__gettimeofday_us()`，逐 **context** 不同 ⇒ 不可复现）。
   ★ 只删 `globalThis.Function` **不够**：实测 `(function(){}).constructor('return 1')()`
   在只删全局 `Function` 后仍可用；删掉 `Function.prototype.constructor` 后才抛 `TypeError`。
   `eval` 全局删掉后是 `ReferenceError: eval is not defined`。
   ⇒ 这一段推翻本节早先的"不加 Eval"写法（同一条实测，2026-11-18）。
5. 无 `std` / `os`：那是 `quickjs-libc.c` 的模块（本仓不 vendor 它）。module loader **已接**
   （`script_quickjs.cpp:moduleLoad` / `moduleNormalize`），但它只读 **UI 根目录子树内的相对路径**；
   bare specifier（含 `std` / `os`）/ 绝对路径 / `data:` 一律在 resolve 期拒绝 ⇒ 那些 `import` 仍失败。
6. 无 host 反射：不学 `NativeObject<T>` / `NativePointerHolder`，⛔ 不给脚本任何 native 指针 / 句柄。

### 2.2 预算：三条线、三个失败面

| 资源 | 设置 | 失败面 |
|---|---|---|
| 内存 | `JS_SetMemoryLimit(rt, N)`（0 = 关）；超限 ⇒ 分配返 NULL ⇒ 抛错 | **插件致命**（OOM 后 ctx 不可信）⇒ 卸载，旧实例继续 |
| 栈 | `JS_SetMaxStackSize(rt, 512 KiB)`（默认 1 MiB；Windows exe 默认栈 reserve 也是 1 MiB ⇒ 不调小会先打穿宿主栈再报溢出；M2a 实值 512 KiB，见 `script_quickjs.cpp`） | 同上 |
| CPU | `JS_SetInterruptHandler(rt, cb, opaque)`：回调返非 0 ⇒ 上游走 `JS_ThrowInternalError("interrupted")` + `JS_SetUncatchableError` ⇒ 脚本 `try/catch` 吞不掉（★ 0.18.0 的 `JS_ThrowInterrupted` 是 `quickjs.c:8653` 的 static，头里没有，⛔ 不要直接调）。★ 0.18.0 起回调签名的**第一个参数是 `JSContext*`**（不再是 `JSRuntime*`；`quickjs.h:1192`，PR #1802） | 本帧不上屏 + 一次响亮；连续 3 次 ⇒ 卸载 |

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
| `load` | `JS_NewRuntime` → 三个预算设置 → `JS_NewContextRaw` → intrinsic（BaseObjects + Eval + Promise）→ 注入 `ui` / `game` / `host` → **classic 优先探针**（`COMPILE_ONLY`）：能编走 `JS_Eval(..., GLOBAL\|STRICT)`，否则 module 走 loader + `JS_Eval(MODULE)`（返 Promise ⇒ `drainPendingJobs` + 查 rejection）→ 存 `render` 句柄（`JS_DupValue`） |
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
- `Promise` **存在**（`JS_AddIntrinsicPromise`、不删；用户裁决 2026-11-19），宿主的 job 泵**只**服务
  module 求值 / 关闭的 microtask；⛔ `render` 仍必须**同步**出 display list —— 没有跨帧 async / 协程
  （`overlay.md` D10）。
- `game` 定死为 **getter 字段**：带下标的只能是函数（`local_int(3)` 等）；⛔ 字段与函数两种形态不并存。

## 4. JS 作者体验（M2a 实况；S2 起 hud 是插件）

- 默认 HUD = **外置** `apps/emulator-lift/scripts/ui/hud.js`（UTF-8、**module**、strict；它
  `import { Button } from './kit.js'`。★ **S2 起入口是插件契约** `export default { render(ctx) }`
  （`ctx.draw.*` / `ctx.game.*` / `ctx.profile.*`）；**同一份文件还保留** `globalThis.render` ——
  旧 `--ui` loader（deprecated）仍要它，两个入口共用同一份绘制体、只换薄适配器。
  classic 脚本仍支持（`--ui` 旧语义：能按 classic 编译就绝不走 module）。
- 随发行分发 `scripts/ui/{hud.js, kit.js, ui.d.ts, README.md}`；**`hud.js` 与 `kit.js` 必须同目录**
  （`import './kit.js'` 按 `hud.js` 所在目录解析）——缺 kit 与缺 hud 同属加载期硬失败
  （`--overlay` / `--plugin hud` exit 2，错误里点名路径）。`hud` 是**保留 id**（S2），解析顺序：
  ① `<exe 目录>/scripts/ui/hud.js`（发行布局）→ ② `LIFT_UI_SCRIPT_DIR/hud.js`（开发回退，
  CMake 把源码目录以 `target_compile_definitions(... PUBLIC ...)` 注入）→ ③ 硬错（err 点名两个路径）；
  两份都在场 ⇒ **exe 赢**；用户插件根里的 `hud.js` ⇒ exit 2（⛔ 不静默被内置件遮住）。
- `--ui <path>` 只吃 `.js`；显式优先且隐含 `--overlay`。启动期校验"文件在 + 编译过 + `render` 是函数"，
  失败 = `exit 2` + `<path>:<行>:<列>`。★ **S1 起 `--ui` 已弃用**（一行 deprecation，行为不变；
  新入口 `--plugin <id|path>`）；★ **S2 起 `--overlay` = `--plugin hud` 预设**（窗口档走 adopt，
  `SdlWindow::init` 不再为 `--overlay` 装载 hud.js）。
- `scripts/ui/ui.d.ts` 是作者声明（`declare const ui` / `game` / `host`，含 `game.local_frames()` 与
  module 的 `import` 说明）；
  **不参与构建、不进 `pnpm typecheck`**。同目录的 `tsconfig.json` 是**编辑器 / 手动检查**用的
  （`pnpm exec tsc -p apps/emulator-lift/scripts/ui`，见 `scripts/ui/README.md`），不是仓库门禁；
  ⛔ 没有 `tsc` 产物 / 漂移守卫（`tools/test/ui-dts.test.mjs` 未做）。
- 模板：`scripts/ui/hud.js` 本身就是模板（版式常量 PAD/SIZE/BG/FG 在文件头）。

## 5. 静态链接与发行

- QuickJS **vendor** 进 `apps/emulator-lift/third_party/quickjs-ng`（tag **v0.18.0** / commit
  `d7215b0a…`；MIT；**4 个 C 源** + 头 + `LICENSE` + 本仓 `README.md` + 本仓 `CMakeLists.txt` 建 `lift-qjs`）。
  ⛔ 不是 git submodule（本仓 `third_party/` 的惯例 = 原样副本 + README 记录来源 / 清单 / 怎么升级）。
- `lift-qjs` **静态链进 exe**（`emulator-lift-runtime` PUBLIC 链接）⇒ 没开 `--ui` 也在 exe 里。
  ★ **默认 HUD 的 JS 是外置文件**（不编进 exe）：发行时 `scripts/ui/` 与 exe 一起放。
- **版本 pin**：`src/ui/quickjs_value.h` 对 `QJS_VERSION_MAJOR/MINOR/PATCH == 0/18/0` 做 `static_assert`；
  升级要同时改 `third_party/quickjs-ng/README.md`。
- **LTO**：`lift-qjs` 是独立 target，未加 `-flto`（与 `LIFT_AOT_LTO` 的 runtime / AOT 目标分开；
  "vendored C 一起 LTO"是原始设想，本切片**未验证 / 未启用**）。
- **体积 / 首帧开销未实测**；⛔ 不把估计写进判据。

## 待核

1. clang-cl 编译（**2026-11-18 已核**，clang 22.1.2 / RelWithDebInfo）：上游对 MSVC 加
   `/experimental:c11atomics`（`CMakeLists.txt:129`）、对"认这个 flag 的编译器"加 `-funsigned-char`
   （`:108-110`）。本仓用 `if(NOT MSVC)` ⇒ clang-cl 走 MSVC 分支、**不加**；这不叫"少加了"：
   `-funsigned-char` 在 clang-cl 上是 **no-op**（`clang-cl: warning: unknown argument ignored ...
   '-funsigned-char'`）⇒ 上游 win32（MSVC / clang-cl）最终也是 signed char，与本仓等价。
   C11 atomics 不受影响：`quickjs-c-atomics.h` 对 clang 走 `#include <stdatomic.h>`（`GCC_BUILTIN_ATOMICS`
   只给 GCC < 4.9），clang-cl 22.1.2 直接编过（`lift-qjs` 4 个 TU 全绿），**不需要** `/experimental:c11atomics`。
2. 告警：0.18.0 的 `-Werror` 只在上游 `QJS_BUILD_WERROR=ON` 时加（`CMakeLists.txt:93-96`，`:95` 是
   `-Werror`；缺省 OFF），
   且本仓**不 vendor 上游 CMakeLists.txt** ⇒ 这条对 lift 不适用（`lift-qjs` 不加任何告警 flag）。
3. `JSValue` 泄漏：唯一正确形状 = 每个 `JSValue` 有唯一所有者；开发构建开 `JS_DUMP_LEAKS`，产品构建关。
4. GC 与宿主生命周期：宿主只在 runtime 内持句柄，跨调用 `JS_DupValue`；热重载换 runtime 后旧句柄必须失效。
5. 体积 / 首帧开销：仍是估计；实测落 `observations.md`，阈值由读数定。
6. 白名单 intrinsic 是否够：`Array.prototype.map` / `Object.entries` 在 `BaseObjects` 里（已核）；
   `JSON` 是否要加，等 HUD 样例定。

## 复现命令

```powershell
# 本仓 vendor 的那份（v0.18.0；文件清单 / 来源 / 怎么升级见 third_party/quickjs-ng/README.md）
$qjs = 'E:\Projects\amayui-re\apps\emulator-lift\third_party\quickjs-ng'
rg -n 'JS_SetMemoryLimit|JS_SetMaxStackSize|JS_SetInterruptHandler' $qjs/quickjs.h
rg -n 'JS_AddIntrinsic' $qjs/quickjs.h
rg -n 'JS_AddIntrinsicBigInt|JSContext \*JS_NewContextRaw|JS_INTERRUPT_COUNTER_INIT|js_random_init' $qjs/quickjs.c

# MaaFramework 只是**只读参考**（本仓不依赖它的 pin；它的 quickjs submodule 可能仍是旧 tag）
$maa = 'E:\Projects\MAA\MaaFramework'
git -C $maa submodule status 3rdparty/quickjs
rg -n 'JS_SetMemoryLimit|JS_SetMaxStackSize|JS_SetInterruptHandler' $maa/source/binding/NodeJS/src   # 零命中
```
