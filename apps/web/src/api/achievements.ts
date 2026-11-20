import { request } from './client'
export const achievementRules = [
  { code: 'first_task', label: '第一步', description: '首次完成任务', metric: 'total', target: 1 },
  { code: 'task_10', label: '积少成多', description: '累计完成 10 次任务', metric: 'total', target: 10 },
  { code: 'task_50', label: '五十次小进步', description: '累计完成 50 次任务', metric: 'total', target: 50 },
  { code: 'streak_3', label: '三日同行', description: '连续 3 个北京时间日期有任务完成', metric: 'streak', target: 3 },
  { code: 'streak_7', label: '一周足迹', description: '连续 7 个北京时间日期有任务完成', metric: 'streak', target: 7 },
] as const
export interface AchievementBadge { code: typeof achievementRules[number]['code']; metric: 'total' | 'streak'; target: number; progress: number; earned_at: string | null }
export interface AchievementSnapshot { enabled: boolean; enabled_at: string | null; timezone: 'Asia/Shanghai'; total_completed: number; longest_streak: number; badges: AchievementBadge[] }
const object = (x: unknown): x is Record<string, unknown> => !!x && typeof x === 'object' && !Array.isArray(x)
const integer = (x: unknown): x is number => Number.isSafeInteger(x) && (x as number) >= 0
const timestamp = (x: unknown): x is string => typeof x === 'string' && /^\d{4}-\d\d-\d\dT.*Z$/.test(x) && Number.isFinite(Date.parse(x))
export function isAchievementSnapshot(v: unknown): v is AchievementSnapshot {
  if (!object(v) || typeof v.enabled !== 'boolean' || v.timezone !== 'Asia/Shanghai' || !integer(v.total_completed) || !integer(v.longest_streak) || v.longest_streak > v.total_completed ||
    !(v.enabled ? timestamp(v.enabled_at) : v.enabled_at === null && v.total_completed === 0 && v.longest_streak === 0) || !Array.isArray(v.badges) || v.badges.length !== 5) return false
  const badges = v.badges
  return achievementRules.every((rule, n) => {
    const b: unknown = badges[n]
    if (!object(b) || b.code !== rule.code || b.metric !== rule.metric || b.target !== rule.target || b.progress !== Math.min(rule.target, rule.metric === 'total' ? v.total_completed as number : v.longest_streak as number)) return false
    return b.progress === rule.target ? timestamp(b.earned_at) && timestamp(v.enabled_at) && Date.parse(b.earned_at) >= Date.parse(v.enabled_at) : b.earned_at === null
  })
}
export const getAchievements = () => request<unknown>('/api/v1/achievements')
