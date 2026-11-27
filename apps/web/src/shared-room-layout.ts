import { renderCoords, ROOM_ITEMS, type Placement, type RoomItem } from '@/api/shared-room'

const items = Object.keys(ROOM_ITEMS) as RoomItem[]

export const FLOOR_MIN = 80
export const FLOOR_MAX = 920
export const MIN_FURNITURE_GAP = 85

export const DEFAULT_ITEM_SPOTS: Record<RoomItem, { x: number; y: number }> = {
  rug: { x: 500, y: 650 },
  table: { x: 500, y: 600 },
  plant: { x: 170, y: 300 },
  lamp: { x: 830, y: 300 },
  bookshelf: { x: 500, y: 235 },
  armchair: { x: 700, y: 560 },
  vase: { x: 500, y: 500 },
}

export const ROOM_LAYOUT_PRESETS = [
  {
    name: '茶角',
    layout: [
      { item: 'rug', x: 500, y: 640 },
      { item: 'table', x: 500, y: 600 },
      { item: 'lamp', x: 210, y: 280 },
    ],
  },
  {
    name: '窗边绿植',
    layout: [
      { item: 'plant', x: 185, y: 280 },
      { item: 'table', x: 640, y: 520 },
    ],
  },
  {
    name: '休闲角',
    layout: [
      { item: 'rug', x: 500, y: 620 },
      { item: 'plant', x: 175, y: 270 },
      { item: 'lamp', x: 835, y: 270 },
      { item: 'table', x: 530, y: 600 },
    ],
  },
] satisfies { name: string; layout: Placement[] }[]

export function clampFloorCoords(x: number, y: number): { x: number; y: number } {
  return {
    x: Math.round(Math.min(FLOOR_MAX, Math.max(FLOOR_MIN, x))),
    y: Math.round(Math.min(FLOOR_MAX, Math.max(FLOOR_MIN, y))),
  }
}

export function tooClose(a: Placement, b: Placement): boolean {
  if (a.item === 'rug' || b.item === 'rug') return false
  const ca = renderCoords(a)
  const cb = renderCoords(b)
  return Math.hypot(ca.x - cb.x, ca.y - cb.y) < MIN_FURNITURE_GAP
}

export function resolveFreePosition(
  item: RoomItem,
  rawX: number,
  rawY: number,
  others: Placement[],
): { x: number; y: number } {
  const clamped = clampFloorCoords(rawX, rawY)
  if (item === 'rug') return clamped
  const peers = others.filter(entry => entry.item !== item && entry.item !== 'rug')
  const isClear = (cand: { x: number; y: number }) =>
    peers.every(peer => {
      const cp = renderCoords(peer)
      return Math.hypot(cand.x - cp.x, cand.y - cp.y) >= MIN_FURNITURE_GAP
    })
  if (isClear(clamped)) return clamped
  const radii = [88, 130, 175, 220, 270]
  for (const radius of radii) {
    let best: { x: number; y: number; dist: number } | null = null
    for (let step = 0; step < 12; step++) {
      const angle = (Math.PI * 2 * step) / 12
      const candidate = clampFloorCoords(
        clamped.x + Math.cos(angle) * radius,
        clamped.y + Math.sin(angle) * radius,
      )
      if (!isClear(candidate)) continue
      const dist = Math.hypot(candidate.x - clamped.x, candidate.y - clamped.y)
      if (!best || dist < best.dist) best = { ...candidate, dist }
    }
    if (best) return { x: best.x, y: best.y }
  }
  return clamped
}

export function defaultSpawnPosition(item: RoomItem, current: Placement[]): { x: number; y: number } {
  const base = DEFAULT_ITEM_SPOTS[item]
  return resolveFreePosition(item, base.x, base.y, current)
}

export function describeSpot(placement: Placement | undefined): string {
  if (!placement) return '不摆放'
  const { x, y } = renderCoords(placement)
  const row = y < 380 ? '后排' : y < 680 ? '中景' : '前景'
  const col = x < 360 ? '偏左' : x > 640 ? '偏右' : '居中'
  return `${row}${col}${placement.flip ? ' · 已翻转' : ''}`
}

function samePosition(a: Placement | undefined, b: Placement | undefined) {
  if (!a || !b) return a === b
  const ca = renderCoords(a)
  const cb = renderCoords(b)
  return ca.x === cb.x && ca.y === cb.y && Boolean(a.flip) === Boolean(b.flip)
}

export function sameLayout(a: Placement[], b: Placement[]) {
  return a.length === b.length && a.every(placement =>
    samePosition(placement, b.find(other => other.item === placement.item)))
}

export type LayoutConflict =
  | { kind: 'item'; item: RoomItem }
  | { kind: 'overlap'; first: Placement; second: Placement }

export type RoomMergeChoices = Partial<Record<RoomItem, 'draft' | 'latest'>>

export function conflictItems(conflicts: LayoutConflict[]): RoomItem[] {
  return [...new Set(conflicts.flatMap(conflict => conflict.kind === 'item'
    ? [conflict.item] : [conflict.first.item, conflict.second.item]))]
}

export function mergeRoomLayout(base: Placement[], draft: Placement[], latest: Placement[], choices: RoomMergeChoices = {}) {
  const layout: Placement[] = []
  const conflicts: LayoutConflict[] = []
  for (const item of items) {
    const before = base.find(placement => placement.item === item)
    const mine = draft.find(placement => placement.item === item)
    const theirs = latest.find(placement => placement.item === item)
    const choice = choices[item]
    if (choice) {
      const placement = choice === 'draft' ? mine : theirs
      if (placement) layout.push(placement)
      continue
    }
    const edited = !samePosition(mine, before)
    if (edited && !samePosition(theirs, before) && !samePosition(mine, theirs)) {
      conflicts.push({ kind: 'item', item })
      continue
    }
    const placement = edited ? mine : theirs
    if (placement) layout.push(placement)
  }

  for (const [index, first] of layout.entries()) {
    const second = layout.slice(index + 1).find(other => tooClose(first, other))
    if (second) conflicts.push({ kind: 'overlap', first, second })
  }
  return conflicts.length ? { conflicts } : { layout }
}
