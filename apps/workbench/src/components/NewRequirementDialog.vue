<script setup lang="ts">
/**
 * components/NewRequirementDialog.vue — **快速记一笔**（工作台里唯一的写操作）。
 *
 * ★ 这张表刻意**只问最少的东西**（用户口径 2026-10）：*"我先简单描述，然后我会在 DSH 这边给出单号来细化具体内容"*。
 *   所以这里只有"一句话 + 类型 + 挂在哪条下面"（缺陷再多一项：复现锚点 —— 那是**守卫**要求的，
 *   见下），其余字段（`status` / `order` / `tags` / `verify` / `severity` / 正文…）一律**留给出单号之后那一步**：
 *   `pnpm tools requirements set <短名> …`。
 *   正文不填 ⇒ 服务端按类型给一份骨架（`## 判据` / 缺陷给 `## 复现`），那正是"之后要细化"的位置。
 *
 * ★ 这个组件里**没有任何建单规则**：它把几个值变成一个 JSON 发给 `POST /api/nodes`，
 *   服务端交给模型 `planAdd()`（与 `pnpm tools requirements add` 同一个函数）。所以"网页建的"与
 *   "命令行建的"不可能长得不一样 —— 连**写后守卫没过**时那份不变量清单都是服务端算好回传的（`422` + `report`），
 *   这里只负责**显示**，不重写一遍判据。
 *   ⇒ 所以"缺陷必须写 repro"这条**不在这份代码里**：表单允许你留空，是**服务端**拒回并把理由念给你听。
 *   （缺陷"必须有存在证明"是台账不变量 #1，网页这张快速表不该把它偷偷降级 —— 真要没有锚点，
 *   就先建成**需求**，细化时再定 type。）
 *
 * 两个"少给用户添麻烦"的**界面**取舍（不是规则，规则真源仍在模型 + validate）：
 *   · 类型里不给 `decision`、状态里不给 `superseded`：架构决策**不许写 parent**（不变量 #2），
 *     而这张表永远挂在某个父节点下 ⇒ 那两个选项在这里必然建出一张过不了守卫的单子。
 *   · 父节点必选：树"恰好一个根"（不变量 #2），根已经存在了；没有更好的信息时就默认**挂在根上**。
 */
import { computed, reactive, ref, watch } from 'vue';
import { NAlert, NButton, NForm, NFormItem, NInput, NModal, NRadioButton, NRadioGroup, NSelect, NSpace, useMessage } from 'naive-ui';

import { CreateError, createNode, type CheckReport, type CreateSpec, type TreeRow, type WritesPayload } from '../api';

const props = defineProps<{
  show: boolean;
  /** 服务端给的能力 + 枚举（真源 = 模型）；还没读到就是 null ⇒ 按钮不可按 */
  writes: WritesPayload | null;
  /** 父节点候选：整棵树（`/api/tree` 的行） */
  nodes: TreeRow[];
  /** 打开时预选的父节点（从某条详情页开"新建子需求单"就是那个节点） */
  parentId?: string | null;
}>();

const emit = defineEmits<{
  (e: 'update:show', value: boolean): void;
  (e: 'created', payload: { id: string; short: string; title: string; file: string }): void;
}>();

const message = useMessage();

const open = computed({
  get: () => props.show,
  set: (v: boolean) => emit('update:show', v),
});

const form = reactive({ title: '', type: '', parent: '', repro: '' });

const busy = ref(false);
const problems = ref<string[]>([]);
const failure = ref('');

/** 类型只给这两个：`decision` 不许有 parent（见文件头） */
const TYPES_QUICK: { value: string; label: string }[] = [
  { value: 'req', label: '需求（要交付，有判据）' },
  { value: 'bug', label: '缺陷（可观测分歧）' },
];
const types = computed(() => (props.writes?.types ?? []).filter((t) => t !== 'decision'));
const typeOptions = computed(() => TYPES_QUICK.filter((o) => types.value.includes(o.value)));
const isBug = computed(() => form.type === 'bug');

/** 没给父节点时的缺省：**根**（树只有一个根，本来就是最不意外的落点） */
const rootId = computed(() => props.nodes.find((n) => n.parent === null)?.id ?? '');

/**
 * 父节点候选：整棵树（缩进表示层级；搜索可以按短名 / 标题找）。
 * ★ 从**详情页**打开时只带了 `parentId`（那一页不加载整棵树）⇒ 把预选的那一条**补进选项**，
 *   否则下拉框会显示成空的（值在、标签不在）。想挂到**别的**节点下就从总览页新建。
 */
const parentOptions = computed(() => {
  const opts = props.nodes.map((n) => ({
    label: `${'　'.repeat(Math.max(n.depth, 0))}${n.short} · ${n.title || '(无标题)'}`,
    value: n.id,
  }));
  if (props.parentId && !opts.some((o) => o.value === props.parentId)) {
    opts.unshift({ label: `${props.parentId}（当前这条）`, value: props.parentId });
  }
  return opts;
});

