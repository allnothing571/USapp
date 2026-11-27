<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from 'vue'
import { getSharedGarden, type SharedGarden } from '@/api/shared-garden'
import { GARDEN_STAGES, gardenStageIndex } from '@/shared-garden-growth'
import { useSessionStore } from '@/stores/session'

const props = defineProps<{ refreshKey: number; summaryOnly?: boolean; asLink?: boolean }>()
const session = useSessionStore()
const garden = ref<SharedGarden | null>(null)
const busy = ref(false)
const unavailable = ref(false)
const available = computed(() => Boolean(session.isAuthenticated && session.user && session.space))
const stage = computed(() => GARDEN_STAGES[gardenStageIndex(garden.value?.total_waterings ?? 0)]!)
let activeRead: AbortController | null = null

function cancelRead() {
  const previous = activeRead
  activeRead = null
  previous?.abort()
}

async function refresh() {
  cancelRead()
  garden.value = null
  unavailable.value = false
  busy.value = false
  if (!available.value) return

  const read = new AbortController()
  activeRead = read
  busy.value = true
  const deadline = window.setTimeout(() => {
    cancelRead()
    busy.value = false
    unavailable.value = true
  }, 8_000)
  read.signal.addEventListener('abort', () => window.clearTimeout(deadline), { once: true })
  try {
    const next = await getSharedGarden(read.signal)
    if (activeRead === read) garden.value = next
  } catch {
    if (activeRead === read) unavailable.value = true
  } finally {
    window.clearTimeout(deadline)
    if (activeRead === read) {
      activeRead = null
      busy.value = false
    }
  }
}

watch([() => session.user?.id, () => session.space?.id, available], refresh, { immediate: true, flush: 'sync' })
watch(() => props.refreshKey, refresh)
onBeforeUnmount(cancelRead)
</script>

<template>
  <RouterLink
    v-if="available && asLink"
    :to="{ name: 'shared-garden' }"
    class="hub-card"
    data-test="brief-garden"
    aria-labelledby="home-garden-title"
  >
    <span class="hub-card__icon hub-card__icon--green" aria-hidden="true">
      <span class="room-art home-garden-art--sm" data-item="garden"></span>
    </span>
    <span class="hub-card__body">
      <span id="home-garden-title" class="hub-card__title">共享花园</span>
      <span class="hub-card__sub" aria-live="polite" :aria-busy="busy">
        <template v-if="garden">
          {{ stage.name }} · 累计浇水 {{ garden.total_waterings }} 次 · 今天{{ garden.watered_today ? '我已浇水' : '我待浇水' }}
        </template>
        <template v-else-if="unavailable">暂时无法读取花园进度，点击进入花园</template>
        <template v-else>正在读取花园…</template>
      </span>
    </span>
    <span class="hub-card__meta">
      <span v-if="garden && !garden.watered_today" class="hub-badge">待浇水</span>
      <span class="hub-arrow" aria-hidden="true">›</span>
    </span>
  </RouterLink>
  <section v-else-if="available" class="card home-garden" aria-labelledby="home-garden-title">
    <span class="room-art home-garden-art" data-item="garden" aria-hidden="true"></span>
    <div class="home-garden-copy">
      <h2 id="home-garden-title" class="card__title">共享花园</h2>
      <div aria-live="polite" :aria-busy="busy">
        <template v-if="garden">
          <p class="home-garden-stage">{{ stage.name }} <span>· 累计浇水 {{ garden.total_waterings }} 次</span></p>
          <p class="card__hint">
            <time :datetime="garden.watering_date">今天</time> · {{ garden.watered_today ? '我已浇水' : '我待浇水' }} ·
            {{ garden.partner_watered_today ? '对方已浇水' : session.space?.member_count === 1 ? '对方尚未加入' : '对方待浇水' }}
          </p>
        </template>
        <p v-else-if="unavailable" class="card__hint">暂时无法读取花园进度。</p>
        <p v-else class="card__hint">正在读取花园…</p>
      </div>
    </div>
    <div v-if="unavailable || !summaryOnly" class="home-garden-actions">
      <button v-if="unavailable" type="button" class="btn btn--small btn--ghost" @click="refresh()">重试</button>
      <RouterLink v-if="!summaryOnly" class="btn btn--small btn--soft" :to="{ name: 'shared-garden' }">去共享花园</RouterLink>
    </div>
  </section>
</template>

<style scoped>
.home-garden { display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: .65rem 1rem; border-inline-start: .25rem solid #729570; }
.home-garden-art { width: 3rem; }
.home-garden-art--sm { width: 1.7rem; }
.home-garden-copy { min-width: 0; flex: 1 1 11rem; }
.home-garden-stage { margin: .2rem 0; color: #315338; font-weight: 650; }
.home-garden-stage span { color: inherit; font-size: .9rem; font-weight: 500; }
.home-garden .card__hint { margin: .2rem 0 0; }
.home-garden-actions { display: flex; gap: .45rem; flex-wrap: wrap; }
</style>
