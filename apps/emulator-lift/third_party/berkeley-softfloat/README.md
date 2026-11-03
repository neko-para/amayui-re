# berkeley-softfloat（x87 的 80 位算术内核 · vendor）

`source/` 是 **Berkeley SoftFloat Release 3e** 的原样副本（未改一行 C 源码），只保留 **8086 特化**。
它是 `apps/emulator-lift` 里 x87 的**算术内核**（decision D3，见
[`../../docs/plan-m1-cpu-base.md`](../../docs/plan-m1-cpu-base.md) §4 D3）。

## 来源与身份

- 上游：<https://github.com/ucb-bar/berkeley-softfloat-3>（Release **3e**，John R. Hauser）
- 本仓取的提交：`07ebd29db0cfb55723a79fbbffe99103ef38e305`（= 上游 `master`，即 3e 的发布点；
  上游**没有打 tag**，所以这里用 commit 而不用 tag 名，`git describe` 那套在这里不成立）
- 上游 tarball：`https://github.com/ucb-bar/berkeley-softfloat-3/archive/07ebd29db0cfb55723a79fbbffe99103ef38e305.tar.gz`
  - tarball sha256 = `4B003E4983B53F11919C9375A589199F74B86339E4F746165812F3D9C18BFD78`
- **本仓字节**（目录形态，所以给的是清单哈希）：375 个文件 / 1,455,980 字节；
  清单 = 每行 `<sha256>  <相对路径>`（按路径排序，不含本 `README.md` 与 `CMakeLists.txt`），
  该清单的 sha256 = `0db213fb3472ee6adb4298f631e7c36860c36822d22e1c34bd75904d4fc77703`
  - 复核命令（在仓库根跑）：
    ```bash
    cd apps/emulator-lift/third_party/berkeley-softfloat && \
    { find . -type f ! -name README.md ! -name CMakeLists.txt | sort | xargs sha256sum ; } | sha256sum
    ```
    （`xargs sha256sum` 的输出格式与清单一致；Windows 上 `sha256sum` 可用 Git 自带的。）
- 许可：**BSD-3-Clause**（见 `COPYING.txt`，逐字保留）

## 为什么 vendor 而不是 vcpkg

1. 上游 registry **没有**这个 port（`../emulator-cxx/vcpkg/ports` 里只有 `mpfr` / `gmp` /
   `boost-multiprecision` / `intx` 一类，都不是逐位可复现的 x87 内核）。
2. 更要紧的是：**我们依赖的是它的逐位行为**。包升级换了算术实现或编译选项，
   会让 `apps/emulator-lift` 的位精确夹具**无声失效** —— 那不是"新版本更好"，而是判据换了一把尺子。
   ⇒ 必须钉死字节（vendor + 上面的清单哈希）。

## 本仓只动了三处（都不是 C 源码）

1. **删掉了另外三个互斥特化目录**：`source/8086-SSE/`、`source/ARM-VFPv2/`、
   `source/ARM-VFPv2-defaultNaN/`、`source/RISCV/`。上游按目标平台选**一个**特化编译；
   x87 的 80 位扩展精度只对应 **8086**。删目录不改任何被编译文件的内容。
2. **新增 `platform.h`**（本仓唯一自己写的文件）。上游把它当**模板**放在
   `build/template-FAST_INT64/platform.h`（文件里带 `==>` 标记，由它的 Makefile 复制进构建目录），
   所以 `source/8086/` 里**没有**它 —— 缺了它编译直接失败（`'platform.h' file not found`）。
   我们的那份照模板的三行（`LITTLEENDIAN` / `INLINE` / `THREAD_LOCAL`）填好，理由写在文件头。
3. **新增 `CMakeLists.txt`**：把 `source/*.c` + `source/8086/*.c` 编成静态库 `berkeley-softfloat`。
   它排除了两类文件（说明写在 CMakeLists 里）：
   - `*M.c` 结尾与 `*MToCommonNaN.c`：**memory 形式**的内部实现，属于"非 FAST_INT64"那一档，
     与 64 位宿主这一档互斥（编进来会立刻报 `softfloat_*M` 未声明）；
   - `*bf16*`：bfloat16 是新格式，8086 特化的 `specialize.h` 没有对应的 NaN 转换函数。

## 怎么用

只有 `src/runtime/fpu_bridge.cpp` `#include "softfloat.h"`，其余代码只认 `fpu_bridge.h` 的
`x87Add` / `x87Sub` / `x87Mul` / `x87Div` / `x87Sqrt` / `x87Compare` / … 这一层抽象。
⛔ 不要把 `softfloat.h` include 进 `exec_fpu.cpp` 之外的任何地方。

★ 一个省事的巧合（写在 `fpu_bridge.cpp` 文件头）：8086 特化的 `extFloat80_t` 布局是
`{uint64 signif; uint16 signExp;}`（16 字节），**前 10 字节与 x86 的 80 位内存格式逐字节相同** ⇒
x87 寄存器槽直接存它，只有 4/8 字节（float/double）的读写需要真转换。

## 怎么改 / 怎么升级

* **升级**：换整个 `source/`，然后把上面两处数字（tarball sha256、清单 sha256）与文件数/字节数
  一起更新；再跑 `ctest` —— **x87 的位精确夹具是这次升级唯一的验收**。
* **本地补丁**：⛔ 不要直接改 `source/` 里的 C 文件。要改行为就在 `fpu_bridge.cpp` 的包装层做
  （例如 pseudo-denormal 的复现、`fscale` 的指数域处理），并在那里写明为什么。
* ⛔ 不要把它挪进 vcpkg，也不要让 CI 用"最新版"（理由见上）。
