# spec：第四轮复核 S2/S3 清理（同章两口径 / 状态词 / 正文机器细节 / 过期残差）

- id: REQ-01M5FBD5DWWJEH48Q4T5NZAN2Q
- type: req
- status: done
- parent: REQ-01M5D1G31PN9EGANRVAR753BPC
- done_reason: S2/S3 与机器细节 30 组逐条清理完成；无单一能力守卫可挂，机械判据为 spec validate 13 条 + spec.test.mjs 判别力用例；6 个仍需取证的缺口已进 audit 残差并转 REQ-01M5FKFVSQHA7QSFVVCYYQQ3YQ
- tags: [spec, review, cleanup]

## 结果（2026-10-21/22）

S2（15 条）+ S3（19 条）+ 正文机器细节 8 项，**30 组逐条已处理，无"跳过"**。落点全部在 `repo:data/spec/`（正文 / bindings / audit；另 `format/verification.md` 等台账文本同步）。逐条清单见 git diff 与本节末尾的要点：

* **同章两口径**：`40-pools` raw=下标限定池族 3–14；`0x8005`/`0x800B` 的池分别写明；容器句柄与 `0x32f` 互引（同一性留残差）；`55-config` 池下标→键下标、写规则限定标量键；`0x192` 的"文本读原语"改为"取文本 + 本条格式化"；`60-ops` 末尾新增「异常与错误通道」定义退出型异常。
* **状态与断言强度**：默认组无观察 ⇒ `0x70` / `0x79+0x1c1` / `0x303` 降 `unhandled`（后由 `REQ-01M5D4NN…` 补证后升回 `handled`）；颜色哨兵无数值 ⇒ 五条颜色规则降 `unhandled` + 残差；`0x71` 的 bit 27 补进正文。
* **正文机器细节**：元素宽度 4/28 字节与阈值 16 定为合同（从"机器细节不规定"划出）；`55-config` 的 12 字节缓冲删除。
* **残差过期**：位置表残差改写（单位与基已由 `15-frame` 统一）；帧记录生命周期改为四转移逐字段；删一条重复的"子表元素结构未成篇"。

## 边界 / 未做

* **没有为每一类新加一条守卫**：文本一致性没有不脆弱的机械判据（"不许出现某字符串"式的守卫按 `AGENTS.md` §10 不建）；机械判据仍是 `spec validate` 13 条 + `tools/test/spec.test.mjs` 的判别力用例（27/27）。
* **6 个证据缺口不在本单**：`0x32f` 容器同一性 / 三张位置表语义 / 颜色位段 / 负组号 / `file_index` 已逐条进 `audit` 残差（带 next），并交由 `REQ-01M5FKFVSQHA7QSFVVCYYQQ3YQ` 跟踪。
* 参考实现里仍有与正文不一致处（`0x1ca` / `0x324` / `0x73` 等），已记残差，不在本单改 emulator。

## 为什么用 done_reason

本单是**文档清理类**：交付物是逐条裁决与改写，没有单一"能力守卫"能代表它；机械可判的部分已由 `spec validate` + `spec.test.mjs` 覆盖。

## 怎么查

`pnpm tools spec validate` · `pnpm tools spec report` · `pnpm tools spec show <规则 id>`。
