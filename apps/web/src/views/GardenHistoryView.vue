<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from 'vue'
import { describeError } from '@/api/error-messages'
import { getSharedGardenMonth, isGardenMonthKey, type SharedGardenMonth } from '@/api/shared-garden'
import { useSessionStore } from '@/stores/session'
import { useSmartBack } from '@/router/back-navigation'

const back = useSmartBack('shared-garden-history', 'shared-garden')
const weekdays = ['一', '二', '三', '四', '五', '六', '日']
const session = useSessionStore()
const snapshot = ref<SharedGardenMonth | null>(null)
const monthShown = ref<string>()
const currentMonth = ref<string>()
const monthInput = ref('')
const monthInputError = ref('')
const selectedDate = ref<string>()
const busy = ref(false)
const error = ref('')
let activeRead: AbortController | null = null

const canGoBack = computed(() => !!monthShown.value && monthShown.value > '0001-01')
const canGoForward = computed(() => !!monthShown.value && !!currentMonth.value && monthShown.value < currentMonth.value)
const firstColumn = computed(() => {
  const month = snapshot.value?.month
  return month ? (new Date(`${month}-01T00:00:00Z`).getUTCDay() + 6) % 7 + 1 : 1
})
const together = computed(() => snapshot.value?.days.filter(day => day.watered && day.partner_watered).length ?? 0)
const selectedDay = computed(() => snapshot.value?.days.find(day => day.watering_date === selectedDate.value))
const today = computed(() => snapshot.value?.month === currentMonth.value
  ? snapshot.value?.days.at(-1)?.watering_date : undefined)

function cancelRead() {
  const previous = activeRead
  activeRead = null
  previous?.abort()
}

async function load(month?: string) {
  if (!session.user || !session.space) return
  cancelRead()
  snapshot.value = null
  monthShown.value = month
  monthInput.value = month ?? currentMonth.value ?? ''
  monthInputError.value = ''
  selectedDate.value = undefined
  error.value = ''
  busy.value = true
  const read = new AbortController()
  activeRead = read
  const deadline = window.setTimeout(() => {
    cancelRead()
    busy.value = false
    error.value = '读取月历超时，请重试。'
  }, 8_000)
  read.signal.addEventListener('abort', () => window.clearTimeout(deadline), { once: true })
  try {
    const result = await getSharedGardenMonth(month, read.signal)
    if (activeRead !== read) return
    snapshot.value = result
    monthShown.value = result.month
    currentMonth.value = result.current_month
    monthInput.value = result.month
  } catch (problem) {
    if (activeRead === read) error.value = describeError(problem)
  } finally {
    window.clearTimeout(deadline)
    if (activeRead === read) {
      activeRead = null
      busy.value = false
    }
  }
}

function goByMonth(step: -1 | 1) {
  if (busy.value || !monthShown.value || (step === -1 ? !canGoBack.value : !canGoForward.value)) return
  const year = Number(monthShown.value.slice(0, 4))
  const month = Number(monthShown.value.slice(5)) - 1
  const next = year * 12 + month + step
  void load(`${String(Math.floor(next / 12)).padStart(4, '0')}-${String(next % 12 + 1).padStart(2, '0')}`)
}

function jumpToMonth() {
  if (busy.value || !currentMonth.value) return
  const month = monthInput.value.trim()
  if (!isGardenMonthKey(month)) {
    monthInputError.value = '请输入有效月份，格式为 YYYY-MM。'
  } else if (month > currentMonth.value) {
    monthInputError.value = '请选择当前月份或更早的月份。'
  } else if (month !== snapshot.value?.month) {
    void load(month === currentMonth.value ? undefined : month)
  }
}

watch([() => session.user?.id, () => session.space?.id], ([user, space]) => {
  cancelRead()
  snapshot.value = null
  monthShown.value = undefined
  currentMonth.value = undefined
  monthInput.value = ''
  monthInputError.value = ''
  selectedDate.value = undefined
  error.value = ''
  busy.value = false
  if (user && space) void load()
}, { immediate: true, flush: 'sync' })

onBeforeUnmount(cancelRead)
</script>

