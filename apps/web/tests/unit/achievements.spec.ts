import { beforeEach, afterEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { mount, flushPromises } from '@vue/test-utils'
import { achievementRules, isAchievementSnapshot, getAchievements, type AchievementSnapshot } from '@/api/achievements'
import { useAchievementsStore } from '@/stores/achievements'
import { useSessionStore } from '@/stores/session'
import View from '@/views/AchievementsView.vue'
vi.mock('@/api/achievements', async original => ({ ...(await original<typeof import('@/api/achievements')>()), getAchievements: vi.fn() }))
const get = vi.mocked(getAchievements)
const at = '2026-11-10T00:00:00.000000Z'
function snapshot(total = 0, longest = 0, enabled = true): AchievementSnapshot {
  return { enabled, enabled_at: enabled ? at : null, timezone: 'Asia/Shanghai', total_completed: total, longest_streak: longest,
    badges: achievementRules.map(r => ({ code: r.code, metric: r.metric, target: r.target, progress: Math.min(r.target, r.metric === 'total' ? total : longest), earned_at: (r.metric === 'total' ? total : longest) >= r.target ? at : null })) }
}
const deferred = () => { let resolve!: (v: unknown) => void; const promise = new Promise(r => { resolve = r }); return { promise, resolve } }
function signIn(n = 1) { const s = useSessionStore(); s.status = 'authenticated'; s.user = { id: `10000000-0000-4000-8000-${String(n).padStart(12, '0')}`, email: 'synthetic@example.test', display_name: '我', created_at: at }; return s }
beforeEach(() => { setActivePinia(createPinia()); vi.clearAllMocks(); signIn(); get.mockResolvedValue(snapshot()) })
afterEach(() => { vi.unstubAllGlobals() })
describe('five badge contract', () => {
  it('accepts disabled and enabled snapshots, five fixed rules and completed thresholds', () => {
    expect(isAchievementSnapshot(snapshot(0, 0, false))).toBe(true); expect(isAchievementSnapshot(snapshot(50, 7))).toBe(true)
  })
  it.each([
    (s: AchievementSnapshot) => { s.badges.reverse() },
    (s: AchievementSnapshot) => { s.badges.pop() },
    (s: AchievementSnapshot) => { s.badges[0]!.target = 9 },
    (s: AchievementSnapshot) => { s.badges[0]!.metric = 'streak' },
    (s: AchievementSnapshot) => { s.badges[1]!.progress = 11 },
    (s: AchievementSnapshot) => { s.badges[1]!.earned_at = null },
    (s: AchievementSnapshot) => { s.badges[3]!.earned_at = '2020-01-01T00:00:00Z' },
    (s: AchievementSnapshot) => { s.longest_streak = 100 },
    (s: AchievementSnapshot) => { s.enabled = false; s.enabled_at = null },
  ])('refuses inconsistent, wrong-rule or retroactive facts', mutate => { const s = snapshot(10, 3); mutate(s); expect(isAchievementSnapshot(s)).toBe(false) })
  it('refuses an earned timestamp before the threshold', () => { const s = snapshot(); s.badges[0]!.earned_at = at; expect(isAchievementSnapshot(s)).toBe(false) })
})
describe('private read lifecycle', () => {
  it('newest read wins', async () => {
    const d = deferred(); get.mockReturnValueOnce(d.promise); const s = useAchievementsStore(); const old = s.load(); get.mockResolvedValue(snapshot(10, 3)); await s.load(); d.resolve(snapshot()); await old; expect(s.data?.total_completed).toBe(10)
  })
  it('ignores a late answer after account switch', async () => {
    const d = deferred(); get.mockReturnValueOnce(d.promise); const s = useAchievementsStore(); const old = s.load(); signIn(2); await s.load(); d.resolve(snapshot(50, 7)); await old; expect(s.data?.total_completed).toBe(0)
  })
  it('session reset clears data immediately and invalidates late answers', async () => {
    const s = useAchievementsStore(); await s.load(); const d = deferred(); get.mockReturnValueOnce(d.promise); const old = s.load(); useSessionStore().resetBusinessState(); expect(s.data).toBeNull(); d.resolve(snapshot(50, 7)); await old; expect(s.status).toBe('idle')
  })
  it('a failure is an error, not zero progress; retry is explicit', async () => {
    const s = useAchievementsStore(); get.mockRejectedValueOnce(new Error('offline')); await s.load(); expect(s.status).toBe('error'); expect(s.data).toBeNull(); expect(get).toHaveBeenCalledTimes(1); await s.load(); expect(s.status).toBe('ready')
  })
  it('malformed responses are not displayed', async () => { get.mockResolvedValue({ badges: [] }); const s = useAchievementsStore(); await s.load(); expect(s.status).toBe('error') })
  it('does not fetch anonymously and never persists badge state', async () => {
    const spy = vi.spyOn(Storage.prototype, 'setItem'); const s = useAchievementsStore(); await s.load(); expect(spy).not.toHaveBeenCalled(); useSessionStore().resetBusinessState(); const n = get.mock.calls.length; await s.load(); expect(get).toHaveBeenCalledTimes(n)
  })
})
describe('achievement page', () => {
  it('shows disabled activation without suggesting historical credit', async () => {
    get.mockResolvedValue(snapshot(0, 0, false)); const w = mount(View, { global: { stubs: { RouterLink: true } } }); await flushPromises(); expect(w.text()).toContain('尚未启用'); expect(w.text()).toContain('不会计入'); expect(w.findAll('progress')).toHaveLength(5); w.unmount()
  })
  it('shows earned and still-growing badges, no spending or claim controls', async () => {
    get.mockResolvedValue(snapshot(10, 3)); const w = mount(View, { global: { stubs: { RouterLink: true } } }); await flushPromises(); expect(w.text()).toContain('不发放阳光'); expect(w.get('[data-test=task_10]').text()).toContain('已获得'); expect(w.get('[data-test=task_50]').text()).toContain('还在积累'); expect(w.findAll('button')).toHaveLength(1); expect(w.find('img').exists()).toBe(false); w.unmount()
  })
  it('unmount discards private data and pending reads', async () => { const w = mount(View, { global: { stubs: { RouterLink: true } } }); await flushPromises(); w.unmount(); expect(useAchievementsStore().data).toBeNull() })
})
