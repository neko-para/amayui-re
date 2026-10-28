# 🐞S3 知识层把 0x71 的置位记成 when:'always'，字节处是有条件的

- id: REQ-01M5CWFZVNNHE55YMNP5WQF513
- type: bug
- status: done
- parent: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- verify: tools/test/emulator-engine-scalars.assets.test.mjs#条件位副作用
- repro: packages/age-format/src/engine/layout.mts#ENGINE_SCALAR_BITS
- severity: S3
- tags: [engine scalar bits]

## 分歧

`packages/age-format/src/engine/layout.mts` 的 `ENGINE_SCALAR_BITS` 把 `0x71` 的置位记成 `when: 'always'`；**字节是有条件的**：

```
.text:0041EE89 test    …
.text:0041EE8B jz      short …        ; 条件不成立 ⇒ 跳过下面那条置位
.text:0041EE8D or      dword ptr [ebx+0AAB44h], 8000000h
```

⇒ 按现行表，`0x71` 每次执行都会置 `0xAAB44` 的 bit27；引擎只在那条门成立时置。

## 复现

```bash
pnpm tools disasm-at at --ea 0x41EE89      # 看到 test / jz 这一对门
rg -n "0x71" packages/age-format/src/engine/layout.mts   # 看到 when: 'always'
```

## 影响面（诚实写清）

- 消费方只有旧模拟器 `apps/emulator/src/vm/ops.ts` 的 `applyScalarBits`（**已废弃**，只作架构参考）与这条知识层表本身；
- `data/spec` 的规则**不引用**这张表 ⇒ 规范侧不受影响；
- 但知识层是"可再校验观察"的登记处：表与字节不符会让后人按 `when` 推理，属于 §6 意义上的**失效观察**，必须显式化（不删、不就地改，追加更正记录）。

## 修法（二选一，先取证再改）

1. 补出门的**判据**（`0x41EE89` 那半条 `test` 读的是哪个槽 / 哪一位），把 `when` 换成有条件形态，并追加一条台账更正记录（`replaces` 指向登记 `always` 的那条）。
2. 若门的判据一时取不到 ⇒ 把 `when` 标成"未定"（不许留 `always`），并保留本单作跟踪。

## 注意

- ⛔ 不要顺手改 `apps/emulator/**`（已废弃）；要改的是知识层 + 台账 + （若需要）新守卫用例。
- 台账里 `opcode/0x88+0x71/bit27-side-effects-modeled`（`01M4GYTBDW7S344D4F032M650J`）是**当前** accepted 的 0x88/0x71 记录；本条与它不冲突（那条只说"置位存在、与 0x88 成对"，本单说的是 `when` 的条件性）。