/** 每次打开都回到一份干净的缺省（一次只记一笔，不留上一次的残留） */
watch(
  () => props.show,
  (isOpen) => {
    if (!isOpen) return;
    form.title = '';
    form.type = props.writes?.defaults.type ?? 'req';
    form.parent = props.parentId ?? rootId.value;
    form.repro = '';
    problems.value = [];
    failure.value = '';
  },
);

/** 空串不参与（模型自己有缺省）—— 这正是 `planAdd` 的输入口径 */
function spec(): CreateSpec {
  const out: CreateSpec = { title: form.title.trim(), type: form.type };
  if (form.parent) out.parent = form.parent;
  if (isBug.value && form.repro.trim()) out.repro = form.repro.trim();
  return out;
}

function reportProblems(report: CheckReport | null): string[] {
  if (!report) return [];
  const out: string[] = [];
  for (const c of report.checks) {
    for (const p of c.problems) out.push(`#${c.id} ${p}`);
  }
  return out;
}

async function submit() {
  if (busy.value) return;
  problems.value = [];
  failure.value = '';
  if (form.title.trim() === '') {
    failure.value = '写一句话就行（那是这张单子唯一的必填项）';
    return;
  }
  busy.value = true;
  try {
    const res = await createNode(spec());
    message.success(`已记下 ${res.created.short} —— 细节用这个短名补：pnpm tools requirements set ${res.created.short} …`, {
      duration: 6000,
    });
    open.value = false;
    emit('created', res.created);
  } catch (err) {
    if (err instanceof CreateError) {
      failure.value = err.message;
      problems.value = reportProblems(err.report);
    } else {
      failure.value = (err as Error).message;
    }
  } finally {
    busy.value = false;
  }
}
</script>

<template>
  <n-modal
    v-model:show="open"
    preset="card"
    title="记一笔（需求 / 缺陷）"
    :style="{ width: '38rem', maxWidth: '92vw' }"
    :mask-closable="false"
  >
    <n-alert v-if="!writes?.enabled" type="warning" :show-icon="true" class="mb">
      这个服务现在不能建单：{{ writes?.why ?? '正在读取服务端能力…' }}
      <br />页面照常能看；要建单就用 <code>pnpm tools requirements add</code>，或者把服务绑回回环地址再开。
    </n-alert>

    <n-form label-placement="top" size="small" :disabled="!writes?.enabled">
      <n-form-item label="一句话">
        <n-input
          v-model:value="form.title"
          placeholder="要做什么 / 现象是什么（细节之后用单号补）"
          @keyup.enter="submit"
        />
      </n-form-item>

      <n-form-item label="类型">
        <n-radio-group v-model:value="form.type" size="small">
          <n-radio-button v-for="o in typeOptions" :key="o.value" :value="o.value">{{ o.label }}</n-radio-button>
        </n-radio-group>
      </n-form-item>

      <n-form-item label="挂在哪条下面">
        <n-select v-model:value="form.parent" :options="parentOptions" filterable />
      </n-form-item>

      <n-form-item v-if="isBug" label="复现">
        <n-input v-model:value="form.repro" placeholder="路径#测试名 或 路径#锚点" />
        <template #feedback>
          缺陷的规则是必须有存在证明（台账不变量 #1），所以这一项不给会被守卫拒回；
          此刻还没有锚点就先建成需求，细化时再定类型。
        </template>
      </n-form-item>
    </n-form>

    <div v-if="failure" class="fail">
      <div class="fail-title">{{ failure }}</div>
      <ul v-if="problems.length" class="fail-list">
        <li v-for="(p, i) in problems" :key="i">{{ p }}</li>
      </ul>
      <div v-if="problems.length" class="dim">（服务端已经把刚写的文件删掉了：磁盘上没有留下半成品）</div>
    </div>

    <template #footer>
      <n-space justify="space-between" align="center">
        <span class="dim">
          正文会自动给一份骨架；其余字段（状态 / 收口凭据 / 顺序 / 标签…）之后用短名补：
          <code>pnpm tools requirements set &lt;短名&gt; …</code>
        </span>
        <n-space>
          <n-button size="small" @click="open = false">取消</n-button>
          <n-button size="small" type="primary" :loading="busy" :disabled="!writes?.enabled" @click="submit">
            记下
          </n-button>
        </n-space>
      </n-space>
    </template>
  </n-modal>
</template>

<style scoped>
.mb {
  margin-bottom: 0.8rem;
}
.fail {
  margin-top: 0.6rem;
  border-left: 3px solid var(--err);
  padding: 0.3rem 0.6rem;
  background: color-mix(in srgb, var(--err) 10%, transparent);
  font-size: 13px;
}
.fail-title {
  color: var(--err);
  font-weight: 600;
}
.fail-list {
  margin: 0.2rem 0 0.2rem 1.1rem;
  padding: 0;
}
</style>
