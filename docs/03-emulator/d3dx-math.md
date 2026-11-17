# D3DX math · `D3DXMatrixDecompose`（口径 / native 对拍 / 引擎调用点）

> 本文只写**这一条** D3DX math 导出的口径与实测读数。D3DX 的其余 math 导出
> （Multiply / Scaling / Translation / RotationAxis / YawPitchRoll / OrthoLH /
> PerspectiveFovLH / LookAtLH / Vec4Transform / Vec3Normalize）的判据在
> `apps/emulator-lift/tests/d3dx9.cpp` 的 `d3dx9/matrix` 里，实现与其公式在同文件的 shim 段。
>
> 口径：**语义真源 = native `d3dx9_43.dll`（9.29.952.3111 / x64）+ DirectXMath 的
> `XMMatrixDecompose`**。本文所有"native 给什么"都来自 P/Invoke 对拍（命令见 §6），
> ⛔ 不来自 D3DX 文档（文档只写 S_OK / D3DERR_INVALIDCALL，**没写失败条件**）。

## 0. 落点与依赖

| 是什么 | 在哪 |
|---|---|
| 数学核（唯一实现） | `apps/emulator-lift/src/runtime/math.cpp` 的 `math::decomposeD3D`（声明在 `runtime/math.h`） |
| shim（指针校验 / guest 读写 / HRESULT） | `apps/emulator-lift/src/shims/d3dx9.cpp` 的 `shimD3DXMatrixDecompose` |
| 注册 | 同文件 `registerD3DX9Shims`：`shims.add("d3dx9_43", "D3DXMatrixDecompose", Conv::Stdcall, 4, …)` |
| 依赖 | `apps/emulator-lift/vcpkg.json` 的 `directxmath`（port `version-date 2026-06-12` / REF `jun2026` / **MIT**）；CMake 侧 `find_package(directxmath CONFIG REQUIRED)` + `target_link_libraries(emulator-lift-runtime **PRIVATE** Microsoft::DirectXMath)` |
| 守卫 | CTest `d3dx9` 的 `caseMatrixDecompose()`（`apps/emulator-lift/tests/d3dx9.cpp`） |
| 变异 | `tools/mutate-check.mjs` 的"四元数写共轭"条目（`--match '共轭'`） |

★ **`XMVECTOR` / `<DirectXMath.h>` 只出现在 `runtime/math.cpp` 这一个 TU**：`runtime/math.h` 继续只暴露
glm 类型（`Vec3` / `Quat`），⛔ 不把 SIMD 类型漏进公共头；链接一律 PRIVATE。

## 1. 语义（照 native，逐条有对拍）

输入按 **D3DX 行主序 16 个 float**（= guest 里的 `D3DXMATRIX`）原样喂 `XMLoadFloat4x4`
—— DirectXMath 的 `r[i]` 就是 D3DX 的第 i 行，**不是**"跨约定 memcpy"。判定规则：

| # | 规则 | native 读数（P/Invoke） | DirectXMath 一致？ |
|---|---|---|---|
| 1 | `translation` = flat **12/13/14** 逐位照读；`m[3][3]`（0 / 2 / 5）**忽略**（不归一化） | I+T(10,20,30) ⇒ T 逐位；m33=2 / 5 ⇒ T 仍逐位 | ✅ |
| 2 | `scale_i` = 第 i 行 **xyz 的长度**（`m[i][3]` 不计） | diag(2,3,4) ⇒ (2,3,4) | ✅ ≤1 ulp |
| 3 | **零 scale 是 S_OK**（不是失败）：diag(2,0,4) ⇒ (2,0,4)/(0,0,0,1) | 全零矩阵也 S_OK，quat=(1,0,0,0)（**非单位**） | ✅ 逐位 |
| 4 | 极小 scale 的阈值 = `XM3_DECOMP_EPSILON = 1e-4f`，判据是**严格 `<`** | 判别实验：1e-5 ⇒ 走 canonical-basis 回退（quat 错位到 180°、scale 出现 -1）；1e-4 ⇒ 正常路径（quat=Rz45） | ✅ |
| 5 | 切变（非 SRT）：判据 `(det(R̂) − 1)² > 1e-4`（R̂ = 每一行归一化后的 3×3） | 0.100…0.200 扫描（步 0.001）：**0.142 过、0.143 失败**；理论临界 0.14250 | ✅ 同阈值同判据 |
| 6 | 失败时 **scale + translation 已写、quat 一个字节都不动** | sentinel `0xA5A5A5A5` 在失败路径仍原样 | ✅（实现里显式照做） |
| 7 | 负 det 的**符号落点**：`|scale|` 最大的那根轴被取反（并列取下标最小），旋转部分保持 `det(R)>0` | diag(-1,1,1)⇒(-1,1,1)/单位；diag(1,1,-1)⇒(-1,1,1)/180°y；diag(-2,3,4)⇒(2,3,-4)/180°y | ✅（134 例全对） |
| 8 | 透视列（flat 3/7/11）**忽略** | (0.1,0.2,0.3)/(10,20,30)/(-1,0.5,-2) 三组 ⇒ S_OK、其余字段正确 | ✅ |
| 9 | 返回值只有两种：`S_OK=0` / `D3DERR_INVALIDCALL=0x8876086C` | — | ✅ |

