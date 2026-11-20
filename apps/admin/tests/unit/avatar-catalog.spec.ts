import { beforeEach, describe, it, expect, vi } from 'vitest';
import { createPinia, setActivePinia } from 'pinia';
import { mount, flushPromises } from '@vue/test-utils';
import { nextTick } from 'vue';
import { asset, item, id, starter, date } from './avatar-fixtures';
import { isAsset, isItem, isStarter, validReceipt, type Operation } from '../../src/api/avatar';
import { AdminApiError } from '../../src/api/client';
import { useAvatarCatalogStore } from '../../src/stores/avatar-catalog';
import { useAdminSessionStore } from '../../src/stores/admin-session';
import View from '../../src/views/AvatarCatalogView.vue';
const api = vi.hoisted(() => ({ assets: vi.fn(), items: vi.fn(), starter: vi.fn(), detail: vi.fn(), write: vi.fn() }));
vi.mock('../../src/api/avatar', async original => ({ ...await original<typeof import('../../src/api/avatar')>(), avatarAdminApi: api }));
const create: Operation = { kind: 'create', body: { name: item.name, asset_key: asset.key, category: 'hair', starter: false, price_sunshine: 20, listed: true } };
const revise: Operation = { kind: 'revise', target: item.id, base: item, body: { expected_version: 1, name: '新名称', asset_key: asset.key, price_sunshine: 30, listed: false } };
const receipt = () => ({ replayed: false, revision_version: 1, item: structuredClone(item) });
const deferred = <T>() => { let resolve!: (v: T) => void; const promise = new Promise<T>(r => { resolve = r; }); return { promise, resolve }; };
beforeEach(() => {
  setActivePinia(createPinia()); vi.clearAllMocks();
  const s = useAdminSessionStore(); s.authenticated = true; s.admin = { id: id(99), username: 'test-admin' };
  api.assets.mockResolvedValue({ assets: [structuredClone(asset)] });
  api.items.mockResolvedValue({ items: [structuredClone(item)] }); api.starter.mockResolvedValue({ starter: null });
  api.write.mockResolvedValue(receipt());
});
describe('admin catalog response validation', () => {
  it('validates registry layers, hashes and fixed category metadata', () => {
    expect(isAsset(asset)).toBe(true); expect(isAsset({ ...asset, layers: { main: 'a'.repeat(64) } })).toBe(false);
    expect(isAsset({ ...asset, key: 'https://example.test/image' })).toBe(false);
    expect(isAsset({ ...asset, covers_hair_top: true })).toBe(false);
  });
  it('validates items and starter configuration', () => {
    expect(isItem(item)).toBe(true); expect(isItem({ ...item, starter: true })).toBe(false);
    expect(isItem({ ...item, owner_count: -1 })).toBe(false); expect(isItem({ ...item, asset: { ...item.asset, key: 'hair.other.v1' } })).toBe(false);
    expect(isStarter(starter)).toBe(true); expect(isStarter({ ...starter, clothes: null })).toBe(false);
  });
  it('requires captured content at the original create version', () => {
    expect(validReceipt(receipt(), create)).toBe(true);
    expect(validReceipt({ ...receipt(), item: { ...item, name: '错了' } }, create)).toBe(false);
    expect(validReceipt({ ...receipt(), revision_version: 2 }, create)).toBe(false);
  });
  it('accepts a newer create replay, without claiming its current terms were original', () => {
    expect(validReceipt({ replayed: true, revision_version: 1, item: { ...item, version: 3, name: '后来改名' } }, create)).toBe(true);
    expect(validReceipt({ ...receipt(), item: { ...item, version: 3, category: 'clothes' } }, create)).toBe(false);
  });
  it('rejects an older item and unrelated target on revision replay', () => {
    expect(validReceipt({ replayed: true, revision_version: 2, item }, revise)).toBe(false);
    expect(validReceipt({ replayed: true, revision_version: 2, item: { ...item, id: id(20), version: 3 } }, revise)).toBe(false);
    expect(validReceipt({ replayed: true, revision_version: 2, item: { ...item, version: 3 } }, revise)).toBe(true);
    expect(validReceipt({ replayed: false, revision_version: 2, item: { ...item, version: 2, ...revise.body } }, revise)).toBe(true);
  });
  it('checks the original starter version and every captured id', () => {
    const op: Operation = { kind: 'starter', body: { expected_version: 0, hair: starter.hair, clothes: starter.clothes, expression: starter.expression } };
    expect(validReceipt({ replayed: true, starter }, op)).toBe(true);
    expect(validReceipt({ replayed: true, starter: { ...starter, version: 2 } }, op)).toBe(false);
    expect(validReceipt({ replayed: true, starter: { ...starter, hair: id(40) } }, op)).toBe(false);
  });
});
describe('admin catalog state and captured operations', () => {
  it('loads all three read surfaces and exposes a truly empty catalog', async () => {
    api.assets.mockResolvedValue({ assets: [] }); api.items.mockResolvedValue({ items: [] });
    const s = useAvatarCatalogStore(); await s.load(); expect(s.loaded).toBe(true); expect(s.items).toEqual([]); expect(s.starter).toBeNull();
  });
  it('rejects malformed or duplicated reads atomically', async () => {
    api.items.mockResolvedValue({ items: [item, item] });
    const s = useAvatarCatalogStore(); await s.load(); expect(s.loaded).toBe(false); expect(s.assets).toEqual([]); expect(s.error).toBeTruthy();
  });
  it('drops older read results', async () => {
    const d = deferred<unknown>(); api.items.mockReturnValueOnce(d.promise);
    const s = useAvatarCatalogStore(); const old = s.load(); api.items.mockResolvedValue({ items: [] }); await s.load();
    d.resolve({ items: [item] }); await old; expect(s.items).toEqual([]);
  });
  it('drops a read after logout and clears private state synchronously', async () => {
    const s = useAvatarCatalogStore(); await s.load(); const d = deferred<unknown>(); api.items.mockReturnValueOnce(d.promise); const p = s.load();
    useAdminSessionStore().clear(); expect(s.items).toEqual([]); d.resolve({ items: [item] }); await p; expect(s.loaded).toBe(false);
  });
  it('requires confirmation, serializes writes and clones the captured body', async () => {
    const s = useAvatarCatalogStore(); await s.load(); const op = structuredClone(create); s.stage(op); expect(api.write).not.toHaveBeenCalled();
    op.body.name = 'changed outside'; s.stage(revise); expect(s.action!.operation.kind).toBe('create');
    const d = deferred<unknown>(); api.write.mockReturnValue(d.promise); const p = s.send(); await s.send(); expect(api.write).toHaveBeenCalledTimes(1);
    expect(api.write.mock.calls[0]![0].body.name).toBe(item.name); d.resolve(receipt()); await p; expect(s.action).toBeNull(); expect(s.notice).toContain('第 1 版');
  });
  it('keeps the same key and payload through unknown then 4xx then replay', async () => {
    const s = useAvatarCatalogStore(); await s.load(); s.stage(create);
    api.write.mockRejectedValueOnce(new AdminApiError(503, 'RESULT_UNKNOWN', 'unknown')).mockRejectedValueOnce(new AdminApiError(409, 'AVATAR_VERSION_CONFLICT', 'conflict')).mockResolvedValueOnce({ ...receipt(), replayed: true, item: { ...item, version: 4, name: '今日名称' } });
    await s.send(); expect(s.action!.phase).toBe('unknown'); const key = s.action!.key; s.dismiss(); expect(s.action).not.toBeNull();
    await s.send(); expect(s.action!.phase).toBe('unknown'); expect(s.action!.key).toBe(key);
    await s.send(); expect(s.action).toBeNull(); expect(api.write.mock.calls.map(c => c[1])).toEqual([key, key, key]); expect(api.write.mock.calls[2]![0]).toEqual(create); expect(s.notice).toContain('原操作');
  });
  it.each([new Error('network'), new DOMException('abort', 'AbortError')])('preserves uncertain sends for transport errors', async e => {
    const s = useAvatarCatalogStore(); await s.load(); s.stage(create); api.write.mockRejectedValueOnce(e); await s.send(); expect(s.action!.phase).toBe('unknown');
  });
  it('treats a malformed success as uncertain', async () => {
    const s = useAvatarCatalogStore(); await s.load(); s.stage(create); api.write.mockResolvedValue({ ...receipt(), revision_version: 9 }); await s.send(); expect(s.action!.phase).toBe('unknown');
  });
  it('first definite rejection needs explicit dismissal and fresh confirmation', async () => {
    const s = useAvatarCatalogStore(); await s.load(); s.stage(revise); const key = s.action!.key;
    api.write.mockRejectedValueOnce(new AdminApiError(409, 'AVATAR_VERSION_CONFLICT', 'conflict')); await s.send(); expect(s.action!.phase).toBe('rejected'); await s.send(); expect(api.write).toHaveBeenCalledTimes(1);
    s.dismiss(); s.stage(revise); expect(s.action!.key).not.toBe(key); expect(s.action!.phase).toBe('confirm');
  });
  it('same-admin new session cannot receive an old write result', async () => {
    const s = useAvatarCatalogStore(); await s.load(); s.stage(create); const d = deferred<unknown>(); api.write.mockReturnValue(d.promise); const p = s.send();
    useAdminSessionStore().epoch++; expect(s.action).toBeNull(); d.resolve(receipt()); await p; expect(s.notice).toBe(''); expect(s.items).toEqual([]);
  });
  it('an old read cannot overwrite post-write refresh', async () => {
    const s = useAvatarCatalogStore(); await s.load(); s.stage(create);
    const d = deferred<unknown>(); api.items.mockReturnValueOnce(d.promise); const p = s.load();
    api.items.mockResolvedValue({ items: [{ ...item, version: 2 }] }); await s.send();
    d.resolve({ items: [item] }); await p; expect(s.items[0]!.version).toBe(2);
  });
  it('loads only matching item history and clears it on identity change', async () => {
    api.detail.mockResolvedValue({ item, revisions: [{ version: 1, name: item.name, asset_key: asset.key, price_sunshine: 20, listed: true, admin_user_id: id(99), created_at: date }] });
    const s = useAvatarCatalogStore(); await s.openDetail(item.id); expect(s.detail?.revisions).toHaveLength(1);
    useAdminSessionStore().clear(); expect(s.detail).toBeNull();
  });
  it('never persists the catalog or captured operation', async () => {
    const spy = vi.spyOn(Storage.prototype, 'setItem'); const s = useAvatarCatalogStore(); await s.load(); s.stage(create); await s.send(); expect(spy).not.toHaveBeenCalled();
  });
});
describe('admin catalog form', () => {
  const button = (w: ReturnType<typeof mount>, text: string) => w.findAll('button').find(b => b.text() === text)!;
  it('shows the empty registry without inventing assets or writes', async () => {
    api.assets.mockResolvedValue({ assets: [] }); api.items.mockResolvedValue({ items: [] });
    const w = mount(View); await flushPromises(); expect(w.text()).toContain('暂无商品'); await button(w, '新建商品').trigger('click'); expect(w.text()).toContain('没有符合条件的素材'); expect(api.write).not.toHaveBeenCalled(); w.unmount();
  });
  it('captures a new product only after explicit confirmation, locks the form', async () => {
    const w = mount(View); await flushPromises(); await button(w, '新建商品').trigger('click');
    await w.get('input[required]').setValue('新头发'); await w.get('select[required]').setValue(asset.key);
    await w.get('form').trigger('submit'); expect(api.write).not.toHaveBeenCalled(); expect(w.get('fieldset').attributes('disabled')).toBeDefined();
    await button(w, '确认提交').trigger('click'); await flushPromises(); expect(api.write).toHaveBeenCalledTimes(1); expect(api.write.mock.calls[0]![0].body.price_sunshine).toBe(20); w.unmount();
  });
  it('keeps category and starter immutable and filters correction assets', async () => {
    api.assets.mockResolvedValue({ assets: [asset, { ...asset, key: 'hair.test.v2', revision: 2, item_id: item.id }, { ...asset, key: 'hair.other.v1', family: 'hair.other' }] });
    const w = mount(View); await flushPromises(); await button(w, '修订').trigger('click');
    expect(w.get('select').attributes('disabled')).toBeDefined(); expect(w.get('input[type=checkbox]').attributes('disabled')).toBeDefined();
    const opts = w.get('select[required]').text(); expect(opts).toContain('hair.test.v2'); expect(opts).not.toContain('hair.other.v1'); w.unmount();
  });
  it('allows only starter items of the right slot and rejects incomplete defaults', async () => {
    api.items.mockResolvedValue({ items: [item, { ...item, id: id(2), starter: true, owner_count: null, price_sunshine: null }] });
    const w = mount(View); await flushPromises(); await button(w, '配置初始装扮').trigger('click');
    const opts = w.findAll('select'); expect(opts[0]!.findAll('option')).toHaveLength(2); expect(opts[1]!.findAll('option')).toHaveLength(1); expect(button(w, '核对操作').attributes('disabled')).toBeDefined(); w.unmount();
  });
  it('clears a dirty form on session invalidation', async () => {
    const w = mount(View); await flushPromises(); await button(w, '新建商品').trigger('click'); useAdminSessionStore().clear(); await nextTick(); expect(w.find('form').exists()).toBe(false); w.unmount();
  });
  it('submits a complete starter configuration, including its expected version', async () => {
    api.items.mockResolvedValue({ items: [
      { ...item, id: id(2), starter: true, owner_count: null, price_sunshine: null },
      ...(['clothes', 'expression'] as const).map((category, n) => ({ ...item, id: id(3 + n), category, family: `${category}.test`, asset: { ...item.asset, key: `${category}.test.v1`, layers: ['main'] }, starter: true, owner_count: null, price_sunshine: null })),
    ] });
    const w = mount(View); await flushPromises(); await button(w, '配置初始装扮').trigger('click');
    for (const [n, select] of w.findAll('select').entries()) await select.setValue(id(n + 2));
    await w.get('form').trigger('submit'); expect(api.write).not.toHaveBeenCalled();
    api.write.mockResolvedValue({ replayed: false, starter }); await button(w, '确认提交').trigger('click'); await flushPromises();
    expect(api.write.mock.calls[0]![0]).toEqual({ kind: 'starter', body: { expected_version: 0, hair: id(2), clothes: id(3), expression: id(4) } }); expect(w.text()).toContain('操作已确认'); w.unmount();
  });
  it('preserves a revision draft after a refresh and requires explicit reopening on a conflict', async () => {
    const w = mount(View); await flushPromises(); await button(w, '修订').trigger('click'); await w.get('input[required]').setValue('保留草稿');
    api.items.mockResolvedValue({ items: [{ ...item, version: 2, name: '别人修改' }] }); await button(w, '刷新目录').trigger('click'); await flushPromises();
    expect((w.get('input[required]').element as HTMLInputElement).value).toBe('保留草稿'); expect(w.text()).toContain('商品版本已变化'); expect(button(w, '核对操作').attributes('disabled')).toBeDefined(); expect(api.write).not.toHaveBeenCalled(); w.unmount();
  });
});
