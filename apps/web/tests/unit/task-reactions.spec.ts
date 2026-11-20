import { createPinia, setActivePinia } from 'pinia'
import { beforeEach, expect, it, vi } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
vi.mock('@/api/tasks', () => ({ reactToTask: vi.fn(), newIdempotencyKey: vi.fn(() => 'reaction-key-0000001') }))
import { reactToTask } from '@/api/tasks'
import { ApiError, setCsrfToken } from '@/api/client'
import type { Task, TaskReaction, TaskReactionResponse } from '@/api/types'
import { reactionTarget, useTaskReactionsStore } from '@/stores/task-reactions'
import { useSessionStore } from '@/stores/session'
import { validReactionText } from '@/api/reaction-rules'
import TaskReactionPanel from '@/components/TaskReactionPanel.vue'
function login(id = 'creator') {
 useSessionStore().applyBootstrap({ authenticated: true, csrf_token: 'csrf', user: { id, email: `${id}@example.com`, display_name: id, created_at: '2026-10-30T00:00:00Z' }, space: { id: 'space', name: '', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-10-30T00:00:00Z', members: [] } }); setCsrfToken('csrf')
}
const task = { id: 'task', creator_id: 'creator', assignee_id: 'partner', title: 'x', status: 'completed', period_date: '2026-10-30' } as Task
function answer(overrides: Partial<TaskReaction> = {}): TaskReactionResponse {
 const r: TaskReaction = { id: 'reaction', task_id: 'task', period_date: '2026-10-30', author_id: 'creator', kind: 'emoji', emoji_code: 'heart', body: null, created_at: '2026-10-30T01:00:00Z', ...overrides }
 return { mutation: r, task: { ...task, reaction: { ...r } } }
}
beforeEach(() => { setActivePinia(createPinia()); vi.clearAllMocks(); login() })
it('captures period, requires confirmation and overlays stale detail', async () => {
 vi.mocked(reactToTask).mockResolvedValue(answer());const w=mount(TaskReactionPanel,{props:{task}})
 await w.findAll('button').find(b=>b.text()==='❤️')!.trigger('click');expect(reactToTask).not.toHaveBeenCalled()
 await w.find('form').trigger('submit');await flushPromises()
 expect(reactToTask).toHaveBeenCalledWith('task',{kind:'emoji',emoji_code:'heart',period_date:'2026-10-30'},'reaction-key-0000001')
 await w.setProps({task:{...task,reaction:null}});expect(w.get('[data-test="reaction-fact"]').text()).toContain('❤️');expect(w.find('form').exists()).toBe(false);w.unmount()
})
it.each([['CSRF_TOKEN_INVALID',403],['RATE_LIMITED',429],['TASK_ALREADY_REACTED',409]])('unknown followed by %s keeps original request',async(code,status)=>{
 const s=useTaskReactionsStore();vi.mocked(reactToTask).mockRejectedValueOnce(new Error('lost')).mockRejectedValueOnce(new ApiError(code as string,'error',status as number)).mockResolvedValueOnce(answer())
 await s.start(task,{kind:'emoji',emoji_code:'heart'});const target=reactionTarget(task);await s.retry(target);expect(s.actions[target].status).toBe('unknown');await s.retry(target)
 expect(s.actions[target]).toBeUndefined();expect(s.confirmed[target].id).toBe('reaction');expect(vi.mocked(reactToTask).mock.calls[2]).toEqual(vi.mocked(reactToTask).mock.calls[0])
})
it('first definitive rejection ends action',async()=>{
 vi.mocked(reactToTask).mockRejectedValue(new ApiError('TASK_ALREADY_REACTED','error',409));const s=useTaskReactionsStore();await s.start(task,{kind:'emoji',emoji_code:'heart'});expect(s.actions[reactionTarget(task)]).toBeUndefined()
})
it('malformed success remains unknown',async()=>{
 vi.mocked(reactToTask).mockResolvedValue(answer({period_date:'2026-10-29'}));const s=useTaskReactionsStore();await s.start(task,{kind:'emoji',emoji_code:'heart'});expect(s.actions[reactionTarget(task)].status).toBe('unknown')
})
it('late success is discarded on identity change',async()=>{
 let resolve!:(r:TaskReactionResponse)=>void;vi.mocked(reactToTask).mockReturnValue(new Promise(r=>{resolve=r}));const s=useTaskReactionsStore();const p=s.start(task,{kind:'emoji',emoji_code:'heart'});await flushPromises();login('other');resolve(answer());await p;expect(s.actions).toEqual({});expect(s.confirmed).toEqual({});expect(s.notices).toEqual({})
})
it('completer cannot author a reaction',()=>{login('partner');const w=mount(TaskReactionPanel,{props:{task}});expect(w.find('form').exists()).toBe(false);w.unmount()})
it('limits code points and single lines',()=>{expect(validReactionText('🌸'.repeat(40))).toBe(true);expect(validReactionText('🌸'.repeat(41))).toBe(false);expect(validReactionText('a\nb')).toBe(false)})
it('period change clears draft',async()=>{const w=mount(TaskReactionPanel,{props:{task}});await w.find('input[value="text"]').setValue();await w.get('[data-test="reaction-body"]').setValue('old');await w.setProps({task:{...task,period_date:'2026-10-31'}});expect(w.find('[data-test="reaction-body"]').exists()).toBe(false);w.unmount()})

it('an observed reaction does not hide the unresolved same-key retry', async () => {
 vi.mocked(reactToTask).mockRejectedValue(new Error('lost'))
 const store=useTaskReactionsStore();await store.start(task,{kind:'emoji',emoji_code:'heart'})
 const w=mount(TaskReactionPanel,{props:{task:{...task,reaction:answer().mutation}}})
 expect(w.get('[data-test="reaction-fact"]').text()).toContain('❤️')
 expect(w.text()).toContain('用原请求重试');w.unmount()
})

it.each([{id: 42}, {created_at: 42}])('wrong fact field types stay unresolved', async (invalid) => {
 vi.mocked(reactToTask).mockResolvedValue(answer(invalid as unknown as Partial<TaskReaction>))
 const s=useTaskReactionsStore();await s.start(task,{kind:'emoji',emoji_code:'heart'})
 expect(s.actions[reactionTarget(task)].status).toBe('unknown');expect(s.confirmed).toEqual({})
})
