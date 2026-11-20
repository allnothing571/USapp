<script setup lang="ts">
/**
 * New photo memory (P3-1c). A memory needs at least one photo. With uploads off or unknown the page says so, and the form stays: previously
 * uploaded photos can still be chosen. Whether uploads are on comes from the capability query, not the timeline. An unconfirmed create stays
 * locked to what it captured, and survives leaving and returning.
 */
import { computed, onMounted, ref, watch } from 'vue'
import { useRouter } from 'vue-router'

import type { MemoryTerms } from '@/api/types'
import MemoryEditor from '@/components/MemoryEditor.vue'
import PartnerNoticeToggle from '@/components/PartnerNoticeToggle.vue'
import { useMemoriesStore } from '@/stores/memories'
import { useMediaCapabilityStore } from '@/stores/media-capability'
import { useSessionStore } from '@/stores/session'

const session = useSessionStore()
const store = useMemoriesStore()
const router = useRouter()

const capability = useMediaCapabilityStore()
const creating = computed(() => store.actions.new)
const formKey = ref(0)
// D20: off on every fresh form and never remembered; an unconfirmed create shows the choice it captured.
const notify = ref(false)
const partnerPresent = computed(() => (session.space?.member_count ?? 0) >= 2)
const shownNotify = computed(() => (creating.value ? creating.value.notify === true : notify.value))

watch(() => session.user?.id ?? null, (id, previous) => {
  if (id === previous) return
  formKey.value += 1
  notify.value = false
  if (id && session.isAuthenticated) void capability.load()
})

onMounted(() => {
  // Opened directly: the capability query, never the timeline.
  if (session.isAuthenticated) capability.ensure()
})

async function finished(result: { outcome: string; response?: { memory: { id: string } } }, owner: string | undefined): Promise<void> {
  if (result.outcome === 'done' && result.response && session.user?.id === owner) {
    await router.push({ name: 'memory-detail', params: { id: result.response.memory.id } })
  }
}

async function submit(terms: MemoryTerms): Promise<void> {
  const owner = session.user?.id
  await finished(await store.create(terms, notify.value && partnerPresent.value), owner)
}

async function retryCreate(): Promise<void> {
  const owner = session.user?.id
  await finished(await store.retry('new'), owner)
}
</script>

<template>
  <section class="page">
    <header class="row">
      <div class="spacer">
        <h1 class="page__title">添加回忆</h1>
        <p class="page__subtitle">1 到 9 张照片。照片确认可用后才能保存。</p>
      </div>
      <RouterLink class="btn btn--small btn--soft" :to="{ name: 'memories' }">返回</RouterLink>
    </header>

    <div class="card stack" data-test="memory-new">
      <template v-if="!creating">
        <p v-if="capability.uploadsEnabled === false" class="card__hint" data-test="memory-new-off">
          照片上传暂未开放，现在不能上传新照片。仍可选择之前上传、还没有用过的照片来添加回忆；已有的回忆仍可查看。
        </p>
        <p v-else-if="capability.status === 'loading'" class="card__hint" data-test="memory-new-loading">正在确认照片上传是否可用…</p>
        <template v-else-if="capability.uploadsEnabled !== true">
          <p class="card__hint" data-test="memory-new-unknown">暂时无法确认照片上传是否可用。{{ capability.errorText }}</p>
          <button class="btn btn--small btn--soft" type="button" data-test="memory-new-recheck" @click="capability.load()">重新检查</button>
        </template>
      </template>
      <MemoryEditor :key="formKey" name="memory-create" scope="new" submit-label="保存回忆" :busy="!!creating"
        :initial="creating ? creating.terms : null" @submit="submit">
        <PartnerNoticeToggle :key="formKey" name="memory-create" :model-value="shownNotify" :locked="!!creating"
          :unavailable="!partnerPresent" @update:model-value="notify = $event" />
      </MemoryEditor>
      <template v-if="creating && creating.status !== 'submitting'">
        <p class="hint hint--error" role="alert" data-test="memory-create-pending">{{ creating.errorText }}</p>
        <button class="btn btn--small" type="button" data-test="memory-create-retry" @click="retryCreate()">重试保存</button>
      </template>
    </div>
  </section>
</template>
