import { describe, it, expect, vi, afterEach } from 'vitest';
import { adminHttp } from '../../src/api/admin';
import { avatarAdminApi as api, type Operation } from '../../src/api/avatar';
import { asset, item } from './avatar-fixtures';
afterEach(() => { vi.unstubAllGlobals(); adminHttp.setCsrfToken(null); });
describe('avatar admin wire boundary', () => {
  it('sends only the admin surface, same-origin credentials, CSRF, exact body and key', async () => {
    const fetch = vi.fn().mockImplementation(async () => new Response('{}', { status: 200 })); vi.stubGlobal('fetch', fetch); adminHttp.setCsrfToken('test-csrf');
    const op: Operation = { kind: 'create', body: { category: 'hair', starter: false, name: item.name, asset_key: asset.key, price_sunshine: 20, listed: false } };
    await api.write(op, 'test-captured-key-123456'); await api.write(op, 'test-captured-key-123456');
    const [path, init] = fetch.mock.calls[0]!;
    expect(path).toBe('/api/admin/v1/avatar/items'); expect(init.credentials).toBe('same-origin'); expect(init.cache).toBe('no-store'); expect(init.redirect).toBe('error');
    expect(init.headers['X-Requested-By']).toBe('usapp-admin'); expect(init.headers['X-CSRF-Token']).toBe('test-csrf'); expect(init.headers['Idempotency-Key']).toBe('test-captured-key-123456'); expect(JSON.parse(init.body)).toEqual(op.body);
    expect(fetch.mock.calls[1]).toEqual(fetch.mock.calls[0]);
  });
  it('refuses a write before network when the admin CSRF token is absent', async () => {
    const fetch = vi.fn(); vi.stubGlobal('fetch', fetch); adminHttp.setCsrfToken(null);
    await expect(api.write({ kind: 'starter', body: { expected_version: 0, hair: item.id, clothes: item.id, expression: item.id } }, 'test-captured-key-123456')).rejects.toMatchObject({ code: 'CSRF_TOKEN_MISSING' }); expect(fetch).not.toHaveBeenCalled();
  });
  it('uses the existing registry, item, history and starter paths only', async () => {
    const fetch = vi.fn().mockImplementation(async () => new Response('{}')); vi.stubGlobal('fetch', fetch);
    await api.assets(); await api.items(); await api.starter(); await api.detail(item.id);
    expect(fetch.mock.calls.map(c => c[0])).toEqual(['/api/admin/v1/avatar/assets', '/api/admin/v1/avatar/items', '/api/admin/v1/avatar/starter', `/api/admin/v1/avatar/items/${item.id}`]);
  });
});
