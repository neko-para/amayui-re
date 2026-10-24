# STH 边界：BMP 容器搬回 core（实现）

- id: REQ-01M5Q002XWK1RKCB5MEMXRWV1V
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-bmp.test.mjs#★ encodeBmp24：24 位头字段（声明长度 = 行宽 × 高 + 40，不是 +54）
- tags: [emulator save]

# 第三十批：缩略图 STH 的 BMP 容器搬回 core

## 一句话

BMP 容器的组装 / 解析归 core，平台只给「像素 + 行宽 + 掩码」；`0x1ae` 先截断建空文件，`0x1af` 先拒 0/1/2。

## 代码

- 新增 `core/bmp.ts`：`encodeBmp24` / `decodeBmp` / `bmpBitDepth` / `bmpRowStride` / `BmpError`。写 24 位、声明长度 `行宽×高+40`、32 位对齐、填充 0、自下而上、BGR；读 24 位、零读者；1/4/8/32 位明确报错。
- `render.ts`：`SurfacePixels`；`captureTexture → SurfacePixels|null`；删 `uploadTextureBytes`。
- `handlers.ts`：`0x1ae` 截断建文件 → 捕捉 → 编码 → 写；`0x1af` 先判 op3 0/1/2 拒（不读文件）→ 解码 → `uploadTexture(RGBA8)`。
- 测试：`emulator-new-bmp` 6 条；`ops-100-200` 4 条；`save-sth.assets` 3 条（含真样本逐字节可逆）。
- 变异迁移到 `bmp.ts`，另加 +40/+54、行序两条。

## 未做

平台两方法本就没有（保持缺省）；32 位写、1/4/8/32 位读展开不建模（`RES-SAVE-STH-DECODE` 继续）。

## 判据

`pnpm test` 1060/0 · `pnpm test:assets` 204/0（1 skip）· `pnpm test:mutation` 358/358 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §53 · `tools/test/emulator-new-bmp.test.mjs`。