★ 第 7 条的"落点规则"是 DirectXMath 的 `pfScales[a]` 约定，**不是 D3DX 文档承诺** ⇒ 用例只锁
`|scale|` + `det(R(q))>0` + 合成，⛔ 不锁符号落哪根轴（`d3dx9/matrix-decompose-mirror`）。

## 2. 134 例对拍（这一节是"为什么敢用 DirectXMath"）

方法：134 个矩阵写成 **bit-exact** 输入（`mats.bin`），native 走真 `d3dx9_43.dll`（P/Invoke，
出参预填 `0xA5` sentinel），候选各自跑；两边输出**按 float 位型落文本**再比
（复现命令在 `.tmp/matrix-decompose/perf.md` §复现，本轮评估用；该目录不入库）。

| 候选 | 判定（接受/拒绝） | scale | translation | quat | 判定 |
|---|---|---|---|---|---|
| **DirectXMath `XMMatrixDecompose`** | **134/134 一致** | ≤1 ulp | 逐位 | 正交输入逐位 / ≤1e-7；**被接受的切变**输入差 ≤6.2e-4 | ✅ 采用 |
| 手写（float） | 134/134 | dS 逐位 0 | 逐位 | dQ ≤7.5e-9 | 退路（42.4 ns/call） |
| 手写（double） | 134/134 | dS ≤2.4e-7 | 逐位 | dQ ≤3e-8 | 退路（44.4 ns/call） |
| glm 1.0.3 `glm::decompose` | **65/134 不同** | dS 6.0 | dT 24 | dQ 1.31 | ❌ 不可用 |
| Eigen 3 | 无 D3DX 式仿射分解 API（要自己接 QR/SVD，且无切变阈值 / 负 det 约定） | — | — | — | ❌ |

- **唯一可观察差异**（第 5 行）：矩阵被接受但**不正交**（切变 ≤0.142）时，native 返回**未归一化**的
  四元数（s=0.142：native `(0,0,0.0351911,0.998758)`、`|q|=0.99938`），DirectXMath 归一化。
  **方向相同**；引擎 5 个调用点都不读四元数（§3）⇒ 不阻塞。⛔ 所以用例**不逐位断言 quat**。
- **为什么不是 glm**（保留在此，别再有第二次评估）：glm 先 `/M[3][3]`（native 忽略 m33）、
  3×3 行列式≈0 就 `false`（native 零 scale 是 S_OK）、会解 perspective（native 丢掉）、
  det<0 时**三轴全反**（native 只反一根）、切变靠 Gram-Schmidt **永不失败**（native >0.1425 返
  INVALIDCALL）⇒ 134 例里 65 例接受/拒绝与 native 不同。
- 性能**不是**决策点：33 ns/call（min）⇒ 5 站点 × 1000 节点/帧 ≈ 0.17 ms（60 fps 预算的 ~1%）。

## 3. 引擎的 5 个调用点（EA 锚，⛔ 不锚语料行号）

`rg 'D3DXMatrixDecompose' corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst` 只命中
IAT（`.data:0051D4D0`）、导入名（`.idata:005C14A4`）与下面 5 个 call：

