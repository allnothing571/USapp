import { ROOM_ITEMS, type Placement, type RoomItem } from '@/api/shared-room'

const items = Object.keys(ROOM_ITEMS) as RoomItem[]

export const ROOM_LAYOUT_PRESETS = [
  {
    name: '茶角',
    layout: [
      { item: 'table', x: 1, y: 1 },
      { item: 'lamp', x: 0, y: 0 },
      { item: 'rug', x: 1, y: 2 },
    ],
  },
  {
    name: '窗边绿植',
    layout: [
      { item: 'plant', x: 3, y: 0 },
      { item: 'table', x: 2, y: 1 },
    ],
  },
  {
    name: '休闲角',
    layout: [
      { item: 'lamp', x: 3, y: 0 },
      { item: 'plant', x: 0, y: 0 },
      { item: 'table', x: 2, y: 2 },
      { item: 'rug', x: 1, y: 1 },
    ],
  },
] satisfies { name: string; layout: Placement[] }[]

function samePosition(a: Placement | undefined, b: Placement | undefined) {
  return a?.x === b?.x && a?.y === b?.y
}

export function sameLayout(a: Placement[], b: Placement[]) {
  return a.length === b.length && a.every(placement =>
    samePosition(placement, b.find(other => other.item === placement.item)))
}

export type LayoutConflict =
  | { kind: 'item'; item: RoomItem }
  | { kind: 'cell'; first: Placement; second: Placement }
  | { kind: 'capacity'; items: RoomItem[] }

export type RoomMergeChoices = Partial<Record<RoomItem, 'draft' | 'latest'>>

export function conflictItems(conflicts: LayoutConflict[]): RoomItem[] {
  return [...new Set(conflicts.flatMap(conflict => conflict.kind === 'item'
    ? [conflict.item] : conflict.kind === 'capacity' ? conflict.items : [conflict.first.item, conflict.second.item]))]
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
    const second = layout.slice(index + 1).find(other => samePosition(first, other))
    if (second) conflicts.push({ kind: 'cell', first, second })
  }
  if (layout.length > 4) {
    const added = layout.filter(entry => !base.some(previous => previous.item === entry.item))
    conflicts.push({ kind: 'capacity', items: added.map(entry => entry.item) })
  }
  return conflicts.length ? { conflicts } : { layout }
}
