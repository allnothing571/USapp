<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { describeError, errorCodeOf } from '@/api/error-messages'
import { getSharedGarden, waterSharedGarden, type SharedGarden } from '@/api/shared-garden'
import { GARDEN_STAGES, gardenStageIndex } from '@/shared-garden-growth'
import { useSessionStore } from '@/stores/session'

const session = useSessionStore()
const garden = ref<SharedGarden | null>(null)
const busy = ref(false)
const notice = ref<'recorded' | 'date-changed' | 'awaiting-sync' | null>(null)
const error = ref('')
const syncUnavailable = ref(false)
const waterEffect = ref(false)
let waterEffectTimer: number | undefined
let generation = 0
let syncRead: AbortController | null = null
let syncInterval: number | undefined

const stage = computed(() => gardenStageIndex(garden.value?.total_waterings ?? 0))
const currentStage = computed(() => GARDEN_STAGES[stage.value]!)
const nextStage = computed(() => GARDEN_STAGES[stage.value + 1])
const together = computed(() => garden.value?.watered_today && garden.value.partner_watered_today)
const feedback = computed(() => {
  if (notice.value === 'date-changed') return '浇水日期已更新。请查看今天的状态。'
  if (together.value) return '今天一起浇过水了'
  if (notice.value === 'recorded') return '浇水已记录。'
  if (notice.value === 'awaiting-sync' && !garden.value?.watered_today) return '浇水已提交，正在等待同步。'
  return ''
})

function clearWaterEffect() {
  window.clearTimeout(waterEffectTimer)
  waterEffect.value = false
}

function cancelSync() {
  const read = syncRead
  syncRead = null
  read?.abort()
}

async function syncGarden() {
  const current = garden.value
  if (!current || busy.value || syncRead || document.hidden) return
  const read = new AbortController()
  syncRead = read
  const deadline = window.setTimeout(() => {
    cancelSync()
    syncUnavailable.value = true
  }, 8_000)
  read.signal.addEventListener('abort', () => window.clearTimeout(deadline), { once: true })
  try {
    const snapshot = await getSharedGarden(read.signal)
    if (syncRead !== read) return
    if (snapshot.watering_date !== current.watering_date) {
      notice.value = null
      clearWaterEffect()
    }
    garden.value = snapshot
    syncUnavailable.value = false
  } catch (problem) {
    if (syncRead !== read) return
    if (['UNAUTHENTICATED', 'FORBIDDEN', 'SPACE_NOT_FOUND'].includes(errorCodeOf(problem) ?? '')) {
      garden.value = null
      clearWaterEffect()
      notice.value = null
      error.value = describeError(problem)
      syncUnavailable.value = false
    } else {
      syncUnavailable.value = true
    }
  } finally {
    window.clearTimeout(deadline)
    if (syncRead === read) syncRead = null
  }
}

async function refresh(wateringDate?: string) {
  if (busy.value) return
  cancelSync()
  clearWaterEffect()
  const operation = ++generation
  busy.value = true
  error.value = ''
  notice.value = null
  syncUnavailable.value = false
  try {
    let wateringAccepted = false
    if (wateringDate) {
      try {
        await waterSharedGarden(wateringDate)
        wateringAccepted = true
      } catch (problem) {
        if (errorCodeOf(problem) !== 'SHARED_GARDEN_DATE_CHANGED') throw problem
      }
      if (operation !== generation) return
    }
    const snapshot = await getSharedGarden()
    if (operation !== generation) return
    garden.value = snapshot
    if (wateringDate && (!wateringAccepted || snapshot.watering_date !== wateringDate)) {
      notice.value = 'date-changed'
    } else if (wateringAccepted && snapshot.watered_today) {
      notice.value = 'recorded'
      waterEffect.value = true
      waterEffectTimer = window.setTimeout(clearWaterEffect, 1100)
    } else if (wateringAccepted) {
      notice.value = 'awaiting-sync'
    }
  } catch (problem) {
    if (operation === generation) {
      garden.value = null
      error.value = describeError(problem)
    }
  } finally {
    if (operation === generation) busy.value = false
  }
}

watch([() => session.user?.id, () => session.space?.id], ([user, space]) => {
  generation++
  cancelSync()
  clearWaterEffect()
  garden.value = null
  busy.value = false
  notice.value = null
  error.value = ''
  syncUnavailable.value = false
  if (user && space) void refresh()
}, { immediate: true, flush: 'sync' })

function refreshWhenVisible() {
  if (document.hidden) {
    cancelSync()
  } else if (garden.value) {
    void syncGarden()
  } else if (session.user && session.space) {
    void refresh()
  }
}