| # | call EA | 所在函数 | 返回值 / 错误路径 | 出参落点（stack） |
|---|---|---|---|---|
| 1 | `0x49BA4A` | `sub_49AA30` | **忽略**（call 后紧接着 `fldz`，不是 `test eax,eax`） | pOutScale=`[ebp-5C]` · pOutRotation=`[ebp-224]` · pOutTranslation=`[ebp-50]` · pM=`[ebp-A8]` |
| 2 | `0x49C817` | `sub_49BCC0` | **忽略**（同形：call 后是 `fldz`） | pOutScale=`[ebp-9C]` · pOutRotation=`[ebp-154]` · pOutTranslation=`[ebp-50]` · pM=`[ebp-90]` |
| 3 | `0x4A9F50` | `sub_4A9EC0`（`GetLocalMatrixS`，错误串点名） | **检查**：`test eax,eax / jz` 跳过；非 0 ⇒ 打「関数：GetLocalMatrixS エラー：マトリックスの分割に失敗しました」后返回 | pOutScale=`[ebp-10]` · pOutRotation=`[ebp-320]` · pOutTranslation=`[ebp-1C]` · pM=`[ebp-294]` |
| 4 | `0x4AA0F0` | `sub_4AA060`（`GetLocalMatrixT`，错误串点名） | **检查**（同形；错误串 = `GetLocalMatrixT`） | pOutScale=`[ebp-1C]` · pOutRotation=`[ebp-320]` · pOutTranslation=`[ebp-10]` · pM=`[ebp-194]` |
| 5 | `0x4AF05A` | `sub_4AEEA0`（`RenderScene`，错误串点名） | **检查**（同形；错误串 = `RenderScene … マトリックスの分割に失敗しました`） | pOutScale=`[ebp-1C]` · pOutRotation=`[ebp-3EC]` · pOutTranslation=`[ebp-10]` · pM=`[ebx+0xB608]` |

- 站点 3/4 的 pM 来自一条 **740 B（`0xB9` dwords）节点记录 +0x6C**：`0x4A9F2A lea esi,[eax+10h]` /
  `0x4A9F34 mov ecx,0B9h` / `rep movsd` 把记录抄到栈上，矩阵在 `[ebp-300]+0x6C`。
- 站点 5 的 pM 是 `[ebx+0xB608]`（渲染器自己的矩阵）。
- **引擎真正消费的字段**：`scale`（站点 3 拷 x/y 去做尺寸；站点 5 用 `scale.x/y`）与
  `translation`（站点 4）。四元数只落在栈上 scratch —— **5 处都不读**（`pOutRotation` 只在
  上表的栈槽被 push，之后没有读点）⇒ §2 的 quat 归一化差异对本作不可见。
- 矩阵来源：引擎自己用 `D3DXMatrixScaling` × `RotationAxis` × `Translation` 乘出来的
  ⇒ **仿射、无透视**；切变不太可能；零 / 负 scale 取决于游戏数据（**未证**，见 §7）。

## 4. 实现要点（改代码前先读这三条）

1. **float 路径**：输入直接收 `const float[16]`（`memcpy` 到 `XMFLOAT4X4`，两者 flat 布局逐位相同），
   ⛔ 不先转 `double` —— native 是 float 路径，double 版会丢 1–2 ulp（对拍：float 版 dS 逐位 0）。
2. **`outScale` / `outTrans` 是 `XMVECTOR*`、会写满 16 B**（scale 写 4 个 float，第 4 个 0；
   translation 写整个 `M.r[3]`）⇒ 必须用 **16 B 对齐的 scratch** 接，⛔ 不能把 guest 的
   12 B `D3DXVECTOR3` 指针直接传进去（踩栈）。
3. **失败路径不写 quat**：scratch 接出来，按 `ok` 决定写不写 guest 的四元数缓冲。
   shim 里失败分支**不碰**那 16 B（⛔ 不清零、⛔ 不写默认四元数）—— 用例拿 `0xA5A5A5A5` sentinel 判它。

