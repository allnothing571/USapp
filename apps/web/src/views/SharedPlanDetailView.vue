<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { PLAN_DECISION_LABEL, PLAN_STATUS_LABEL, type PlanBody, type PlanCommand, type PlanDecision } from '@/api/shared-plans'
import type { MemoryTerms } from '@/api/types'
import MemoryEditor from '@/components/MemoryEditor.vue'
import SharedPlanForm from '@/components/SharedPlanForm.vue'
import { useMemoriesStore } from '@/stores/memories'
import { useMediaCapabilityStore } from '@/stores/media-capability'
import { useSessionStore } from '@/stores/session'
import { isPlanId, useSharedPlansStore } from '@/stores/shared-plans'

const props = defineProps<{ id: string }>()
const session = useSessionStore()
const store = useSharedPlansStore()
const memories = useMemoriesStore()
const capability = useMediaCapabilityStore()
const id = computed(() => props.id.toLowerCase())
const valid = computed(() => isPlanId(id.value))
const state = computed(() => valid.value ? store.details[id.value] : undefined)
const plan = computed(() => state.value?.plan)
const action = computed(() => valid.value ? store.actions[id.value] : undefined)
const error = computed(() => valid.value ? store.errors[id.value] : undefined)
const me = computed(() => session.user?.id)
const creator = computed(() => !!me.value && me.value === plan.value?.creator_id)
const mode = ref<'edit' | 'link' | 'convert' | null>(null)
const draft = ref<{ version: number; body: PlanBody } | null>(null)
const editorKey = ref(0)
const selectedMemory = ref('')

watch([id, me], ([target, owner]) => {
  mode.value = null
  draft.value = null
  selectedMemory.value = ''
  editorKey.value++
  if (owner && isPlanId(target)) void store.loadDetail(target)
}, { immediate: true })

function openEdit(): void {
  if (!plan.value) return
  draft.value = { version: plan.value.body_version, body: { ...plan.value.body } }
  mode.value = 'edit'
}

function openMemory(next: 'link' | 'convert'): void {
  selectedMemory.value = ''
  mode.value = next
  // The memory list is shown only for linking. Converting needs only whether new uploads are on, from the capability query.
  if (next === 'link') void memories.load()
  else capability.ensure()
}

async function send(command?: PlanCommand): Promise<void> {
  const target = id.value
  const owner = me.value
  const kind = command?.kind ?? action.value?.command.kind
  const result = command ? await store.submit(command) : await store.retry(target)
  if (result.ok && id.value === target && owner === me.value && kind !== 'advance' && kind !== 'decide') {
    mode.value = null
    draft.value = null
    editorKey.value++
  }
}

function saveBody(body: PlanBody): void {
  if (draft.value) void send({ kind: creator.value ? 'edit' : 'propose', id: id.value, version: draft.value.version, body })
}

function decide(outcome: PlanDecision): void {
  const proposal = plan.value?.pending_proposal
  if (proposal) void send({ kind: 'decide', id: id.value, proposalId: proposal.id, outcome,
    baseVersion: proposal.base_version, suggested: { ...proposal.suggested } })
}

function advance(): void {
  const item = plan.value
  if (item && item.status !== 'finished') void send({ kind: 'advance', id: item.id, from: item.status, version: item.status_version })
}

function link(): void {
  if (selectedMemory.value) void send({ kind: 'link', id: id.value, memoryId: selectedMemory.value })
}

function convert(terms: MemoryTerms): void {
  void send({ kind: 'convert', id: id.value, terms })
}
</script>

