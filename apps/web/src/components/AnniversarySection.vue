<script setup lang="ts">
/**
 * “我们”页的纪念日区块（阶段 2 P2-2）：列表（按创建顺序，“加载更多”）与新建表单。
 * 天数只显示服务端按北京时间日期计算的值。结果未确认时保留原内容与请求标识，只在点击“重试”时
 * 用同一标识重发；账号切换时清空草稿（重新挂载表单）。
 */
import { computed, onMounted, ref, watch } from 'vue'

import { anniversaryHeadline, civilDateLabel, KIND_LABEL } from '@/api/anniversary-rules'
import type { AnniversaryTerms } from '@/api/types'
import AnniversaryForm from '@/components/AnniversaryForm.vue'
import { useAnniversariesStore } from '@/stores/anniversaries'
import { useSessionStore } from '@/stores/session'

const session = useSessionStore()
const store = useAnniversariesStore()

const creating = computed(() => store.actions.new)
const partnerPresent = computed(() => (session.space?.member_count ?? 0) >= 2)
const showForm = ref(false)
const formKey = ref(0)

watch(() => session.user?.id ?? null, (id, previous) => {
  if (id === previous) return
  showForm.value = false
  formKey.value += 1
  if (id && session.isAuthenticated) void store.load()
})

onMounted(() => {
  if (session.isAuthenticated) void store.load()
})

async function submit(terms: AnniversaryTerms, notifyPartner: boolean | undefined): Promise<void> {
  const owner = session.user?.id
  const result = await store.create(terms, notifyPartner)
  // Only a confirmed create for the same account clears the form; unknown keeps it for retry.
  if (result.outcome === 'done' && session.user?.id === owner) {
    showForm.value = false
    formKey.value += 1
  }
}

async function retryCreate(): Promise<void> {
  const owner = session.user?.id
  const result = await store.retry('new')
  if (result.outcome === 'done' && session.user?.id === owner) {
    showForm.value = false
    formKey.value += 1
  }
}
</script>

<template>
  <div class="card stack" data-test="anniversary-section" aria-live="polite" :aria-busy="store.status === 'loading'">
    <div class="row">
      <h2 class="card__title spacer">纪念日</h2>
      <button v-if="!showForm" class="btn btn--small btn--soft" type="button" data-test="anniversary-new"
        @click="showForm = true">添加</button>
    </div>

    <template v-if="showForm || creating">
      <AnniversaryForm :key="formKey" name="anniversary-create" submit-label="保存" :busy="!!creating"
        notify-operation="anniversary_create" :partner-present="partnerPresent"
        :captured-notify="creating ? creating.notify === true : null" @submit="submit" />
      <template v-if="creating && creating.status !== 'submitting'">
        <p class="hint hint--error" role="alert" data-test="anniversary-create-pending">{{ creating.errorText }}</p>
        <button class="btn btn--small" type="button" data-test="anniversary-create-retry" @click="retryCreate()">重试保存</button>
      </template>
      <button v-if="!creating" class="btn btn--small btn--ghost" type="button" @click="showForm = false">取消</button>
    </template>

    <ul v-if="store.notices.length" class="list" data-test="anniversary-notices">
      <li v-for="notice in store.notices" :key="notice.id" class="list__item">
        <div class="list__main">
          <div class="list__title" :class="notice.ok ? 'hint--ok' : 'hint--error'">{{ notice.text }}</div>
        </div>
        <button class="btn btn--small btn--ghost" type="button" @click="store.dismissNotice(notice.id)">知道了</button>
      </li>
    </ul>

    <template v-if="store.status === 'error'">
      <p class="hint hint--error" role="alert">{{ store.errorText }}</p>
      <button class="btn btn--small btn--soft" type="button" @click="store.load()">重试</button>
    </template>
    <p v-else-if="store.status !== 'ready'" class="card__hint">加载中…</p>
    <p v-else-if="store.items.length === 0" class="card__hint" data-test="anniversary-empty">还没有纪念日。</p>
    <ul v-else class="list">
      <li v-for="item in store.items" :key="item.id" class="list__item" :data-test="`anniversary-${item.id}`">
        <RouterLink class="list__main message-link" :to="{ name: 'anniversary-detail', params: { id: item.id } }">
          <div class="list__title">{{ item.title }}</div>
          <div class="list__meta">{{ KIND_LABEL[item.kind] }} · {{ civilDateLabel(item.date) }}</div>
          <div class="list__meta" :data-test="`anniversary-headline-${item.id}`">{{ anniversaryHeadline(item) }}</div>
        </RouterLink>
        <span v-if="item.pending_proposal" class="badge badge--accent">有修改申请</span>
        <span v-else-if="item.pinned_by_me" class="badge badge--muted">首页展示</span>
      </li>
    </ul>
    <p v-if="store.moreErrorText" class="hint hint--error" role="alert">{{ store.moreErrorText }}</p>
    <button v-if="store.status === 'ready' && store.nextCursor" class="btn btn--small btn--soft" type="button"
      data-test="anniversary-more" :disabled="store.loadingMore" @click="store.loadMore()">
      {{ store.loadingMore ? '加载中…' : '加载更多' }}
    </button>
  </div>
</template>
