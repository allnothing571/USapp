import { defineStore } from 'pinia';
import { ref, watch } from 'vue';
import { AdminApiError } from '@/api/client';
import { isSystemStatus, systemApi, type SystemStatus } from '@/api/system';
import { useAdminSessionStore } from './admin-session';

export const useSystemStatusStore = defineStore('system-status', () => {
  const session = useAdminSessionStore();
  const snapshot = ref<SystemStatus | null>(null), loading = ref(false), error = ref('');
  let sequence = 0;
  const owner = () => session.authenticated && session.admin ? `${session.epoch}:${session.admin.id}` : '';
  function reset() { sequence++; snapshot.value = null; loading.value = false; error.value = ''; }
  watch(owner, reset, { flush: 'sync' });
  async function load() {
    const identity = owner(); if (!identity) { reset(); return; }
    const token = ++sequence;
    loading.value = true; snapshot.value = null; error.value = '';
    const current = () => token === sequence && identity === owner();
    try {
      const value = await systemApi.read();
      if (!current()) return;
      if (!isSystemStatus(value)) throw new Error('malformed snapshot');
      snapshot.value = value;
    } catch (e) {
      if (!current()) return;
      snapshot.value = null;
      error.value = e instanceof AdminApiError && (e.status === 401 || e.status === 403)
        ? '管理员会话已失效或无权访问，请重新登录后再读取。'
        : '运维快照读取失败，请点击重试。';
    } finally { if (current()) loading.value = false; }
  }
  return { snapshot, loading, error, load, reset };
});