onMounted(() => {
  syncInterval = window.setInterval(syncGarden, 15_000)
  document.addEventListener('visibilitychange', refreshWhenVisible)
})
onBeforeUnmount(() => {
  generation++
  cancelSync()
  clearWaterEffect()
  window.clearInterval(syncInterval)
  document.removeEventListener('visibilitychange', refreshWhenVisible)
})
</script>

<template>
  <section class="shared-garden" aria-labelledby="garden-title">
    <header class="garden-heading scene-page-heading">
      <RouterLink class="scene-back" :to="{ name: 'shared-room' }" aria-label="返回小房间">‹</RouterLink>
      <h1 id="garden-title">共享花园</h1>
      <button type="button" class="btn btn--ghost btn--small" :disabled="busy" @click="refresh()">刷新花园</button>
    </header>
    <p v-if="error" role="alert" class="hint hint--error">{{ error }}</p>
    <p v-else-if="!garden && busy" role="status">正在读取花园…</p>
    <p v-else-if="syncUnavailable" role="status" class="hint">暂时无法自动同步，显示上次读取的进度。</p>

    <template v-if="garden">
      <section class="garden-scene" aria-labelledby="garden-stage">
        <h2 id="garden-stage">{{ currentStage.name }}</h2>
        <div class="garden-plant" :data-stage="stage" aria-hidden="true"></div>
        <div v-if="waterEffect" class="garden-drops" aria-hidden="true"><i></i><i></i><i></i><i></i></div>
      </section>
      <section class="garden-today" aria-labelledby="watering-title">
        <div class="garden-today-heading">
          <h2 id="watering-title">今日浇水</h2>
          <time :datetime="garden.watering_date">{{ garden.watering_date }} · 北京时间</time>
        </div>
        <dl class="garden-members">
          <div :class="{ watered: garden.watered_today }"><dt>我</dt><dd>{{ garden.watered_today ? '已浇水' : '待浇水' }}</dd></div>
          <div :class="{ watered: garden.partner_watered_today }"><dt>对方</dt><dd>{{ garden.partner_watered_today ? '已浇水' : session.space?.member_count === 1 ? '尚未加入' : '待浇水' }}</dd></div>
        </dl>
        <p v-if="feedback" :role="syncUnavailable ? undefined : 'status'" class="garden-feedback">
          {{ feedback }}
        </p>
        <button type="button" class="btn garden-water" :class="{ 'garden-water--done': garden.watered_today }"
          :disabled="busy || garden.watered_today" @click="refresh(garden.watering_date)">
          {{ garden.watered_today ? '今日已浇水' : busy ? '处理中…' : '给植物浇水' }}
        </button>
        <RouterLink class="garden-records" :to="{ name: 'shared-garden-history' }">浇水记录</RouterLink>
      </section>

      <section class="garden-progress card card--soft" aria-labelledby="garden-growth-title">
        <div class="garden-today-heading">
          <h2 id="garden-growth-title">成长进度</h2>
          <p>累计浇水 <strong>{{ garden.total_waterings }}</strong> 次</p>
        </div>
        <template v-if="nextStage">
          <label for="garden-growth">距离{{ nextStage.name }}还有 {{ nextStage.from - garden.total_waterings }} 次</label>
          <progress id="garden-growth" :value="garden.total_waterings - currentStage.from" :max="nextStage.from - currentStage.from"></progress>
        </template>
        <p class="hint">每天每人可浇一次，少浇一天也不会影响已积累的成长。</p>
      </section>

      <section v-if="garden.recent_waterings" class="garden-recent" aria-labelledby="garden-recent-title">
        <div class="garden-today-heading">
          <h2 id="garden-recent-title">最近 7 天</h2>
        </div>
        <table aria-labelledby="garden-recent-title">
          <thead><tr><th scope="col">日期</th><th scope="col">我</th><th scope="col">对方</th></tr></thead>
          <tbody>
            <tr v-for="day in garden.recent_waterings" :key="day.watering_date">
              <th scope="row"><time :datetime="day.watering_date">{{ day.watering_date.slice(5) }}</time></th>
              <td :class="{ watered: day.watered }">{{ day.watered ? '已浇水' : '未浇水' }}</td>
              <td :class="{ watered: day.partner_watered }">
                {{ day.partner_watered ? '已浇水' : session.space?.member_count === 1 ? '尚未加入' : '未浇水' }}
              </td>
            </tr>
          </tbody>
        </table>
      </section>
    </template>
  </section>
</template>

