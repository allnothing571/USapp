import { defineStore } from 'pinia'
import { ref } from 'vue'
import { getAchievements, isAchievementSnapshot, type AchievementSnapshot } from '@/api/achievements'
import { useSessionStore } from './session'
export const useAchievementsStore = defineStore('achievements', () => {
  const data = ref<AchievementSnapshot | null>(null), status = ref<'idle' | 'loading' | 'ready' | 'error'>('idle'), error = ref('')
  let generation = 0, sequence = 0
  const owner = () => { const s = useSessionStore(); return s.isAuthenticated && s.user ? `${s.user.id}:${s.space?.id ?? ''}` : null }
  function reset() { generation++; sequence++; data.value = null; status.value = 'idle'; error.value = '' }
  async function load() {
    const who = owner(), gen = generation, seq = ++sequence
    if (!who) { reset(); return }
    data.value = null; status.value = 'loading'; error.value = ''
    try {
      const result = await getAchievements()
      if (who !== owner() || gen !== generation || seq !== sequence) return
      if (!isAchievementSnapshot(result)) throw Error('invalid')
      data.value = result; status.value = 'ready'
    } catch {
      if (who !== owner() || gen !== generation || seq !== sequence) return
      data.value = null; status.value = 'error'; error.value = '成就暂时无法读取，请重试。'
    }
  }
  return { data, status, error, reset, load }
})
