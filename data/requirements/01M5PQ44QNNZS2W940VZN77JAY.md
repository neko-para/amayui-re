# 缩略图 STH：BMP 容器搬回 core（实现边界）

- id: REQ-01M5PQ44QNNZS2W940VZN77JAY
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-bmp.test.mjs#★ encodeBmp24：24 位头字段（声明长度 = 行宽 × 高 + 40，不是 +54）
- tags: [emulator save]

# 缩略图 STH：BMP 容器搬回 core（实现边界）
## 现状
合同：容器由 core 自建 / 自解，平台只给像素 + 行宽 + 通道掩码。当前 `RenderHost.captureTexture` 返回整份 BMP、`uploadTextureBytes` 收整份 BMP。
## 要做
- core 新增 `bmp.ts`：编码 24 / 解码 24 / 位深探测（纯核心）。
- `captureTexture → {pixels,width,height,stride,bytesPerPixel,masks}`；删 `uploadTextureBytes`，读侧复用 `uploadTexture(RGBA8)`。
- `0x1ae`：先截断建空文件 → 对象宽高 → 编码写；`0x1af`：先判 op3 无符号 ∈{0,1,2} 拒（不读内容）→ 解码 → 上传。
- 平台侧（Local/Pixi）今天未实现这两个方法，可先缺省；protocol.ts 不改。
- 测试：删“整份字节”2 条、补 ~6 条；`tools/mutate-check.mjs` 两条旧变异点同步迁移。
## 判据
`pnpm test` / `typecheck` / `test:assets` 绿；`0x1ae/0x1af` 行为按 FMT-SAVE-STH。
