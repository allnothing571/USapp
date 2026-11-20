import { beforeEach, describe, expect, it, vi } from 'vitest';
import { createPinia, setActivePinia } from 'pinia';
import { flushPromises, mount } from '@vue/test-utils';
const api = vi.hoisted(() => ({ read: vi.fn() }));
vi.mock('../../src/api/system', async original => ({ ...await original<typeof import('../../src/api/system')>(), systemApi: api }));
vi.mock('../../src/api/admin', () => ({ adminApi: { health: vi.fn().mockResolvedValue({ status: 'ok', version: 'test', time: '2026-11-15T01:00:00Z' }) } }));
import { isSystemStatus } from '../../src/api/system';
import { AdminApiError } from '../../src/api/client';
import { useAdminSessionStore } from '../../src/stores/admin-session';
import { useSystemStatusStore } from '../../src/stores/system-status';
import View from '../../src/views/OverviewView.vue';
const value = () => ({ read_at: '2026-11-15T01:00:00Z', database: { migration_count: 54, latest_migration: '0053_memory_decision_timestamp' }, process: { scheduler_configured: true, reminders_configured: false, memory_purge_configured: false, uploads_enabled: true, mail_mode: 'log', registration_mode: 'open' } });
function login(id = 'admin-a') { const s = useAdminSessionStore(); s.$patch({ authenticated: true, admin: { id, username: id }, epoch: s.epoch + 1 }); }
function deferred() { let resolve!: (v: unknown) => void, reject!: (v: unknown) => void; const promise = new Promise((a, b) => { resolve = a; reject = b; }); return { resolve, reject, promise }; }
beforeEach(() => { setActivePinia(createPinia()); vi.clearAllMocks(); login(); api.read.mockResolvedValue(value()); });
describe('read-only operational snapshot', () => {
  it('validates explicit states and migration consistency', () => {
    expect(isSystemStatus(value())).toBe(true);
    const v = value(); v.database.migration_count = 0; expect(isSystemStatus(v)).toBe(false);
    expect(isSystemStatus({ ...v, database: { migration_count: 0, latest_migration: null } })).toBe(true);
    expect(isSystemStatus({ ...v, read_at: 'invalid' })).toBe(false);
    expect(isSystemStatus({ ...v, database: { migration_count: -1, latest_migration: null } })).toBe(false);
    expect(isSystemStatus({ ...v, process: { ...v.process, scheduler_configured: 'true' } })).toBe(false);
    expect(isSystemStatus({ ...value(), process: { ...v.process, mail_mode: 'unknown' } })).toBe(false);
    expect(isSystemStatus({ ...value(), password: 'unexpected' })).toBe(false);
    expect(isSystemStatus({ ...value(), process: { ...v.process, host: 'unexpected' } })).toBe(false);
  });
  it('clears an old snapshot while refreshing and does not retain malformed data', async () => {
    const s = useSystemStatusStore(); await s.load(); expect(s.snapshot).not.toBeNull();
    const d = deferred(); api.read.mockReturnValueOnce(d.promise); const p = s.load();
    expect(s.snapshot).toBeNull(); expect(s.loading).toBe(true); d.resolve({}); await p;
    expect(s.snapshot).toBeNull(); expect(s.loading).toBe(false); expect(s.error).toContain('读取失败');
  });
  it('drops an answer after logout and after same-admin reauthentication', async () => {
    const s = useSystemStatusStore(), d = deferred(); api.read.mockReturnValueOnce(d.promise); const p = s.load();
    useAdminSessionStore().clear(); login(); d.resolve(value()); await p;
    expect(s.snapshot).toBeNull(); expect(s.loading).toBe(false);
  });
  it('latest read wins and a late failure cannot replace it', async () => {
    const s = useSystemStatusStore(), d = deferred(); api.read.mockReturnValueOnce(d.promise); const p = s.load();
    await s.load(); d.reject(new Error('late')); await p; expect(s.snapshot).toEqual(value()); expect(s.error).toBe('');
  });
  it.each([401, 403])('clears data and requires explicit retry after %s', async status => {
    const s = useSystemStatusStore(); await s.load(); api.read.mockRejectedValueOnce(new AdminApiError(status, 'DENIED', 'private diagnostic'));
    await s.load(); expect(s.snapshot).toBeNull(); expect(s.error).toContain('重新登录'); expect(s.error).not.toContain('private diagnostic');
    expect(api.read).toHaveBeenCalledTimes(2); await s.load(); expect(s.snapshot).toEqual(value());
  });
  it('does not request without an authenticated admin', async () => {
    useAdminSessionStore().clear(); const s = useSystemStatusStore(); await s.load(); expect(api.read).not.toHaveBeenCalled();
  });
  it('reset invalidates pending work', async () => {
    const d = deferred(), s = useSystemStatusStore(); api.read.mockReturnValueOnce(d.promise); const p = s.load();
    s.reset(); d.resolve(value()); await p; expect(s.snapshot).toBeNull();
  });
  it('renders observed records, truthful scope and working finance link; unmount invalidates reads', async () => {
    const wrapper = mount(View, { global: { stubs: { RouterLink: { template: '<a><slot /></a>' } } } });
    await flushPromises(); expect(wrapper.text()).toContain('0053_memory_decision_timestamp');
    expect(wrapper.text()).toContain('配置状态不代表工作进程正在执行');
    expect(wrapper.text()).toContain('日志模式（不发信）'); expect(wrapper.text()).toContain('选择用户并查看／纠错资产');
    expect(wrapper.text()).not.toContain('契约未定义'); wrapper.unmount(); expect(useSystemStatusStore().snapshot).toBeNull();
  });
});
