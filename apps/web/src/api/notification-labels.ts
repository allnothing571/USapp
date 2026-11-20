/**
 * Fixed notification labels and destinations, shared by the inbox and the service worker. No imports:
 * this file is bundled into the service worker. Fixed labels only, never message, reply or note text.
 */
const labels: Record<string, string> = {
  'security.wallet_adjusted': '管理员已完成你的资产纠错',
  'security.redemption_cancelled': '管理员已取消你的兑换，已扣款部分已退回',
  'security.session_created': '你的账号有新的登录会话',
  'redemption.created': '对方兑换了你的奖励', 'redemption.requested': '收到新的奖励兑换申请',
  'redemption.confirm': '兑换申请已确认', 'redemption.reject': '兑换申请已拒绝',
  'redemption.withdraw': '兑换申请已撤回', 'redemption.cancel_propose': '收到兑换取消申请',
  'redemption.cancel_approve': '双方已同意取消兑换', 'redemption.cancel_reject': '兑换取消申请已拒绝',
  'redemption.cancel_withdraw': '兑换取消申请已撤回', 'redemption.fulfilled': '奖励已兑现',
  'task.change_proposed': '收到任务修改或取消申请', 'task.change_approved': '任务变更申请已同意',
  'task.change_rejected': '任务变更申请已拒绝', 'task.change_withdrawn': '任务变更申请已撤回',
  'anniversary.edit_proposed': '收到纪念日修改建议', 'anniversary.edit_approved': '纪念日修改建议已同意',
  'anniversary.edit_rejected': '纪念日修改建议已拒绝', 'anniversary.edit_withdrawn': '纪念日修改建议已撤回',
  'task.published': '对方给你发布了新任务', 'anniversary.created': '对方添加了新纪念日',
  'anniversary.edited': '对方修改了纪念日', 'message.created': '对方给你写了一张留言',
  'message.replied': '对方回复了你的留言',
  'reminder.task': '有一个任务快到截止时间了', 'reminder.anniversary': '有一个重要的日子快到了',
  // D20: generic, never a title, description, photo or name.
  'memory.created': '对方记录了新的回忆', 'shared_plan.created': '对方添加了新的共同计划',
  'album.photos_added': '对方往相册里添加了照片',
}

/** Every target kind a notice can name; the inbox and the service worker share this closed set. */
export type NotificationTargetKind = 'task' | 'reward' | 'anniversary' | 'message' | 'memory' | 'shared_plan' | 'album' | 'account'

/** The fixed label, or null for an event type this table does not know. */
export function eventLabel(eventType: string): string | null {
  return Object.hasOwn(labels, eventType) ? labels[eventType] : null
}

/** A page path only: opening it goes through that page's own current authorization (a deleted or unavailable item is not found). */
export function targetPath(kind: NotificationTargetKind, id: string): string {
  if (kind === 'account') return '/notifications'
  if (kind === 'task') return '/tasks'
  if (kind === 'reward') return '/rewards'
  if (kind === 'message') return `/us/messages/${encodeURIComponent(id)}`
  if (kind === 'memory') return `/us/memories/${encodeURIComponent(id)}`
  if (kind === 'shared_plan') return `/us/plans/${encodeURIComponent(id)}`
  if (kind === 'album') return `/us/albums/${encodeURIComponent(id)}`
  return `/us/anniversaries/${encodeURIComponent(id)}`
}
