<script setup lang="ts">
import { computed, onBeforeUnmount, watch } from 'vue'
import { achievementRules } from '@/api/achievements'
import { messageTime } from '@/api/message-rules'
import { useAchievementsStore } from '@/stores/achievements'
import { useSessionStore } from '@/stores/session'
import { useSmartBack } from '@/router/back-navigation'
const back = useSmartBack('achievements', 'us')
const store = useAchievementsStore(), session = useSessionStore()
watch(() => [session.user?.id, session.space?.id, session.isAuthenticated], () => { if (session.isAuthenticated) void store.load() }, { immediate: true })
onBeforeUnmount(() => store.reset())
const badges = computed(() => store.data?.badges.map((b, n) => ({ ...b, ...achievementRules[n] })) ?? [])
</script>
<template>
  <section class="page">
    <header class="subpage-header">
      <RouterLink class="back-pill" :to="back.to" data-test="achievements-back">‹ 返回{{ back.label }}</RouterLink>
      <h1 class="subpage-header__title">我的成就</h1>
    </header>
    <p class="hint">记录每一次小进步。成就仅授予徽章，不发放阳光或小红花。</p>
    <button class="btn btn--small" :disabled="store.status === 'loading'" @click="store.load()">{{ store.status === 'loading' ? '正在读取' : '刷新成就' }}</button>
    <p v-if="store.status === 'error'" class="hint hint--error" role="alert">{{ store.error }}</p>
    <template v-if="store.data">
      <p v-if="!store.data.enabled" class="card" role="status">成就统计尚未启用。启用之前的完成记录不会计入。</p>
      <div v-else class="card"><p>从 {{ messageTime(store.data.enabled_at!) }} 开始统计，本空间累计完成 {{ store.data.total_completed }} 次，历史最长连续 {{ store.data.longest_streak }} 天。</p><p class="hint">按北京时间日期计算；计次任务达到完成条件才计入一次。同一天多次完成不会增加连续天数，之后休息也不会收回已获得的徽章。</p></div>
      <ul class="list">
        <li v-for="b in badges" :key="b.code" class="card" :data-test="b.code">
          <h2 class="card__title">{{ b.earned_at ? '★' : '☆' }} {{ b.label }}</h2>
          <p>{{ b.description }}</p><p>{{ b.earned_at ? '已获得' : '还在积累' }} · {{ b.progress }} / {{ b.target }}</p>
          <progress :value="b.progress" :max="b.target" :aria-label="b.label + '进度'" style="width:100%" />
          <p v-if="b.earned_at" class="hint">获得于 {{ messageTime(b.earned_at) }}</p>
        </li>
      </ul>
    </template>
  </section>
</template>
