import { defineStore } from 'pinia';
import { ref, watch } from 'vue';
import { useAdminSessionStore } from './admin-session';
import { AdminApiError } from '@/api/client';
import { avatarAdminApi as api, object, isAsset, isItem, isStarter, isRevision, validReceipt, type Asset, type Item, type Starter, type Revision, type Operation } from '@/api/avatar';
type Action = { operation: Operation; key: string; phase: 'confirm' | 'sending' | 'unknown' | 'rejected'; message: string };
const clone = <T>(v: T): T => JSON.parse(JSON.stringify(v)) as T;
export const useAvatarCatalogStore = defineStore('avatar-catalog', () => {
  const session = useAdminSessionStore();
  const assets = ref<Asset[]>([]), items = ref<Item[]>([]), starter = ref<Starter | null>(null);
  const detail = ref<{ item: Item; revisions: Revision[] } | null>(null);
  const loading = ref(false), loaded = ref(false), error = ref(''), detailError = ref(''), notice = ref('');
  const action = ref<Action | null>(null);
  let generation = 0, sequence = 0, detailSequence = 0;
  const owner = () => session.authenticated ? `${session.epoch}:${session.admin?.id ?? ''}` : '';
  function reset() {
    generation++; sequence++; detailSequence++;
    assets.value = []; items.value = []; starter.value = null; detail.value = null;
    loading.value = loaded.value = false; error.value = detailError.value = notice.value = ''; action.value = null;
  }
  watch(owner, reset, { flush: 'sync' });
  const token = () => ({ generation, owner: owner() });
  const current = (t: ReturnType<typeof token>) => !!t.owner && t.generation === generation && t.owner === owner();
  async function load() {
    if (!owner()) return;
    const t = token(), s = ++sequence;
    loading.value = true; error.value = '';
    try {
      const [a, i, d] = await Promise.all([api.assets(), api.items(), api.starter()]);
      if (!current(t) || s !== sequence) return;
      if (!object(a) || !Array.isArray(a.assets) || !a.assets.every(isAsset) || !object(i) || !Array.isArray(i.items) || !i.items.every(isItem) ||
        !object(d) || !(d.starter === null || isStarter(d.starter))) throw Error('shape');
      if (new Set(a.assets.map(x => x.key)).size !== a.assets.length || new Set(i.items.map(x => x.id)).size !== i.items.length) throw Error('duplicates');
      assets.value = a.assets; items.value = i.items; starter.value = d.starter; loaded.value = true;
    } catch {
      if (current(t) && s === sequence) { loaded.value = false; assets.value = []; items.value = []; starter.value = null; error.value = '目录读取失败，无法开始新的修改，请重试读取。'; }
    } finally { if (current(t) && s === sequence) loading.value = false; }
  }
  async function openDetail(id: string) {
    if (!owner()) return;
    const t = token(), s = ++detailSequence;
    detail.value = null; detailError.value = '';
    try {
      const r = await api.detail(id);
      if (!current(t) || s !== detailSequence) return;
      if (!object(r) || !isItem(r.item) || r.item.id !== id || !Array.isArray(r.revisions) || !r.revisions.every(isRevision)) throw Error('shape');
      detail.value = { item: r.item, revisions: r.revisions };
    } catch { if (current(t) && s === detailSequence) detailError.value = '修订历史读取失败。'; }
  }
  function stage(operation: Operation) {
    if (!owner() || action.value || !loaded.value || loading.value) return;
    notice.value = '';
    action.value = { operation: clone(operation), key: crypto.randomUUID(), phase: 'confirm', message: '' };
  }
  function dismiss() {
    if (action.value?.phase === 'confirm' || action.value?.phase === 'rejected') action.value = null;
  }
  async function send() {
    const a = action.value;
    if (!owner() || !a || !['confirm', 'unknown'].includes(a.phase)) return;
    const t = token(); const uncertain = a.phase === 'unknown';
    a.phase = 'sending'; a.message = ''; sequence++; detailSequence++; loading.value = false;
    try {
      const r = await api.write(clone(a.operation), a.key);
      if (!current(t) || action.value !== a) return;
      if (!validReceipt(r, a.operation)) throw Error('receipt');
      const v = r as { revision_version?: number; starter?: Starter; replayed: boolean };
      notice.value = `${v.replayed ? '原操作已确认' : '操作已确认'}：${a.operation.kind === 'starter' ? '初始配置' : '商品修订'}第 ${v.revision_version ?? v.starter!.version} 版。下方将重新读取当前状态。`;
      action.value = null; detail.value = null;
      await load();
    } catch (e) {
      if (!current(t) || action.value !== a) return;
      const definite = !uncertain && e instanceof AdminApiError && (e.status === 0 || (e.status >= 400 && e.status < 500));
      a.phase = definite ? 'rejected' : 'unknown';
      a.message = definite ? `本次被拒绝（${(e as AdminApiError).code}）。刷新并重新核对后，用新操作提交。` : '结果未确认；保留原请求与标识。只可点击重试，不能更改此请求。刷新或退出登录会丢失本地重试记录。';
      // A later error cannot disprove an earlier uncertain send. Never auto-retry.
      if (definite) await load();
    }
  }
  return { assets, items, starter, detail, loading, loaded, error, detailError, notice, action, load, openDetail, stage, dismiss, send };
});