shim 的 HRESULT：成功 `d3d9::kOk`；切变 / 非 SRT ⇒ `d3d9::kInvalidCall`（`0x8876086C`）。
参数未提交 ⇒ `d3dxTrap`（与同文件其它 math shim 同一口径；`unimplemented_calls` +1、机器停）。

## 5. 守卫（8 个子用例，CTest 仍是 `d3dx9`）

`apps/emulator-lift/tests/d3dx9.cpp` 的 `caseMatrixDecompose()`，`g_current_case`：

| 子用例 | 锁什么（能红的点） |
|---|---|
| `d3dx9/matrix-decompose-translation` | scale 逐位 (1,1,1)、q 单位、translation 逐位；**m33 / 透视列被忽略** |
| `d3dx9/matrix-decompose-rotation` | RotZ(+90°) ⇒ `q.z = **+**0.7071068`（**共轭 bug 当场红**）+ 合成 |
| `d3dx9/matrix-decompose-scale` | diag(2,3,4)+T：scale / translation 逐位、q 单位 + 合成 |
| `d3dx9/matrix-decompose-trs` | Rz30°·S(2,3,4)·T：scale 1e-6、`q.z = sin15°`、`|q|≈1`、合成（非对角项 ⇒ 行长 vs 列长在这里分家） |
| `d3dx9/matrix-decompose-zero-scale` | diag(2,0,4) **HR=S_OK**（⛔ "奇异即失败"当场红）、scale 逐位、q 有限且 `|q|≈1` |
| `d3dx9/matrix-decompose-shear` | 切变 0.5 ⇒ **HR=0x8876086C**、机器**没停**、`unimplemented_calls` 不涨、scale+translation 已写、**quat sentinel 原样** |
| `d3dx9/matrix-decompose-mirror` | diag(-1,1,1)：`|scale|`=(1,1,1)、`det(R(q))>0`、合成（⛔ 不锁符号落哪根轴） |
| `d3dx9/matrix-decompose-bad-pointer` | `pOutScale=NULL` ⇒ loud trap（`StopReason::Trap`、`stop_why`、`unimplemented_calls`+1） |

容差口径：translation **逐位**；scale 轴对齐输入逐位、一般 1e-6；quat 按**方向/合成**（不逐位）；
合成 `1e-5 * max(1,|m_ij|)`（测试侧独立实现 `M_ij = scale_i · R_col(q)[j][i]`，⛔ 不借 `math::`）。

## 6. 读数与复现命令

读数口径：**win32 / RelWithDebInfo（`windows-x64` 预设）/ clang-cl 22.1.2 / 2026-11-18**。

| 命令 | 读数 |
|---|---|
| `cmake --preset windows-x64` | vcpkg 装 `directxmath:x64-windows@2026-06-12`（11 s；下载 `Microsoft-DirectXMath-jun2026.tar.gz`）；configure 17.4 s |
| `cmake --build --preset windows-x64 --parallel 4` | 成功（136 步，含 AOT 8 分片 LTO） |
| `ctest --test-dir out -R '^d3dx9$' --output-on-failure` | **1/1 Passed**（0.19 s） |
| `./out/tests/lift-d3dx9.exe` | `d3dx9: **796 checks, 0 failures**`（整个 `d3dx9` 夹具的断言总数，含本单 8 条子用例） |
| `ctest --test-dir out --output-on-failure` | **107/107 passed**（130.49 s） |
| `node tools/mutate-check.mjs --match '共轭'` | **红**（变异体让 `d3dx9/matrix-decompose-rotation` 的 q.z 期望 +0.7071 得 -0.7071；详见 §5 与 `.tmp/matrix-decompose/impl-report.md`） |
| `pnpm tools lift sync --write` | 缺口 5→4 条；`data/lift/overrides.json` 59 条（本条的 `trap` 条目消失） |
| `pnpm tools lift check` | 绿（0 个问题；导入 310 = 实现 257 · 显式 gap 53 · 裸缺口 0） |