<style scoped>
.shared-garden { max-width: 32rem; margin: auto; }
.garden-heading { --scene-side: 5rem; }
.garden-heading .btn { padding-inline: .4rem; min-height: 44px; }
.garden-scene { position: relative; isolation: isolate; aspect-ratio: 1.04; overflow: hidden; border: 1px solid #c6a56c; border-radius: 1.3rem; background: #e9e0bb url('/garden-art/courtyard.jpg') center / cover no-repeat; }
.garden-scene h2 { position: absolute; z-index: 1; top: .7rem; right: .7rem; margin: 0; padding: .25rem .7rem; border-radius: 2rem; background: var(--surface); color: var(--green); font-size: .85rem; }
.garden-plant { position: absolute; left: 50%; bottom: -4%; width: 76%; aspect-ratio: 1; translate: -50%; background: url('/garden-art/plant-stages.png') 0 0 / 200% 200% no-repeat; }
.garden-plant[data-stage="1"] { background-position: 100% 0; }
.garden-plant[data-stage="2"] { bottom: 4%; background-position: 0 100%; }
.garden-plant[data-stage="3"] { bottom: 4%; background-position: 100% 100%; }
.garden-drops { position: absolute; inset: 26% 24% 18%; pointer-events: none; }
.garden-drops i { position: absolute; top: 10%; left: 14%; width: .52rem; height: .76rem; border: 1px solid #fcfff1; border-radius: 50% 50% 68% 68%; background: #f4fff2b5; box-shadow: 0 0 .4rem #fff4c8; opacity: 0; animation: garden-drop .9s ease-out; }
.garden-drops i:nth-child(2) { left: 37%; top: 30%; animation-delay: 100ms; }
.garden-drops i:nth-child(3) { left: 61%; top: 8%; animation-delay: 50ms; }
.garden-drops i:nth-child(4) { left: 83%; top: 34%; animation-delay: 160ms; }
@keyframes garden-drop {
  20% { opacity: .95; }
  72% { opacity: .85; }
  100% { opacity: 0; transform: translateY(2.4rem); }
}
@media (max-width: 480px) { .garden-scene { aspect-ratio: .93; } }
@media (prefers-reduced-motion: reduce) { .garden-drops i { opacity: .9; animation: none; } }
.garden-today { padding: .8rem 0 1rem; }
.garden-today-heading { display: flex; justify-content: space-between; align-items: baseline; flex-wrap: wrap; gap: .3rem .7rem; }
.garden-today-heading h2 { margin: 0; font-size: 1rem; }
.garden-today-heading time, .garden-progress label { font-size: .82rem; color: var(--ink-soft); }
.garden-members { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: .45rem; margin: .6rem 0; }
.garden-members div { display: flex; flex-wrap: wrap; align-items: center; gap: .15rem .32rem; min-height: 44px; padding: .4rem .5rem; border: 1px solid var(--line); border-radius: var(--radius-sm); background: var(--surface); font-size: .86rem; }
.garden-members div::before { content: ''; width: .74rem; height: .74rem; border: 2px solid #92704d; border-radius: 50%; }
.garden-members .watered { color: var(--green); background: var(--green-soft); }
.garden-members .watered::before { border-color: var(--green); background: var(--green); box-shadow: inset 0 0 0 2px var(--green-soft); }
.garden-members dd { margin: 0; font-weight: 650; }
.garden-feedback { margin: .45rem 0; text-align: center; font-size: .9rem; color: var(--green); }
.garden-water, .garden-water:hover { width: 100%; background: var(--green); color: white; }
.garden-water--done:disabled { background: var(--green-soft); color: var(--green); opacity: 1; }
.garden-records { display: block; width: fit-content; margin: .58rem auto 0; padding: .22rem .6rem; text-underline-offset: .2em; }
.garden-progress .garden-today-heading { margin-bottom: .35rem; }
.garden-progress p { margin: 0; }
.garden-progress progress { display: block; width: 100%; margin: .3rem 0 .55rem; accent-color: var(--green); }
.garden-progress .hint { font-size: .8rem; }
.garden-recent { margin: 1rem 0 .5rem; }
.garden-recent table { width: 100%; margin-top: .55rem; border-collapse: collapse; font-size: .9rem; }
.garden-recent :is(th, td) { padding: .55rem .3rem; border-bottom: 1px solid var(--line); text-align: start; }
.garden-recent thead { background: var(--green-soft); color: var(--green); }
.garden-recent tbody th { font-variant-numeric: tabular-nums; font-weight: 500; }
.garden-recent .watered { color: var(--green); font-weight: 600; }
</style>