<template>
  <main class="page garden-month">
    <header class="subpage-header">
      <RouterLink class="back-pill" :to="back.to" data-test="garden-history-back">‹ 返回{{ back.label }}</RouterLink>
      <h1 class="subpage-header__title">浇水月历</h1>
    </header>
    <section class="card" aria-label="按月查看浇水记录" :aria-busy="busy">
      <div class="month-heading">
        <button type="button" class="btn btn--small btn--soft" :disabled="busy || !canGoBack" @click="goByMonth(-1)">上个月</button>
        <h2 class="card__title">{{ monthShown || '本月' }}</h2>
        <button type="button" class="btn btn--small btn--soft" :disabled="busy || !canGoForward" @click="goByMonth(1)">下个月</button>
      </div>
      <div class="month-actions">
        <button v-if="canGoForward" type="button" class="btn btn--small btn--ghost" :disabled="busy" @click="load()">回到本月</button>
        <button type="button" class="btn btn--small btn--ghost" :disabled="busy"
          @click="load(monthShown === currentMonth ? undefined : monthShown)">{{ error ? '重试' : '刷新' }}</button>
      </div>
      <form v-if="currentMonth" class="month-jump" novalidate @submit.prevent="jumpToMonth">
        <label for="month-jump-input">指定月份</label>
        <input id="month-jump-input" v-model="monthInput" type="month" min="0001-01" :max="currentMonth"
          placeholder="YYYY-MM" :disabled="busy" :aria-invalid="!!monthInputError"
          :aria-describedby="monthInputError ? 'month-jump-error' : undefined" @input="monthInputError = ''" />
        <button type="submit" class="btn btn--small btn--soft" :disabled="busy">查看</button>
      </form>
      <p v-if="monthInputError" id="month-jump-error" class="hint hint--error" role="alert">{{ monthInputError }}</p>
      <p v-if="busy" class="card__hint" role="status">正在读取月历…</p>
      <p v-if="error" class="hint hint--error" role="alert">{{ error }}</p>
      <template v-if="snapshot">
        <p class="month-summary">两人都浇水 <strong>{{ together }} 天</strong> · 北京时间</p>
        <div class="month-grid month-weekdays" aria-hidden="true">
          <span v-for="weekday in weekdays" :key="weekday">{{ weekday }}</span>
        </div>
        <ol class="month-grid month-days" aria-label="每天的浇水状态">
          <li v-for="(day, index) in snapshot.days" :key="day.watering_date"
            :style="index === 0 ? { gridColumnStart: firstColumn } : undefined"
            :class="{ together: day.watered && day.partner_watered }">
            <button type="button" class="month-day" :aria-pressed="selectedDate === day.watering_date"
              :aria-current="today === day.watering_date ? 'date' : undefined"
              :aria-label="`${day.watering_date}，我${day.watered ? '已' : '未'}浇水，对方${day.partner_watered ? '已' : '未'}浇水`"
              @click="selectedDate = day.watering_date">
              <time :datetime="day.watering_date">{{ Number(day.watering_date.slice(8)) }}</time>
              <span class="month-marks" aria-hidden="true">
                <span :class="{ mine: day.watered }">我</span>
                <span :class="{ partner: day.partner_watered }">伴</span>
              </span>
            </button>
          </li>
        </ol>
        <p class="card__hint">深色标记表示当天浇过水。</p>
        <div aria-live="polite">
          <div v-if="selectedDay" class="month-day-detail">
            <h3><time :datetime="selectedDay.watering_date">{{ selectedDay.watering_date }}</time></h3>
            <p>我{{ selectedDay.watered ? '已' : '未' }}浇水 · 对方{{ selectedDay.partner_watered ? '已' : '未' }}浇水</p>
          </div>
        </div>
      </template>
    </section>
  </main>
</template>

<style scoped>
.garden-month { max-width: 46rem; margin-inline: auto; }
.month-heading, .month-actions { display: flex; justify-content: space-between; align-items: center; gap: .45rem; flex-wrap: wrap; }
.month-heading .card__title { margin: 0; font-variant-numeric: tabular-nums; }
.month-actions { justify-content: end; margin-top: .5rem; }
.month-jump { display: flex; flex-wrap: wrap; align-items: center; gap: .45rem; margin-top: .55rem; }
.month-jump input { min-width: 0; max-width: 100%; font: inherit; padding: .32rem .45rem; }
.month-summary { margin: .65rem 0; }
.month-grid { display: grid; grid-template-columns: repeat(7, minmax(0, 1fr)); gap: .22rem; }
.month-weekdays { text-align: center; color: #4e6350; padding: .28rem 0; }
.month-days { padding: 0; margin: 0; list-style: none; }
.month-days li { min-width: 0; border: 1px solid #ded6c9; border-radius: .35rem; }
.month-days li.together { background: #e9eddb; border-color: #c0c9ad; }
.month-day { display: grid; align-content: center; justify-items: center; width: 100%; min-height: 3.05rem; padding: 0; border: 0; border-radius: inherit; background: transparent; color: inherit; cursor: pointer; font: inherit; font-variant-numeric: tabular-nums; }
.month-day[aria-current="date"] time { display: inline-flex; align-items: center; justify-content: center; padding: 0 .35rem; border-radius: 999px; background: var(--honey); font-weight: 700; }
.month-day[aria-pressed="true"], .month-day:focus-visible { outline: 2px solid #416349; outline-offset: -1px; }
.month-marks { display: flex; justify-content: center; flex-wrap: wrap; gap: .12rem; font-size: .68rem; }
.month-marks span { color: #635f55; border-radius: .2rem; padding: 0 .09rem; }
.month-marks .mine { background: #416349; color: white; }
.month-marks .partner { background: #77523e; color: white; }
.month-day-detail { padding: .65rem .75rem; border-inline-start: .2rem solid #729570; background: #e9eddb; }
.month-day-detail :is(h3, p) { margin: 0; }
.month-day-detail h3 { margin-bottom: .25rem; font-size: .95rem; }
</style>
