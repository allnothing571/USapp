/**
 * 昵称规则的前端镜像（阶段 1 M1）。
 *
 * 权威校验在服务端 ProfileService；这里只用于即时提示，并保证提交的是
 * 去掉首尾空白后的值。两边规则必须一致：
 *   - 首尾只裁剪 Unicode 空格分隔符（Zs），不裁换行与制表符；
 *   - 控制字符（C0 / DEL / C1）与换行（含 U+2028 / U+2029）出现在任何位置都拒绝；
 *   - 裁剪后要求 1～20 个 **Unicode 码点**。
 *
 * 计数单位：`Array.from` 按码点迭代，'😀' 计 1；而 `'😀'.length === 2`（UTF-16 码元），
 * 所以这里绝不能用 `.length` 或 `String.prototype.trim()`。
 */

export const DISPLAY_NAME_MAX_CODE_POINTS = 20

export type DisplayNameProblem = 'invalid' | 'control_character' | 'empty' | 'too_long'

export type DisplayNameCheck =
  | { ok: true; value: string }
  | { ok: false; problem: DisplayNameProblem }

/** 与服务端 ProfileService::displayNameErrorMessage 文案一致。 */
export const DISPLAY_NAME_MESSAGES: Record<DisplayNameProblem, string> = {
  invalid: '昵称包含无法识别的字符',
  control_character: '昵称不能包含换行或控制字符',
  empty: '昵称不能为空',
  too_long: `昵称最多 ${DISPLAY_NAME_MAX_CODE_POINTS} 个字符`,
}

function isTrimmableSpace(cp: number): boolean {
  return (
    cp === 0x20 ||
    cp === 0xa0 ||
    cp === 0x1680 ||
    (cp >= 0x2000 && cp <= 0x200a) ||
    cp === 0x202f ||
    cp === 0x205f ||
    cp === 0x3000
  )
}

function isForbidden(cp: number): boolean {
  return cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || cp === 0x2028 || cp === 0x2029
}

/** 按码点计数的长度（'😀' 计 1）。 */
export function codePointLength(value: string): number {
  return Array.from(value).length
}

export function checkDisplayName(raw: string): DisplayNameCheck {
  const chars = Array.from(raw)
  const cps = chars.map((ch) => ch.codePointAt(0) ?? 0)

  // 孤立的 UTF-16 代理项无法编码成合法 UTF-8，服务端同样会拒绝。
  if (cps.some((cp) => cp >= 0xd800 && cp <= 0xdfff)) {
    return { ok: false, problem: 'invalid' }
  }
  if (cps.some(isForbidden)) {
    return { ok: false, problem: 'control_character' }
  }

  let first = 0
  let last = cps.length
  while (first < last && isTrimmableSpace(cps[first])) first += 1
  while (last > first && isTrimmableSpace(cps[last - 1])) last -= 1

  if (first === last) return { ok: false, problem: 'empty' }
  if (last - first > DISPLAY_NAME_MAX_CODE_POINTS) return { ok: false, problem: 'too_long' }
  return { ok: true, value: chars.slice(first, last).join('') }
}
