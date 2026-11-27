<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { useRouter } from 'vue-router'
import { PLAN_STATUS_LABEL, type PlanBody } from '@/api/shared-plans'
import PartnerNoticeToggle from '@/components/PartnerNoticeToggle.vue'
import SharedPlanForm from '@/components/SharedPlanForm.vue'
import { useSessionStore } from '@/stores/session'
import { useSharedPlansStore } from '@/stores/shared-plans'
import { useSmartBack } from '@/router/back-navigation'

const back = useSmartBack('shared-plans', 'us')
const router = useRouter()
const session = useSessionStore()
const store = useSharedPlansStore()
const creating = ref(false)
const formKey = ref(0)
const pending = computed(() => store.actions.new)
// D20: off on every fresh form and never remembered; a pending create shows the choice it captured.
const notify = ref(false)
const partnerPresent = computed(() => (session.space?.member_count ?? 0) >= 2)
const shownNotify = computed(() => (pending.value?.command.kind === 'create' ? pending.value.command.notify === true : notify.value))

function startForm(): void {
  notify.value = false
  creating.value = true
}

watch(() => session.user?.id, id => {
  creating.value = false
  notify.value = false
  formKey.value++
  if (id) void store.loadList()
}, { immediate: true })

async function submit(body?: PlanBody): Promise<void> {
  const owner = session.user?.id
  const result = body ? await store.submit({ kind: 'create', body, notify: notify.value && partnerPresent.value }) : await store.retry('new')
  if (result.ok && owner && owner === session.user?.id && router.currentRoute.value.name === 'shared-plans') {
    await router.push({ name: 'shared-plan-detail', params: { id: result.id } })
  }
}
</script>

<template>
  <section class="page">
    <header class="subpage-header">
      <RouterLink class="back-pill" :to="back.to" data-test="shared-plans-back">‹ 返回{{ back.label }}</RouterLink>
      <h1 class="subpage-header__title">共享计划</h1>
      <p class="subpage-header__subtitle">把想一起做的事，从想法变成回忆。</p>
    </header>

    <div class="card stack">
      <button v-if="!creating && !pending" type="button" class="btn" data-test="new-plan" @click="startForm()">添加计划</button>
      <template v-if="creating || pending">
        <SharedPlanForm :key="formKey" label="保存计划" :busy="!!pending"
          :initial="pending?.command.kind === 'create' ? pending.command.body : undefined" @submit="submit">
          <PartnerNoticeToggle name="plan-create" :model-value="shownNotify" :locked="!!pending"
            :unavailable="!partnerPresent" @update:model-value="notify = $event" />
        </SharedPlanForm>
        <button v-if="!pending" type="button" class="btn btn--small btn--soft" @click="creating = false">取消</button>
        <p v-if="pending?.error || store.errors.new" class="hint hint--error" role="alert">{{ pending?.error || store.errors.new }}</p>
        <button v-if="pending && pending.state !== 'sending'" type="button" class="btn btn--small" @click="submit()">重试原请求</button>
      </template>
    </div>

    <p v-if="store.listLoading && !store.items.length" class="card__hint">加载中…</p>
    <div v-if="store.listError" class="card stack">
      <p class="hint hint--error" role="alert">{{ store.listError }}</p>
      <button type="button" class="btn btn--small" @click="store.loadList()">重新加载</button>
    </div>
    <p v-else-if="!store.listLoading && !store.items.length" class="card__hint">还没有共享计划。</p>

    <RouterLink v-for="plan in store.items" :key="plan.id" class="card message-link"
      :to="{ name: 'shared-plan-detail', params: { id: plan.id } }" data-test="plan-list-item">
      <h2 class="card__title">{{ plan.body.title }}</h2>
      <p class="card__hint">{{ PLAN_STATUS_LABEL[plan.status] }}<template v-if="plan.body.date"> · {{ plan.body.date }}</template></p>
    </RouterLink>
    <button v-if="store.cursor" type="button" class="btn btn--soft" :disabled="store.listLoading"
      data-test="plans-more" @click="store.loadList(true)">加载更多</button>
  </section>
</template>
