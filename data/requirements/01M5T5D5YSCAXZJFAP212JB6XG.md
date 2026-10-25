# emulator-cxx · 存档族 7 条（0x19E / 0x1A0 / 0x1A1 / 0x1AB / 0x1AC / 0x1AE / 0x1AF）

- id: REQ-01M5T5D5YSCAXZJFAP212JB6XG
- type: req
- status: open
- parent: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
- verify: tools/test/emulator-cxx-dispatch.test.mjs#★ 分派表 ↔ handler 声明：两个方向都不许漏

## 判据

- `core_selftest` 的 `check_save_ops`（内存 `FakeSaveHost`）：
  - `0x1a0` 打不开 ⇒ 1 且 op3..9 不动；头合法 ⇒ 0 + 年/月/日/时/分/秒/游戏时间（星期与毫秒被跳过）；外族魔数 ⇒ 2。
  - `0x1a1` 打不开 ⇒ 1；打开成功（哪怕头坏）⇒ **op1 不变**且全局整型池被**再 ENC 一遍**（对存储位，不是 DEC∘ENC）。
  - `0x1ab` 两个文件都尝试、**后一步覆盖前一步**（都失败 ⇒ 2）。
  - `0x1ac` 覆盖目标（不因已存在而失败），源 / 目标各拷 `.DAT` + `.STH`。
  - `0x1ae` **先建空文件、后捕捉**；捕捉失败 ⇒ 2 且**不删除**（槽里留 0 字节）。
  - `0x1af` **打不开优先于表面门**（缺文件 + 目标 0 ⇒ 1，不是 2）；合法 BMP ⇒ 0 且纹理落地（行序翻正 + BGR→RGBA）。
- `tools/test/emulator-cxx-dispatch.test.mjs`。

## 实现

- `core/save_host.h`：`SaveHost` 抽象（**三态读**、CREATE_ALWAYS 写、DeleteFileA 语义删、CopyFile 覆盖拷、确认框、表面捕捉）。核心不碰文件系统。
- `core/save_bmp.{h,cpp}`：BMP 容器（写：24 位、声明长度 = 像素 + 40、行自下而上、BGR、行末填充写 0；读：位深 ∈ {1,4,8,24,32}、1/4/8 读调色板、行翻正、短读返回 false）。
- `core/vm_ops_save.cpp`：7 个 handler + `save_directory()`（根 [+ 分隔符] + `set:SavePath`，缺键 `SAVE`）+ `slot_path()`（`SAVE%2.2d.DAT/.STH`）+ `parse_save_header()` + `recode_global_int_pool()`（计数格 + 1 格）。
- `0x19e` 的写侧内容构造与 `0x1a1` 的整档读回属**另外几张单** ⇒ 只留 `slot_builder_` / `slot_loader_` 两个 seam，没注入就**响亮**失败（不假装做到）。

## 待核（已在台账）

- 标题串来源（`set_save_identity` 注入；空串 = 不校验标题）。
- 写盘失败的 op1（引擎是 `sub_40CD10` 的 2/4，本实现用 1 + 诊断）。
- `0x1a1` 头坏时 op1 写不写（本实现不写 + 诊断）。
- 整池重编码在 memflip 双 bank 下的 bank 范围（本实现按当前 bank）。
- 1/4/8/32bpp 缩略图解码无样本（按代码规则写 + 诊断）。
