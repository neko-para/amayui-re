# codec key 的真实来源/取值未取证（0x6 的前置条件要求它非 0）

- id: REQ-01M4E4Q1EVBCVQKRVR2ZEVGS3B
- type: req
- status: done
- parent: REQ-01M4AGPP1T9A60HQYW3GX9W592
- done_reason: 取证完成，按节点判据的**后者**收口：一条台账条目把 key 的真实来源钉在 EA 上。① `Engine+0x5EC8C/codec-key-is-rand-derived`（`KN-01M4GTYZ2D2G0V5B2B0Q510G24`，accepted）—— `key = (rand()<<16)+rand()`、种子 `timeGetTime()/100`、全语料唯一写点 EA `0x417359`（本轮自己重跑 `pnpm tools disasm-at at --ea 0x417359 --lines 8` 与 `--ea 0x415953 --lines 22` 复核：`0x415953 call timeGetTime` → `0x41595B mov eax,51EB851Fh` / `0x415960 mul edx` / `0x415962 shr edx,5`（= ÷100，不是 ÷32）→ `0x415966 call _srand` → `0x415970..0x415986` 重抽循环 → `0x417359 mov [esi+5EC8Ch],edx`）；② `Engine+0x5EC90/enc-zero-equals-rol-key-21`（`KN-01M4GTZQ4G222N28074R22640J`，accepted）。两条都挂 `tools/test/engine-value-codec.test.mjs` 的 G1/G3 守卫（会红：`pnpm test:mutation` 里 `keyField.dword 97059→97060` 那条实测非 0 退出码）。③ 源码注释按事实写窄：`apps/emulator/src/host/environment.ts` 的 `ENV_DEFAULTS.codecKey` 现在写明 key **不是配置项、不是自由参数**、随进程而变、默认 0 只对**盘上位模式**与**压栈前的可观测性**无害、**会让 `0x6` 的前置条件不成立**（:70 / :146 两处 + `vm/ops.ts` 的 `opLoadFrame` 注释同步）。④ 台账 `KN-01M4G2PRR82B1C6T4M3D4Z2Z1N` 那句错口径已 append-only 更正：撤回记录 `KN-01M4GYN5SW6N1X5H006B632F6D` + 更正记录 `KN-01M4GYNEQX651P7T6D7C5M6C7N`（`replaces` 指向它）。⑤ `0x6` 的「记一笔不抛」**继续保留**：前置条件不成立的原因是**本层占位值**（`ENV_DEFAULTS.codecKey = 0`）不合法，不是脚本的问题；照抄成抛会让前沿停在"我的占位值不合法"上。记录名仍是 `codec-key-placeholder`，理由已从「来源未取证」改成「引擎那份恒非 0 这事已取证、默认值只是占位」。

## 事实（本轮从逐字读出来的）

`0x6 load-frame` 的前置条件里有 `|| !*(_DWORD *)(this + 388236)` —— 而 `Engine+0x5EC8C`（= 388236）
就是**编解码 key**（另一条 `ROL(Engine[0x5EC90], 11) != key` 是它们之间的**自洽检查**：
`encZero = ENC(key,0) = ROL(key,21)` ⇒ `ROL(encZero,11) = key`，对**任何** key 恒成立）。
⇒ **引擎真的要求 key 非 0**（key = 0 时它抛 `Command_Exit_Exception`）。

## 与本层现状的冲突

`apps/emulator/src/host/environment.ts` 的 `ENV_DEFAULTS.codecKey = 0`，注释写着
"自由参数：它对脚本的可观测行为**没有影响**，只影响盘上位模式"。前半句**至少在这里不成立** ——
引擎会因为 key = 0 而**抛异常**。

## 要做什么

1. **取证**：引擎自己那份 key 是**启动时赋值**的（它的写入点/来源）—— 找到它，或者证明它对本层可观测行为**确实**无影响
   （那样就该把"自由参数"这句话的范围写窄：*除 `0x6` 的前置条件外*）。
2. **在此之前**：`0x6` 的做法是**记一笔不抛**（`machine.note('codec-key-placeholder', …)`）——
   ⛔ 不照抄成抛，否则前沿会停在"我的占位值不合法"上（那是**模型的问题**，不是脚本的问题）。

## 判据

一条守卫能证明"key 的取值改变 ⇒ 除盘上位模式外**没有**可观测差异"，
或者一条台账条目把 key 的真实来源钉在 EA 上。
