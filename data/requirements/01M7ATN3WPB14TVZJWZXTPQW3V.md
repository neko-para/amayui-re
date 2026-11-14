# 🐞S2 shim advapi32：RegOpenKeyExA(phkResult=NULL) / RegQueryValueExA(lpcbData=NULL) 静默返回 SUCCESS（文档要求 87）

- id: REQ-01M7ATN3WPB14TVZJWZXTPQW3V
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/advapi32_registry.cpp#reg-open-null-phkresult
- repro: apps/emulator-lift/src/shims/advapi32_registry.cpp#void shimRegOpenKeyExA(X86& cpu, ShimCall& call)
- severity: S2
- done_reason: 两条参数校验落地：RegOpenKeyExA(phkResult=NULL) ⇒ 87 + last_error=87 + warn，且不写任何内存；RegQueryValueExA(lpData!=NULL 且 lpcbData==NULL) ⇒ 87，而 lpData==NULL 时 lpcbData==NULL 仍合法（走既有取值路径）。依据从'文档要求'改成**本机实测**（Win11 x64 / P/Invoke 探针 / 2026-11-14：RegOpenKeyExA(NULL)=87、RegQueryValueExA(lpData!=NULL,lpcbData=NULL)=87、(NULL,NULL)=2），文档只作约束（GetRenderState 式的误引不再出现）。守卫新 CTest advapi32-registry（顶层 pure，13 checks；修前 4 failures：expected 0x57 got 0x0），变异把两段校验换回写 NULL ⇒ 该用例红（父级复跑 1/1 红）。已知残留：非法路径连 lpType 也不写是我们的裁决（实测只给了返回码），要更贴近真机得补带 lpType 哨兵的探针。
- tags: [emulator-lift, shim, contract]

## 现象（2026-11-13 分析时读代码发现，父级已复核）

`advapi32!RegOpenKeyExA` 把"注册表整体 no-op"实现成**无条件成功**：

```cpp
const std::uint32_t out = call.u32(4);          // phkResult
if (out) cpu.mem->wr32(out, kFakeKey);
call.result = 0;                                 // ERROR_SUCCESS —— 不看 out
```

⇒ `phkResult == NULL` 时仍然返回 `ERROR_SUCCESS`。**微软文档**：`RegOpenKeyEx` 的 `phkResult` 不能为
`NULL`，此时返回 `ERROR_INVALID_PARAMETER`（87）。

同类（同一文件）：`RegQueryValueExA` **不校验 `lpcbData`**（`NULL` 时按文档应返回
`ERROR_INVALID_PARAMETER`），也是无条件成功。

## 为什么这是缺陷而不是"取舍"

SCOPE §1 允许注册表整体 no-op（我们不读真实注册表）——那是**取值层面**的取舍；
但"**参数非法**时返回什么"是 API 合同（微软文档写死），不是取值取舍。引擎侧行为不可见，
而**换个游戏/工具**（或引擎的某个分支）一旦依赖 87 就会走错路径，且**没有日志能发现**。

## 判据（修好后）

- `RegOpenKeyExA(..., phkResult=NULL)` ⇒ 返回 `ERROR_INVALID_PARAMETER`(87)，且**不写任何内存**；
- `RegQueryValueExA(..., lpcbData=NULL)` ⇒ 返回 87（若引擎实测确实传 NULL，则这条要先取证再改）；
- 有守卫：CTest 里加在现有 `advapi32` 用例（或 `shims-core`）上，红 = 返回值变了；
- ⛔ 不改"读不到真实注册表"这一层取舍。

## 证伪条件

若引擎（或任何 guest 侧观察）**依赖**"`phkResult=NULL` 也成功"这一行为（给出 EA 或调用点），
则本条改成记录该取舍 + 关掉判据。

## 顺带登记（不在本条范围）

`src/shims/shims_core.cpp` 的 `unsignaled-wait-traps` 整块在 `#if !defined(_WIN32)` 里 ⇒
**Windows（主力平台）零守卫**。这是"守卫只在非主力平台跑"的一类，单独开单。
