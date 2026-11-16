# [draw] fa= 的 α 聚合降级为顶点 α ⇒ 逐片元混合 α 失去可观测面（建议做片元级归约）

- id: REQ-01M7B7591WHEVZMYFZ1Q2D8SHQ
- type: req
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M

## 背景（原父单第 7 条留下的欠账）

原父单 `REQ-01M78BF1FNNH6K8N8GEY6GQYXX` 第 7 条「把只在 CPU 光栅上统计的诊断挪到 GPU 路径」**已落地但比要求弱**：
`[draw]` 行照打，但 `fa=` 的口径从「逐**像素**、stage 数学之后的混合 α」降成「逐**顶点** diffuse α」⇒
「这一批最终混出来的 α 是多少」今天**没有可观测面**。本条是「该能力还没实现」，不是「哪里错了」。

来历：`REQ-01M78BF1FNNH6K8N8GEY6GQYXX` 第 7 条的诊断口径降级（该单已收口，本单是**新增能力**、不是它的子项）。

## 现状（2026-11-14 按内容核；该文件同日被并行改动挪过行，行号重核过）

- 聚合在 `apps/emulator-lift/src/runtime/d3d9_pipeline.cpp:4275-4286`：`rasterQueue` **之后**遍历 `gpu_vertices`，
  取 `v.a * 255`（`GpuRasterVertex::a` 是 0..1）。统计对象是 `push_tri` 写的**顶点 diffuse α**（`:4040` 的 `tf_a` /
  `:4045` 的 `v.a / 255.0`）；`CVertex` 的 α 缺省 **255**（`:829`）。
- `n` = **顶点数**（不是像素数），且**面积不加权**：两个像素的三角形与满屏三角形等权。
- α 取自 **stage 数学 / alpha test 之前**：stage 0 的纹理 α、alpha op、TFACTOR 合并结果都不进来；
  `ALPHATESTENABLE=1` 的批今天**整批被拒**（`REQ-01M7AZ65EAE7Q6STW2S0ZNXGM4`）⇒ 那类批连 `[draw]` 行都没有。
- `fa=` 的**打印字段本身没有守卫**：`rg -n "fa=" apps/emulator-lift/tests` **零命中**（2026-11-14 03:47 核）；原来断
  `lastDrawAlphaAgg()` 的用例 `gpu-batch/alpha-aggregation` **在 2026-11-14 03:46 的测试重构里被删掉**了
  （`rg -n "lastDrawAlphaAgg|DrawAlphaAgg" apps/emulator-lift/tests` 零命中）⇒ 整个 α 聚合今天**没有任何断言**。

## 旧口径（CPU 光栅时代，代码已删）

`writeShadedPixel` 里**逐像素、stage 数学之后**的 `c.a`
（`git show 4f1bc7e^:apps/emulator-lift/src/runtime/d3d9_pipeline.cpp` 的 `:1342` → `:1370 const PixelRGBA c = shade();`
→ `:1371-1379` 逐像素累加）；`n` = **像素数**。
⇒ 旧 `fa=` 数的是**进入混合单元的 α**；新 `fa=` 数的是**交进后端的顶点 α**。

## 决定性证据（旧日志，不需要跑 exe）

`.tmp/q2/run28.log` 的坏状态批次（CPU 时代二进制）：
`[draw] frame=2 prim=4 verts=12 … alpha=255.0..255.0 … texa=256:0..0/0 fa=921600:0/0/0`

- `alpha=255.0..255.0` = **顶点 α**（今天 `fa=` 数的就是这个）⇒ 坏状态下它读 **255**；
- `fa=…:0/0/0` = 当年的**逐像素**混合 α ⇒ **0**（stage0 = `4/4(2/3,2/3)` = MODULATE(TEXTURE,TFACTOR)，纹理 α=0 ⇒ 乘积 0）；
- `texa=256:0..0/0` = 绑定纹理自身的 α 采样（这才是根因面）。

**同一批缺陷，新口径会给 255** ⇒ 对「混合 α 被纹理 α 压到 0」这一类**结构性失明**。
修好后（`.tmp/d3d9/art-draw.log`）：`texa=256:0..255/116 … fa=921600:255/255/255`。

## 后果

`REQ-01M735AWXRDZKCDT4124BCFF27`（固定功能批次 α 恒 0）的判据是三条并列，其中 **`fa` 均值 ≠ 0 那一腿今天恒真**
（坏状态下也读 255）⇒ **不能再用 `fa=` 判**。仍然有效的是：

- **`texa=`**：同一行照打、与光栅后端无关（坏状态 `0..0/0` → 修后 `0..255/116`）；
- **像素 dump**（`--dump-frame` / `--present-screenshot`，需跑 exe）。

## 判据（两条路，任选其一交付；⛔ 不用「能编译通过」当验收）

**方案 ①（片元级归约，推荐）**：后端在片元着色器里对**真正参与混合的 α** 做归约（storage buffer / UAV），
每批提交后读回一次；`fa=` 的 `n` 恢复为**像素数**，均值/最小/最大恢复为**逐片元**口径。
验收（同一批里两半都要断）：**人为让纹理 α=0 的那一批**（复现 `.tmp/q2/run28.log` 的坏状态）⇒ `fa` 均值 **= 0**；
正常批 ⇒ `fa` 均值 **> 0**。代价：热路径多一个 UAV + 每批一次小读回；需要的后端能力 = fragment stage 可写
storage/UAV（且 `GpuRaster` 接口要**只**加「α 统计」而不暴露像素）；**要等 `REQ-01M7A74YG08R65NJBFXF7DEJDK` 收口**
（它仍 open，子单 `REQ-01M7AZ65EAE7Q6STW2S0ZNXGM4` 是 alpha test 整批不画 ⇒ 会改变「哪些批进得了 GPU」）。

**方案 ②（每批 bbox 读回 diff）**：批前后读回该批包围盒做差，反推参与混合的 α。
**不可分辨「同色叠加」**（新 α 与旧 α 落在同一混合方程的多解里）；GPU 拿不到片元数 ⇒ **`n` 的语义必须重定**
（例如改成「bbox 内被改动的像素数」并改名，⛔ 不要继续叫 `n`）。验收同上（坏状态 ⇒ `fa` 均值 = 0）。
代价：每批一次回读（热路径；`--present headless` 这档也要付，而 CTest / 无人值守正用它）；依赖同上（`REQ-01M7A74Y…` 收口）。

## 收口时 verify 挂哪条（⛔ 今天不要填）

原候选 `apps/emulator-lift/tests/gpu_batch.cpp#gpu-batch/alpha-aggregation` **2026-11-14 03:46 已被测试重构删掉**
（`rg` 零命中）⇒ 收口时要**新写**一条片元级 α 聚合用例。取数口仍在（`d3d9_pipeline.cpp:5271` 的 `lastDrawAlphaAgg()`；
旧用例断的是顶点口径 `128±8`）。断言两半：**纹理 α=0 的批 ⇒ `fa` 均值 0**、正常批 ⇒ `fa` 均值 > 0。
⛔ 现在填任何锚点都是假的：能力还没实现。