```bash
# 语义 / 调用点（只读语料）
rg -n 'D3DXMatrixDecompose' corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst
pnpm tools disasm-at at --ea 0x4a9f50 --lines 24   # GetLocalMatrixS：HRESULT 检查 + 错误串 + 拷 scale
pnpm tools disasm-at at --ea 0x4aa0f0 --lines 24   # GetLocalMatrixT：拷 translation
pnpm tools disasm-at at --ea 0x4af05a --lines 24   # RenderScene：scale.x/y + translation

# 构建 / 守卫
cd apps/emulator-lift
cmake --preset windows-x64
cmake --build --preset windows-x64 --parallel 4
ctest --test-dir out -R '^d3dx9$' --output-on-failure
ctest --test-dir out --output-on-failure

# 派生真相（⛔ 不要手改 register_import_gaps.cpp / data/lift/overrides.json）
cd ../.. && pnpm tools lift sync --write && pnpm tools lift check

# 变异（只跑这一条；全量是刻意扫，代价见 AGENTS.md §5.3）
node tools/mutate-check.mjs --match '共轭'
```

## 7. 未做 / 待核

- **32 位 DLL 未单独对拍**：上面的 native 读数全部走 x64 `C:\Windows\System32\d3dx9_43.dll`
  （同版本 9.29.952.3111；SysWOW64 那份**没跑**）。AGE.EXE 是 32 位 ⇒ 严格说"native 语义"仍是同一份
  实现的 64 位路径。风险：32 位 SSE 路径的 ulp 差异（本轮**无**读数）。
- **切变阈值是实测取舍**：0.142/0.143 来自单版本单机扫描；`(det−1)² > 1e-4` 是从 DirectXMath 源码
  读出的同一条判据，但⛔ 不要把它写成 D3DX 的契约（D3DX 文档没说失败条件）。
- **quat 在被接受的切变输入上差 ≤6.2e-4**（native 不归一化 / DirectXMath 归一化）：引擎 5 个站点
  都不读 quat ⇒ 本轮不修；若将来有站点读 quat，需要手写不归一化的版本（float 退路 dQ ≤7.5e-9）。
- **mac / Linux 未实测**：port 声明 `supports: !arm32`、非 Windows 会装一份 `sal.h`、头里有
  ARM64 NEON 分支；本轮只在 win32 x64（SSE2 路径）跑过。首次在 mac 构建时**必须重跑 §2 的语义表**。
- **负 det 的符号落点**只锁"合成回去"与 `det(R)>0`，⛔ 不锁哪根轴被取反（§1 第 7 条）。
  若游戏真传镜像矩阵而引擎拿 `scale.x/y` 做尺寸，符号不同会翻 billboard ⇒ 真跑时看一眼。
- **引擎是否真会传零 / 负 scale 未证**：节点记录来自游戏数据 / 动画（站点 3/4 的 740 B 记录）。
  E2E 撞到这条只能说明"有调用"，不说明矩阵形态。
- **E2E 未到**（2026-11-18 实测，4 次尝试）：读档链（`.tmp/appdata` 的 SAVE03 = 列表第 4 槽；
  appdata 副本要删 `SAVE.DAT/SAVE.BAK/RT.DAT`，否则第一页是空的 110..119）走到了 **SC0000**
  （`steps=12 fired=4 pending=0`，`--time-limit 300` 结束在 `eip=004127D2` / `frames=3066`），
  但 SC0000 是 2D 脚本动画，`sub_4AEEA0`（RenderScene）与 `GetLocalMatrix*` 都没被执行；
  FELLOW 路径（`steps=7 fired=3 pending=0`，`frames=11782`）同样没进那段渲染器。
  ⇒ 本条**没有**"实现后不再停在 unimplemented"的端到端读数，只有 §5 的单元判据与 §6 的 CTest 读数。
- ★ **`--until` 只认 dispatch 表的块入口**（`out/aot/lift_table.inc`）：`0x4A9F50` / `0x4AF05A`
  **不是**入口 ⇒ 用它们会永远 time-limit（看起来像"没到"，其实可能早就通过了）。要用**紧随块入口**：
  `0x49BA4F`（站点 1）· `0x49C81C`（站点 2）· `0x4A9F55`（站点 3）· `0x4AA0F5`（站点 4）·
  `0x4AF05F`（站点 5）。它们都只有 call 的 fallthrough 能到（语料里没有跳转指向）⇒
  停在它 = 那次 `D3DXMatrixDecompose` 已经执行并返回。
