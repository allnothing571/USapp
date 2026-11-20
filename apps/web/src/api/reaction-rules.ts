import type { TaskReaction } from './types'
export const REACTION_EMOJIS = { heart: '❤️', clap: '👏', flower: '🌸' } as const
export function reactionText(r: TaskReaction): string { return r.kind === 'emoji' ? REACTION_EMOJIS[r.emoji_code!] : r.body ?? '' }
export function validReactionText(text: string): boolean {
  return Array.from(text).length >= 1 && Array.from(text).length <= 40 && !/[\u0000-\u001f\u007f-\u009f\u2028\u2029]/u.test(text)
}
