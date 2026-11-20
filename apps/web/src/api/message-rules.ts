/**
 * 留言与回复的前端规则镜像（阶段 2 P2-1）。权威校验在服务端 MessageService；
 * 这里只做即时提示，并提交规范化后的值：
 *   - 留言：与任务描述相同的字符规则（CRLF 归一为 LF，其他控制字符拒绝，首尾空白与空行裁剪），1～500 个码点；
 *   - 回复：同样的字符规则，只能一行，1～100 个码点。
 */
import { normalizeTaskDescription, type TextCheck } from './task-rules'

export const MESSAGE_MAX_CODE_POINTS = 500
export const REPLY_MAX_CODE_POINTS = 100

function check(raw: string, reply: boolean): TextCheck {
  const what = reply ? '回复' : '留言'
  // Over-long input is reported against our own limit, not the task-description wording.
  const normalized = normalizeTaskDescription(raw)
  if (!normalized.ok) {
    return normalized.message.includes('最多')
      ? { ok: false, message: `${what}最多 ${reply ? REPLY_MAX_CODE_POINTS : MESSAGE_MAX_CODE_POINTS} 个字符` }
      : { ok: false, message: normalized.message.replace('描述', what) }
  }
  const value = normalized.value
  if (value.length === 0) return { ok: false, message: `${what}不能为空` }
  if (reply && value.includes('\n')) return { ok: false, message: '回复只能是一行' }
  const max = reply ? REPLY_MAX_CODE_POINTS : MESSAGE_MAX_CODE_POINTS
  if (Array.from(value).length > max) return { ok: false, message: `${what}最多 ${max} 个字符` }
  return { ok: true, value }
}

export function checkMessageBody(raw: string): TextCheck {
  return check(raw, false)
}

export function checkReplyBody(raw: string): TextCheck {
  return check(raw, true)
}

/** Beijing-time label for a message or reply time. */
export function messageTime(iso: string): string {
  const time = new Date(iso)
  if (Number.isNaN(time.getTime())) return iso
  return time.toLocaleString('zh-CN', {
    timeZone: 'Asia/Shanghai', month: 'numeric', day: 'numeric', hour: '2-digit', minute: '2-digit', hour12: false,
  })
}

/** Home preview: the first line, at most 60 characters. */
export function messagePreview(body: string): string {
  const firstLine = body.split('\n')[0] ?? ''
  const chars = Array.from(firstLine)
  return chars.length > 60 ? `${chars.slice(0, 60).join('')}…` : firstLine
}