<template>
  <section class="page">
    <header class="row">
      <h1 class="page__title spacer">计划详情</h1>
      <RouterLink class="btn btn--small btn--soft" :to="{ name: 'shared-plans' }">返回计划</RouterLink>
    </header>
    <p v-if="!valid" class="hint hint--error" role="alert">计划不存在。</p>
    <p v-else-if="state?.loading && !plan" class="card__hint">加载中…</p>
    <div v-if="state?.error" class="card stack">
      <p class="hint hint--error" role="alert">{{ state.error }}</p>
      <button type="button" class="btn btn--small" @click="store.loadDetail(id)">重新加载</button>
    </div>

    <div v-if="action || error" class="card stack" data-test="plan-action">
      <p v-if="action?.state === 'sending'" class="card__hint">正在提交…</p>
      <p v-if="action?.error || error" class="hint hint--error" role="alert">{{ action?.error || error }}</p>
      <button v-if="action && action.state !== 'sending'" type="button" class="btn btn--small"
        data-test="plan-retry" @click="send()">重试原请求</button>
    </div>

    <template v-if="plan">
      <article class="card stack">
        <h2 class="card__title" data-test="plan-detail-title">{{ plan.body.title }}</h2>
        <p class="card__hint" data-test="plan-status">{{ PLAN_STATUS_LABEL[plan.status] }}</p>
        <p v-if="plan.body.date">{{ plan.body.date }}<template v-if="plan.body.time"> {{ plan.body.time }}（北京时间）</template></p>
        <p v-if="plan.body.place">地点：{{ plan.body.place }}</p>
        <p v-if="plan.body.note" class="shared-plan-note">{{ plan.body.note }}</p>
        <div class="row">
          <button v-if="plan.status !== 'finished'" type="button" class="btn btn--small" :disabled="!!action"
            data-test="plan-advance" @click="advance">{{ plan.status === 'want' ? '开始计划' : '标记完成' }}</button>
          <button v-if="!plan.pending_proposal && mode !== 'edit'" type="button" class="btn btn--small btn--soft" :disabled="!!action"
            data-test="plan-edit" @click="openEdit">{{ creator ? '修改正文' : '请求修改' }}</button>
        </div>
      </article>

      <article v-if="plan.pending_proposal" class="card stack" data-test="plan-proposal">
        <h2 class="card__title">待处理的修改请求</h2>
        <dl>
          <template v-for="[label, field] in ([['标题','title'], ['日期','date'], ['时间','time'], ['地点','place'], ['备注','note']] as const)" :key="field">
            <dt>{{ label }}</dt>
            <dd class="shared-plan-note">原内容：{{ plan.pending_proposal.original[field] ?? '未填写' }}</dd>
            <dd class="shared-plan-note">建议：{{ plan.pending_proposal.suggested[field] ?? '未填写' }}</dd>
          </template>
        </dl>
        <div class="row">
          <template v-if="creator">
            <button type="button" class="btn btn--small" :disabled="!!action" data-test="plan-approve" @click="decide('approved')">同意</button>
            <button type="button" class="btn btn--small btn--soft" :disabled="!!action" data-test="plan-reject" @click="decide('rejected')">拒绝</button>
          </template>
          <button v-else-if="me === plan.pending_proposal.proposer_id" type="button" class="btn btn--small btn--soft"
            :disabled="!!action" data-test="plan-withdraw" @click="decide('withdrawn')">撤回请求</button>
        </div>
      </article>
      <p v-if="plan.latest_decision" class="card card__hint" data-test="plan-latest-decision">
        最近一次修改请求：{{ PLAN_DECISION_LABEL[plan.latest_decision.outcome] }}
      </p>

      <div v-if="mode === 'edit' && draft && !plan.pending_proposal" class="card stack">
        <SharedPlanForm :initial="draft.body" :label="creator ? '保存修改' : '提交修改请求'" :busy="!!action" @submit="saveBody" />
        <button v-if="!action" type="button" class="btn btn--small btn--soft" @click="mode = null">取消</button>
      </div>

      <article v-if="plan.status === 'finished'" class="card stack">
        <h2 class="card__title">关联回忆</h2>
        <p v-if="plan.memory_unavailable" class="card__hint" data-test="plan-memory-unavailable">关联的回忆已不可用。原关联保留，无法更换。</p>
        <RouterLink v-else-if="plan.memory_id" class="btn btn--small btn--soft"
          :to="{ name: 'memory-detail', params: { id: plan.memory_id } }" data-test="plan-memory">查看已关联的回忆</RouterLink>
        <template v-else>
          <p class="card__hint">每个计划可以关联一条回忆。关联后无法更换。</p>
          <div class="row">
            <button type="button" class="btn btn--small btn--soft" :disabled="!!action" @click="openMemory('link')">选择已有回忆</button>
            <button type="button" class="btn btn--small btn--soft" :disabled="!!action" @click="openMemory('convert')">新建照片回忆</button>
          </div>
          <template v-if="mode === 'link'">
            <label class="field"><span class="field__label">已有回忆</span>
              <select v-model="selectedMemory" class="field__input" :disabled="!!action || memories.status === 'loading'" data-test="plan-memory-select">
                <option value="">请选择</option>
                <option v-for="memory in memories.items" :key="memory.id" :value="memory.id">{{ memory.date }} · {{ memory.title }}</option>
              </select>
            </label>
            <p v-if="memories.errorText || memories.moreErrorText" class="hint hint--error" role="alert">{{ memories.errorText || memories.moreErrorText }}</p>
            <button v-if="memories.errorText" class="btn btn--small btn--soft" type="button" @click="memories.load()">重新读取回忆</button>
            <button v-if="memories.nextCursor" class="btn btn--small btn--soft" type="button" :disabled="memories.loadingMore" @click="memories.loadMore()">加载更多回忆</button>
            <button class="btn" type="button" :disabled="!!action || !selectedMemory" data-test="plan-link-memory" @click="link">确认关联</button>
          </template>
          <template v-if="mode === 'convert' || action?.command.kind === 'convert'">
            <p v-if="capability.uploadsEnabled !== true && action?.command.kind !== 'convert'" class="card__hint" data-test="plan-memory-uploads-note">
              {{ capability.uploadsEnabled === false ? '照片上传暂未开放，仍可关联已有回忆，或选择之前上传、还没有用过的照片。' : '正在确认或暂时无法确认照片上传是否可用；仍可选择之前上传的照片。' }}
            </p>
            <MemoryEditor :key="editorKey"
              name="plan-memory-create" :scope="'plan-memory:' + id" submit-label="保存并关联"
              :busy="!!action" :initial="action?.command.kind === 'convert' ? action.command.terms : null" @submit="convert" />
          </template>
        </template>
      </article>
    </template>
  </section>
</template>

<style scoped>
.shared-plan-note { white-space: pre-wrap; overflow-wrap: anywhere; }
</style>
