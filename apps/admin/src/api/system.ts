import { adminHttp } from './admin';

export interface SystemStatus {
  read_at: string;
  database: { migration_count: number; latest_migration: string | null };
  process: {
    scheduler_configured: boolean; reminders_configured: boolean; memory_purge_configured: boolean;
    uploads_enabled: boolean; mail_mode: 'log' | 'smtp'; registration_mode: 'whitelist' | 'open';
  };
}
const object = (v: unknown): v is Record<string, unknown> => !!v && typeof v === 'object' && !Array.isArray(v);
const exact = (v: Record<string, unknown>, keys: string[]) => Object.keys(v).length === keys.length && keys.every(k => Object.hasOwn(v, k));
export function isSystemStatus(v: unknown): v is SystemStatus {
  if (!object(v) || typeof v.read_at !== 'string' || !Number.isFinite(Date.parse(v.read_at)) || !object(v.database) || !object(v.process)) return false;
  const d = v.database, p = v.process;
  return exact(v, ['read_at', 'database', 'process']) && exact(d, ['migration_count', 'latest_migration']) &&
    exact(p, ['scheduler_configured', 'reminders_configured', 'memory_purge_configured', 'uploads_enabled', 'mail_mode', 'registration_mode']) &&
    typeof d.migration_count === 'number' && Number.isSafeInteger(d.migration_count) && d.migration_count >= 0 &&
    (d.migration_count === 0 ? d.latest_migration === null : typeof d.latest_migration === 'string' && /^\d{4}_[a-z0-9_]+$/.test(d.latest_migration)) &&
    ['scheduler_configured', 'reminders_configured', 'memory_purge_configured', 'uploads_enabled'].every(k => typeof p[k] === 'boolean') &&
    (p.mail_mode === 'log' || p.mail_mode === 'smtp') && (p.registration_mode === 'whitelist' || p.registration_mode === 'open');
}
export const systemApi = { read: () => adminHttp.request<unknown>('/system/status') };
