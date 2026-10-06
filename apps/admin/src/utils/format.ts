/**
 * 展示格式化工具。
 * 统一时区为 Asia/Shanghai —— 与契约中 `Space.timezone` 的固定值一致，
 * 避免运维在对比服务端日志与界面时间时还要自己换算。
 */

const dateTimeFormatter = new Intl.DateTimeFormat('zh-CN', {
  dateStyle: 'short',
  timeStyle: 'medium',
  timeZone: 'Asia/Shanghai',
  hour12: false,
});

/** 把 ISO 时间串格式化成本地运维习惯的显示；无法解析时原样显示，不假装成功。 */
export function formatDateTime(value: string | null | undefined): string {
  if (value === null || value === undefined || value === '') return '—';
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return value;
  return dateTimeFormatter.format(date);
}

/** UUID 太长会撑爆表格；列表里只显示前 8 位，完整值放在 title 上。 */
export function shortId(value: string | null | undefined): string {
  if (!value) return '—';
  return value.length > 8 ? value.slice(0, 8) : value;
}

/** 数字列的统一兜底。 */
export function formatCount(value: number | null | undefined): string {
  if (value === null || value === undefined || Number.isNaN(value)) return '—';
  return String(value);
}

/** 审计日志的 actor_type → 中文标签。 */
export function actorTypeLabel(value: string): string {
  switch (value) {
    case 'user':
      return '用户';
    case 'admin':
      return '管理员';
    case 'system':
      return '系统';
    default:
      return value;
  }
}
