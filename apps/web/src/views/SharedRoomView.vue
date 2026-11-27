<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { getAvatarAppearance, isAppearance, type CharacterGender } from '@/api/avatar'
import { emptyFigureSlots, originalGender, roomCharacter, type FigureSlots } from '@/api/avatar-art'
import { describeError, errorCodeOf } from '@/api/error-messages'
import { ApiError } from '@/api/client'
import { messagePreview } from '@/api/message-rules'
import { listMessages } from '@/api/messages'
import { newIdempotencyKey } from '@/api/tasks'
import {
  buyRoomFurniture,
  changeRoomPosition,
  connectSharedRoom,
  FREE_ROOM_ITEMS,
  getRoomPositions,
  getSharedRoom,
  oppositeSide,
  renderCoords,
  ROOM_ITEMS,
  saveSharedRoom,
  setRoomAvatarSharing,
  sideLabel,
  type Placement,
  type PositionAction,
  type RoomFurniture,
  type RoomItem,
  type SharedRoom,
} from '@/api/shared-room'
import type { Message } from '@/api/types'
import AvatarFigure from '@/components/AvatarFigure.vue'
import { useSmartBack } from '@/router/back-navigation'
import {
  clampFloorCoords,
  conflictItems,
  defaultSpawnPosition,
  describeSpot,
  mergeRoomLayout,
  resolveFreePosition,
  ROOM_LAYOUT_PRESETS,
  sameLayout,
  type LayoutConflict,
  type RoomMergeChoices,
} from '@/shared-room-layout'
import { useRoomDraftStore } from '@/stores/room-draft'
import { useSessionStore } from '@/stores/session'
import { useWalletStore } from '@/stores/wallet'

const back = useSmartBack('shared-room', 'home')
const session = useSessionStore()
const originalSide = computed(() => roomCharacter(session.space, session.user?.id))
const roomDraft = useRoomDraftStore()
const wallet = useWalletStore()
const panel = ref<'layout' | 'shop' | null>(null)
const activeBubble = ref<'self' | 'partner' | null>(null)

function togglePanel(next: 'layout' | 'shop') {
  activeBubble.value = null
  endPointerDrag()
  panel.value = panel.value === next ? null : next
  if (panel.value === 'shop') void wallet.load()
}

const room = ref<SharedRoom | null>(null)
const character = computed(() => room.value?.positions?.side ?? originalSide.value)
const selfName = computed(() => session.user?.display_name?.trim() || '我')
const partnerName = computed(() => {
  const me = session.user?.id
  const partner = session.space?.members?.find(member => member.user_id !== me)
  return partner?.display_name?.trim() || '对方'
})

// —— 晴昼 / 暖夜 / 静谧月夜灯光氛围偏好 ——
const cozyNight = ref(false)
const lampLit = ref(true)
function nightStorageKey(userId: string | undefined): string | null {
  return userId ? `usapp:room-night:${userId}` : null
}
function lampStorageKey(userId: string | undefined): string | null {
  return userId ? `usapp:room-lamp:${userId}` : null
}
function readBoolPref(key: string | null, fallback: boolean): boolean {
  if (!key) return fallback
  try {
    const raw = window.localStorage.getItem(key)
    return raw === null ? fallback : raw === '1'
  } catch {
    return fallback
  }
}
function writeBoolPref(key: string | null, value: boolean): void {
  if (!key) return
  try {
    window.localStorage.setItem(key, value ? '1' : '0')
  } catch {
    // Ignore storage errors in private or sandboxed sessions.
  }
}
function toggleNight(): void {
  cozyNight.value = !cozyNight.value
  writeBoolPref(nightStorageKey(session.user?.id), cozyNight.value)
  if (cozyNight.value && placedLamp.value) {
    lampLit.value = true
    writeBoolPref(lampStorageKey(session.user?.id), true)
  }
}
function onFloorLampClick(): void {
  if (!cozyNight.value) {
    cozyNight.value = true
    lampLit.value = true
    writeBoolPref(nightStorageKey(session.user?.id), true)
    writeBoolPref(lampStorageKey(session.user?.id), true)
    return
  }
  lampLit.value = !lampLit.value
  writeBoolPref(lampStorageKey(session.user?.id), lampLit.value)
}

// —— 人物轻互动与就地换位 ——
const heartBurst = ref(0)
const selfWave = ref(0)
const partnerNote = ref<Message | null>(null)
const partnerNoteStatus = ref<'idle' | 'loading' | 'ready' | 'error'>('idle')
const partnerNoteError = ref('')
let partnerNoteEpoch = 0

async function loadPartnerNote(): Promise<void> {
  if (!session.user?.id) return
  const epoch = ++partnerNoteEpoch
  partnerNoteStatus.value = 'loading'
  partnerNoteError.value = ''
  try {
    const res = await listMessages(null, 1, 'partner')
    if (epoch !== partnerNoteEpoch || !mounted) return
    partnerNote.value = res.messages[0] ?? null
    partnerNoteStatus.value = 'ready'
  } catch (cause) {
    if (epoch !== partnerNoteEpoch || !mounted) return
    partnerNoteError.value = describeError(cause)
    partnerNoteStatus.value = 'error'
  }
}

function interactPartner(): void {
  if (panel.value === 'layout') return
  heartBurst.value += 1
  if (activeBubble.value === 'partner') {
    activeBubble.value = null
    return
  }
  activeBubble.value = 'partner'
  if (partnerNoteStatus.value === 'idle' || partnerNoteStatus.value === 'error') {
    void loadPartnerNote()
  }
}

function interactSelf(): void {
  if (panel.value === 'layout') return
  selfWave.value += 1
  activeBubble.value = activeBubble.value === 'self' ? null : 'self'
}

const swapBusy = ref(false)
const swapNotice = ref('')
const swapError = ref('')
const swapUnresolved = ref<{ id: string; action: PositionAction } | null>(null)

function newSwapOperationId(): string {
  return typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function'
    ? crypto.randomUUID()
    : newIdempotencyKey()
}

async function runSwap(action: PositionAction, id = newSwapOperationId()): Promise<void> {
  if (swapBusy.value || !room.value) return
  const opGeneration = generation
  swapBusy.value = true
  swapNotice.value = ''
  swapError.value = ''
  try {
    await changeRoomPosition(id, action)
    if (opGeneration !== generation || !room.value) return
    swapUnresolved.value = null
    swapNotice.value = {
      request: '换位申请已发出，等待对方回应',
      accept: '已和对方交换位置',
      decline: '已婉拒这次换位',
      cancel: '已撤回换位申请',
    }[action]
    try {
      const updated = await getRoomPositions()
      if (opGeneration === generation && room.value) {
        room.value = { ...room.value, positions: updated }
      }
    } catch {
      refreshQueued = true
    }
  } catch (cause) {
    if (opGeneration !== generation) return
    const rejected = cause instanceof ApiError && cause.status >= 400 && cause.status < 500
    swapUnresolved.value = rejected ? null : { id, action }
    swapError.value = rejected
      ? (cause.status === 409 ? '换位状态已变化，已为你同步最新位置。' : describeError(cause))
      : '换位结果待确认，点击下方可重试同一次操作。'
  } finally {
    if (opGeneration === generation) swapBusy.value = false
  }
}

function respondIncomingSwap(action: 'accept' | 'decline'): void {
  const pendingId = room.value?.positions?.pending?.id
  if (!pendingId) return
  void runSwap(action, pendingId)
}

function cancelOutgoingSwap(): void {
  const pendingId = room.value?.positions?.pending?.id
  if (!pendingId) return
  void runSwap('cancel', pendingId)
}

const ownGender = ref<CharacterGender | undefined>()
const ownBald = ref(false)
const ownSlots = ref<FigureSlots>(emptyFigureSlots())
const layout = ref<Placement[]>([])
const undoLayouts = ref<Placement[][]>([])
const redoLayouts = ref<Placement[][]>([])
const selection = ref<RoomItem | null>(null)
const floorEl = ref<HTMLElement | null>(null)
const pointerDrag = ref<{
  item: RoomItem
  startClientX: number
  startClientY: number
  originX: number
  originY: number
  x: number
  y: number
  moved: boolean
} | null>(null)
let suppressNextItemClick = false
let suppressNextFloorClick = false
const busy = ref(false)
const purchase = ref<{ furniture: RoomFurniture; key: string; unconfirmed: boolean } | null>(null)
const furnitureChoices = computed<RoomItem[]>(() => [
  ...FREE_ROOM_ITEMS,
  ...(room.value?.furniture ?? []).filter(entry => entry.owned).map(entry => entry.item),
])
const placedItems = computed(() => new Set(layout.value.map(entry => entry.item)))
const selectedPlacement = computed(() => selection.value
  ? layout.value.find(entry => entry.item === selection.value) ?? null
  : null)
const placedLamp = computed(() => layout.value.find(entry => entry.item === 'lamp') ?? null)
const isLampIlluminated = computed(() => cozyNight.value && placedLamp.value !== null && lampLit.value)
const lampSceneVars = computed<Record<string, string> | undefined>(() => {
  if (!placedLamp.value) return undefined
  const coords = activeCoords(placedLamp.value)
  const sx = 7 + (coords.x / 1000) * 86
  const sy = 49 + (coords.y / 1000) * 32
  return {
    '--lamp-x': `${sx.toFixed(1)}%`,
    '--lamp-y': `${sy.toFixed(1)}%`,
  }
})
const previewSpot = computed(() => panel.value === 'shop' && purchase.value
  ? defaultSpawnPosition(purchase.value.furniture.item, layout.value)
  : null)
const notice = ref('')
const error = ref('')
const needsReload = ref(false)
const remoteUpdate = ref(false)
const syncUnavailable = ref(false)
const pendingMerge = ref<{ version: number; layout: Placement[]; items: RoomItem[] } | null>(null)
const mergeChoices = ref<RoomMergeChoices>({})
const dirty = computed(() => room.value !== null && !sameLayout(layout.value, room.value.layout))
const canSave = computed(() => dirty.value && !busy.value && !needsReload.value && !remoteUpdate.value)
const canEdit = computed(() => room.value !== null && !busy.value && !needsReload.value)
const historyLimit = 30
const resolution = computed(() => room.value && pendingMerge.value
  ? mergeRoomLayout(room.value.layout, layout.value, pendingMerge.value.layout, mergeChoices.value) : null)
const resolutionItems = computed(() => {
  if (!pendingMerge.value) return []
  const remaining = conflictItems(resolution.value?.conflicts ?? [])
  return [...new Set([...pendingMerge.value.items, ...Object.keys(mergeChoices.value) as RoomItem[], ...remaining])]
})
const remainingConflicts = computed(() => Object.keys(mergeChoices.value).length && resolution.value?.conflicts
  ? describeConflicts(resolution.value.conflicts) : '')
let generation = 0
let syncRead: AbortController | null = null
let syncInterval: number | undefined
let mounted = false
let socket: WebSocket | undefined
let reconnect: number | undefined
let retryDelay = 1000
let refreshQueued = false
const showPresence = ref(false)
const liveReady = ref(false)
const peerOnline = ref(false)
const liveEditing = ref(false)

function stopLive() {
  window.clearTimeout(reconnect)
  const previous = socket
  socket = undefined
  previous?.close()
  liveReady.value = false
  liveEditing.value = false
  peerOnline.value = false
  refreshQueued = false
}

function refreshFromLive() {
  refreshQueued = true
  void syncRoom()
}

function startLive() {
  if (!mounted || !room.value || socket || document.hidden) return
  let next: WebSocket
  try { next = connectSharedRoom(showPresence.value) } catch { return }
  if (!next) return
  socket = next
  next.onmessage = event => {
    if (socket !== next || document.hidden) return
    if (event.data === 'ready') {
      retryDelay = 1000
      liveReady.value = true
      refreshFromLive()
    } else if (event.data === 'room-changed') refreshFromLive()
    else if (event.data === 'peer-online') peerOnline.value = true
    else if (event.data === 'peer-not-visible') peerOnline.value = false
  }
  next.onclose = () => {
    if (socket !== next) return
    stopLive()
    reconnect = window.setTimeout(startLive, retryDelay)
    retryDelay = Math.min(retryDelay * 2, 8000)
  }
}

watch([() => !!room.value, showPresence], () => {
  stopLive()
  startLive()
})
watch(busy, value => { if (!value && refreshQueued) void syncRoom() })

function lostRoomAccess(problem: unknown) {
  return ['UNAUTHENTICATED', 'FORBIDDEN', 'SPACE_NOT_FOUND'].includes(errorCodeOf(problem) ?? '')
}

function cancelSync() {
  const current = syncRead
  syncRead = null
  current?.abort()
}

function hidePartner() {
  if (room.value?.partner_appearance) room.value = { ...room.value, partner_appearance: null }
}

function clearPendingMerge() {
  pendingMerge.value = null
  mergeChoices.value = {}
}

function rebaseDraft(next: Placement[], latest: Placement[]) {
  resetDraft(next)
  if (!sameLayout(next, latest)) undoLayouts.value.push(latest)
}

function locationOf(placements: Placement[], item: RoomItem) {
  return describeSpot(placements.find(entry => entry.item === item))
}

function describeConflicts(conflicts: LayoutConflict[]) {
  return conflicts.map(conflict => {
    if (conflict.kind === 'item') return `${ROOM_ITEMS[conflict.item]}有两种改法`
    const { first, second } = conflict
    return `${ROOM_ITEMS[first.item]}和${ROOM_ITEMS[second.item]}位置重叠了`
  }).join('；')
}

function applyResolution() {
  const current = room.value
  const latest = pendingMerge.value
  const resolved = resolution.value?.layout
  if (!current || !latest || !resolved || busy.value) return
  cancelSync()
  rebaseDraft(resolved, latest.layout)
  room.value = { ...current, version: latest.version, layout: latest.layout }
  needsReload.value = false
  remoteUpdate.value = false
  error.value = ''
  notice.value = dirty.value ? '已合并布置，确认后可保存。' : '已采用最新共享布局。'
  clearPendingMerge()
}

async function syncRoom() {
  const base = room.value
  if (!base || busy.value || syncRead || document.hidden) return
  refreshQueued = false
  const read = new AbortController()
  syncRead = read
  const deadline = window.setTimeout(() => {
    cancelSync()
    hidePartner()
    syncUnavailable.value = true
  }, 8_000)
  read.signal.addEventListener('abort', () => window.clearTimeout(deadline), { once: true })
  try {
    const next = await getSharedRoom(read.signal)
    if (syncRead !== read) return
    syncUnavailable.value = false
    if (dirty.value || needsReload.value) {
      // Retain the draft's base version, but use current consent and appearance.
      room.value = { ...base, share_avatar: next.share_avatar, partner_appearance: next.partner_appearance,
        furniture: next.furniture, positions: next.positions }
      remoteUpdate.value = next.version > base.version
      if (pendingMerge.value && next.version !== pendingMerge.value.version) {
        clearPendingMerge()
        error.value = '共享布局再次更新。请读取最新布局后重新选择。'
      }
      return
    }
    room.value = next
    remoteUpdate.value = false
    if (next.version !== base.version) {
      resetDraft(next.layout)
      notice.value = '已同步最新布局。'
    }
  } catch (problem) {
    if (syncRead !== read) return
    if (lostRoomAccess(problem)) {
      clearRoom()
      error.value = describeError(problem)
      return
    }
    hidePartner()
    syncUnavailable.value = true
  } finally {
    window.clearTimeout(deadline)
    if (syncRead === read) {
      syncRead = null
      if (refreshQueued) void syncRoom()
    }
  }
}

function onVisibilityChange() {
  if (document.hidden) {
    cancelSync()
    stopLive()
    hidePartner()
  } else {
    startLive()
    void syncRoom()
  }
}

async function load(keepDraft = false) {
  cancelSync()
  clearPendingMerge()
  const user = session.user?.id
  const space = session.space?.id
  const saved = !room.value && user && space ? roomDraft.read(user, space) : null
  const base = saved?.base ?? (keepDraft ? room.value : null)
  const draft = saved?.layout ?? layout.value
  const operation = ++generation
  busy.value = true
  error.value = ''
  notice.value = ''
  const appearance = getAvatarAppearance().catch(() => null)
  try {
    const snapshot = await getSharedRoom()
    const result = await appearance
    if (operation !== generation) return
    const next = document.hidden ? { ...snapshot, partner_appearance: null } : snapshot
    syncUnavailable.value = false
    const own = result && typeof result === 'object' && 'appearance' in result && isAppearance(result.appearance)
      ? result.appearance : null
    ownSlots.value = own?.slots ?? emptyFigureSlots()
    ownGender.value = own?.character_gender
    ownBald.value = own?.hair_hidden === true
    if (saved) {
      panel.value = 'layout'
      layout.value = saved.layout
      undoLayouts.value = saved.undoLayouts
      redoLayouts.value = saved.redoLayouts
      roomDraft.reset()
    }
    if (base) {
      const merged = mergeRoomLayout(base.layout, draft, next.layout)
      if (!merged.layout) {
        room.value = { ...next, version: base.version, layout: base.layout }
        needsReload.value = true
        remoteUpdate.value = true
        pendingMerge.value = { version: next.version, layout: next.layout, items: conflictItems(merged.conflicts) }
        error.value = `${describeConflicts(merged.conflicts)}。草稿仍在这里，请选择要保留的布置。`
        return
      }
      const matchesLatest = sameLayout(merged.layout, next.layout)
      if (next.version !== base.version) {
        rebaseDraft(merged.layout, next.layout)
      }
      if (saved && next.version === base.version) {
        notice.value = '已恢复布置记录。'
      } else {
        notice.value = matchesLatest
          ? '最新布局已包含你的摆放。'
          : '已根据最新布局保留你的布置。确认后保存布局。'
      }
    } else {
      resetDraft(next.layout)
    }
    room.value = next
    needsReload.value = false
    remoteUpdate.value = false
    selection.value = null
  } catch (problem) {
    if (operation === generation) {
      error.value = describeError(problem)
      needsReload.value = true
      if (lostRoomAccess(problem)) {
        roomDraft.reset()
        clearRoom()
      }
      else hidePartner()
    }
  } finally {
    if (operation === generation) busy.value = false
  }
}

function clearRoom() {
  stopLive()
  panel.value = null
  activeBubble.value = null
  endPointerDrag()
  showPresence.value = false
  purchase.value = null
  partnerNoteEpoch++
  partnerNote.value = null
  partnerNoteStatus.value = 'idle'
  partnerNoteError.value = ''
  swapBusy.value = false
  swapNotice.value = ''
  swapError.value = ''
  swapUnresolved.value = null
  cancelSync()
  clearPendingMerge()
  room.value = null
  ownSlots.value = emptyFigureSlots()
  ownGender.value = undefined
  ownBald.value = false
  resetDraft([])
  remoteUpdate.value = false
  syncUnavailable.value = false
}

watch([() => session.user?.id, () => session.space?.id], ([user, space]) => {
  generation++
  cozyNight.value = readBoolPref(nightStorageKey(user), false)
  lampLit.value = readBoolPref(lampStorageKey(user), true)
  clearRoom()
  busy.value = false
  error.value = ''
  notice.value = ''
  needsReload.value = false
  if (user && space) void load()
}, { immediate: true, flush: 'sync' })
onMounted(() => {
  mounted = true
  startLive()
  syncInterval = window.setInterval(syncRoom, 15_000)
  document.addEventListener('visibilitychange', onVisibilityChange)
})
onBeforeUnmount(() => {
  endPointerDrag()
  const user = session.user?.id
  const space = session.space?.id
  if (room.value && user && space && (dirty.value || undoLayouts.value.length || redoLayouts.value.length)) {
    roomDraft.remember(user, space, {
      base: { version: room.value.version, layout: room.value.layout },
      layout: layout.value,
      undoLayouts: undoLayouts.value,
      redoLayouts: redoLayouts.value,
    })
  }
  generation++
  partnerNoteEpoch++
  cancelSync()
  mounted = false
  stopLive()
  window.clearInterval(syncInterval)
  document.removeEventListener('visibilitychange', onVisibilityChange)
})

function resetDraft(next: Placement[]) {
  layout.value = next
  undoLayouts.value = []
  redoLayouts.value = []
  selection.value = null
}

function editDraft(next: Placement[]) {
  if (!canEdit.value || sameLayout(layout.value, next)) return
  undoLayouts.value.push(layout.value)
  if (undoLayouts.value.length > historyLimit) undoLayouts.value.shift()
  layout.value = next
  redoLayouts.value = []
  notice.value = ''
  if (liveEditing.value) void save()
}

type RoomLayoutPreset = (typeof ROOM_LAYOUT_PRESETS)[number]

function presetLabel(preset: RoomLayoutPreset) {
  const furniture = preset.layout.map(placement => ROOM_ITEMS[placement.item]).join('、')
  return `套用${preset.name}，包含${furniture}`
}

function applyPreset(preset: RoomLayoutPreset) {
  if (!canEdit.value || sameLayout(layout.value, preset.layout)) return
  editDraft(preset.layout)
  selection.value = null
  if (!liveEditing.value) notice.value = `已套用「${preset.name}」，确认后可保存。`
}

function stepDraft(from: Placement[][], to: Placement[][], message: string) {
  if (!canEdit.value || liveEditing.value) return
  const previous = from.pop()
  if (!previous) return
  to.push(layout.value)
  layout.value = previous
  selection.value = null
  notice.value = message
}

function selectFurniture(item: RoomItem) {
  panel.value = 'layout'
  activeBubble.value = null
  if (!canEdit.value) return
  if (!placedItems.value.has(item)) {
    const pos = defaultSpawnPosition(item, layout.value)
    error.value = ''
    editDraft([...layout.value, { item, x: pos.x, y: pos.y }])
    selection.value = item
    return
  }
  selection.value = selection.value === item ? null : item
}

function activeCoords(placement: Placement): { x: number; y: number } {
  if (pointerDrag.value?.item === placement.item && pointerDrag.value.moved) {
    return { x: pointerDrag.value.x, y: pointerDrag.value.y }
  }
  return renderCoords(placement)
}

function itemStyle(placement: Placement): Record<string, string> {
  const coords = activeCoords(placement)
  const draggingThis = pointerDrag.value?.item === placement.item && pointerDrag.value.moved
  const zIndex = placement.item === 'rug'
    ? (draggingThis ? 5 : 1)
    : (draggingThis ? 180 : 10 + Math.round(coords.y / 10))
  const scale = (0.86 + (coords.y / 1000) * 0.24).toFixed(3)
  return {
    left: `${coords.x / 10}%`,
    top: `${coords.y / 10}%`,
    zIndex: String(zIndex),
    '--depth-scale': scale,
    '--item-flip': placement.flip ? '-1' : '1',
  }
}

const toolbarStyle = computed<Record<string, string> | undefined>(() => {
  const current = selectedPlacement.value
  if (!current) return undefined
  const coords = activeCoords(current)
  const left = Math.min(82, Math.max(18, coords.x / 10))
  const above = coords.y >= 260
  return {
    left: `${left.toFixed(1)}%`,
    top: `${coords.y / 10}%`,
    '--toolbar-shift-y': above ? 'calc(-100% - 3.6rem)' : '0.85rem',
  }
})

function moveOrPlaceFree(item: RoomItem, rawX: number, rawY: number) {
  if (!canEdit.value) return
  const existing = layout.value.find(entry => entry.item === item)
  const pos = resolveFreePosition(item, rawX, rawY, layout.value)
  error.value = ''
  const nextEntry: Placement = existing?.flip
    ? { item, x: pos.x, y: pos.y, flip: true }
    : { item, x: pos.x, y: pos.y }
  editDraft([...layout.value.filter(entry => entry.item !== item), nextEntry])
  selection.value = item
}

function flipSelected() {
  const target = selectedPlacement.value
  if (!target || !canEdit.value) return
  error.value = ''
  editDraft(layout.value.map(entry => {
    if (entry.item !== target.item) return entry
    if (entry.flip) {
      const { flip: _ignored, ...rest } = entry
      return rest
    }
    return { ...entry, flip: true }
  }))
  selection.value = target.item
}

function removeSelected() {
  const target = selectedPlacement.value
  if (!target || !canEdit.value) return
  error.value = ''
  editDraft(layout.value.filter(entry => entry.item !== target.item))
  selection.value = null
}

function floorCoordsFromEvent(event: MouseEvent): { x: number; y: number } {
  const rect = floorEl.value?.getBoundingClientRect()
  if (rect && rect.width > 0 && rect.height > 0) {
    return clampFloorCoords(
      ((event.clientX - rect.left) / rect.width) * 1000,
      ((event.clientY - rect.top) / rect.height) * 1000,
    )
  }
  return clampFloorCoords(event.clientX || 500, event.clientY || 560)
}

function onFloorClick(event: MouseEvent) {
  if (suppressNextFloorClick) {
    suppressNextFloorClick = false
    return
  }
  if (panel.value !== 'layout' || !selection.value || !canEdit.value) return
  const { x, y } = floorCoordsFromEvent(event)
  moveOrPlaceFree(selection.value, x, y)
}

function onItemClick(placement: Placement) {
  if (suppressNextItemClick) {
    suppressNextItemClick = false
    return
  }
  if (panel.value === 'layout') {
    selection.value = selection.value === placement.item ? null : placement.item
  } else if (placement.item === 'lamp') {
    onFloorLampClick()
  }
}

// —— 自由画布指针平滑拖拽（鼠标 + 触屏统一） ——
function endPointerDrag() {
  pointerDrag.value = null
  window.removeEventListener('pointermove', onWindowPointerMove)
  window.removeEventListener('pointerup', onWindowPointerUp)
  window.removeEventListener('pointercancel', onWindowPointerCancel)
}

function onItemPointerDown(placement: Placement, event: PointerEvent) {
  if (panel.value !== 'layout' || !canEdit.value) return
  if (event.button && event.button !== 0) return
  const coords = renderCoords(placement)
  endPointerDrag()
  pointerDrag.value = {
    item: placement.item,
    startClientX: event.clientX,
    startClientY: event.clientY,
    originX: coords.x,
    originY: coords.y,
    x: coords.x,
    y: coords.y,
    moved: false,
  }
  window.addEventListener('pointermove', onWindowPointerMove)
  window.addEventListener('pointerup', onWindowPointerUp)
  window.addEventListener('pointercancel', onWindowPointerCancel)
}

function onWindowPointerMove(event: PointerEvent) {
  const drag = pointerDrag.value
  if (!drag) return
  const rawDx = event.clientX - drag.startClientX
  const rawDy = event.clientY - drag.startClientY
  if (!drag.moved && Math.hypot(rawDx, rawDy) > 4) {
    drag.moved = true
    selection.value = drag.item
  }
  if (!drag.moved) return
  const rect = floorEl.value?.getBoundingClientRect()
  const width = rect && rect.width > 0 ? rect.width : 400
  const height = rect && rect.height > 0 ? rect.height : 200
  const clamped = clampFloorCoords(
    drag.originX + (rawDx / width) * 1000,
    drag.originY + (rawDy / height) * 1000,
  )
  drag.x = clamped.x
  drag.y = clamped.y
}

function onWindowPointerUp() {
  const drag = pointerDrag.value
  endPointerDrag()
  if (!drag || !drag.moved) return
  suppressNextItemClick = true
  suppressNextFloorClick = true
  window.setTimeout(() => {
    suppressNextItemClick = false
    suppressNextFloorClick = false
  }, 0)
  moveOrPlaceFree(drag.item, drag.x, drag.y)
}

function onWindowPointerCancel() {
  endPointerDrag()
}

async function save() {
  const current = room.value
  if (!current || !canSave.value) return
  cancelSync()
  const operation = ++generation
  const draft = layout.value.slice()
  busy.value = true
  error.value = ''
  try {
    const version = await saveSharedRoom(current.version, draft)
    if (operation !== generation) return
    room.value = { ...(room.value ?? current), version, layout: draft }
    resetDraft(draft)
    notice.value = '已保存布局'
  } catch (problem) {
    if (operation !== generation) return
    liveEditing.value = false
    const code = errorCodeOf(problem)
    if (lostRoomAccess(problem)) clearRoom()
    needsReload.value = true
    error.value = code === 'SHARED_ROOM_VERSION_CONFLICT'
      ? '布局已有更新。草稿仍在这里，读取最新布局时可以保留草稿。'
      : describeError(problem)
  } finally {
    if (operation === generation) busy.value = false
  }
}

function selectPurchase(furniture: RoomFurniture) {
  if (busy.value || purchase.value || furniture.owned) return
  purchase.value = { furniture, key: newIdempotencyKey(), unconfirmed: false }
}

async function confirmPurchase() {
  const pending = purchase.value
  if (!room.value || busy.value || !pending) return
  cancelSync()
  const operation = ++generation
  busy.value = true
  error.value = ''
  try {
    await buyRoomFurniture(pending.furniture.item, pending.key)
    if (operation !== generation || !room.value) return
    room.value = { ...room.value, furniture: room.value.furniture?.map(entry =>
      entry.item === pending.furniture.item ? { ...entry, owned: true } : entry) }
    purchase.value = null
    notice.value = '家具已购入，你们都可以摆放。'
    if (panel.value === 'shop') void wallet.load()
  } catch (problem) {
    if (operation !== generation) return
    const code = errorCodeOf(problem)
    if (lostRoomAccess(problem)) clearRoom()
    else if (problem instanceof ApiError && problem.status >= 400 && problem.status < 500 && code !== 'RATE_LIMITED') {
      purchase.value = null
      if (code === 'ROOM_FURNITURE_OWNED') refreshQueued = true
    } else purchase.value = { ...pending, unconfirmed: true }
    error.value = code === 'INSUFFICIENT_BALANCE' ? '阳光不足，本次未扣除阳光。' : describeError(problem)
  } finally {
    if (operation === generation) busy.value = false
  }
}

async function toggleSharing() {
  const current = room.value
  if (!current || busy.value || needsReload.value) return
  cancelSync()
  const operation = ++generation
  busy.value = true
  error.value = ''
  try {
    const shared = await setRoomAvatarSharing(!current.share_avatar)
    if (operation === generation) {
      room.value = { ...(room.value ?? current), share_avatar: shared }
      notice.value = shared ? '你的形象已向对方展示' : '已停止向对方展示'
    }
  } catch (problem) {
    if (operation !== generation) return
    if (lostRoomAccess(problem)) clearRoom()
    needsReload.value = true
    error.value = describeError(problem)
  } finally {
    if (operation === generation) busy.value = false
  }
}
</script>

<template>
  <section
    class="shared-room"
    :class="{ 'shared-room--studio': panel === 'layout' || panel === 'shop' }"
    aria-labelledby="room-title"
  >
    <header class="subpage-header room-heading">
      <RouterLink
        class="back-pill"
        :to="back.to"
        :aria-label="`返回${back.label}`"
        :title="`返回${back.label}`"
        data-test="room-back"
      >‹ 返回{{ back.label }}</RouterLink>
      <h1 id="room-title" class="subpage-header__title">我们的小房间</h1>
      <details class="room-settings">
        <summary class="back-pill">设置</summary>
        <div class="room-refresh">
          <section v-if="room" class="room-preferences" aria-label="房间协作">
            <label><input v-model="showPresence" type="checkbox"> 向对方显示我在房间</label>
            <label><input v-model="liveEditing" type="checkbox" :disabled="!liveReady || busy || dirty || needsReload || remoteUpdate"> 实时保存布置</label>
            <label><input id="room-avatar-sharing" type="checkbox" :checked="room.share_avatar" :disabled="busy || needsReload" @click.prevent="toggleSharing"> 向对方展示我的形象</label>
            <p v-if="liveEditing" class="hint">每次摆放、移除或套用模板都会保存并同步给对方。</p>
          </section>
          <button v-if="room || error" class="btn btn--soft btn--small" type="button" :disabled="busy" @click="load(dirty)">
            {{ dirty ? '读取最新布局，保留草稿' : '读取最新布局' }}
          </button>
          <button v-if="dirty" class="btn btn--ghost btn--small" type="button" :disabled="busy" @click="load()">
            放弃草稿并采用最新布局
          </button>
        </div>
      </details>
    </header>

    <p v-if="error" role="alert" class="hint hint--error">{{ error }}</p>
    <p v-else-if="remoteUpdate" role="status" class="hint">有新的共享布局，可以读取最新布局并保留草稿。</p>
    <p v-else-if="syncUnavailable" role="status" class="hint">暂时无法自动同步。你可以手动读取最新布局。</p>
    <button
      v-if="room && (remoteUpdate || needsReload || syncUnavailable) && !pendingMerge"
      type="button"
      class="btn btn--soft btn--small room-sync-action"
      :disabled="busy"
      @click="load(dirty)"
    >同步最新布局</button>
    <p v-if="!room && busy" role="status">正在读取房间…</p>

    <template v-if="room">
      <div v-if="room.positions?.pending?.incoming" class="card room-swap-banner" role="status">
        <span>对方发来了换位申请，想和你交换左右站位。</span>
        <div class="row">
          <button type="button" class="btn btn--small" :disabled="swapBusy" data-test="room-banner-accept" @click="respondIncomingSwap('accept')">
            同意换位
          </button>
          <button type="button" class="btn btn--small btn--ghost" :disabled="swapBusy" data-test="room-banner-decline" @click="respondIncomingSwap('decline')">
            拒绝
          </button>
          <RouterLink class="btn btn--small btn--soft" :to="{ name: 'avatar-wardrobe', hash: '#room-position-title' }">去衣柜查看</RouterLink>
        </div>
      </div>

      <section v-if="pendingMerge" class="room-resolution" aria-labelledby="room-resolution-title">
        <h2 id="room-resolution-title">选择要保留的布置</h2>
        <fieldset v-for="item in resolutionItems" :key="item" :disabled="busy">
          <legend>{{ ROOM_ITEMS[item] }}</legend>
          <div class="room-resolution-options">
            <button
              type="button"
              class="btn btn--soft btn--small"
              :aria-pressed="mergeChoices[item] === 'draft'"
              @click="mergeChoices[item] = 'draft'"
            >我的布置 · {{ locationOf(layout, item) }}</button>
            <button
              type="button"
              class="btn btn--soft btn--small"
              :aria-pressed="mergeChoices[item] === 'latest'"
              @click="mergeChoices[item] = 'latest'"
            >共享布局 · {{ locationOf(pendingMerge.layout, item) }}</button>
          </div>
        </fieldset>
        <p v-if="remainingConflicts" class="hint" role="status">{{ remainingConflicts }}。</p>
        <button
          type="button"
          class="btn btn--primary btn--small"
          :disabled="busy || !resolution?.layout"
          @click="applyResolution"
        >应用到草稿</button>
      </section>

      <div class="room-studio-layout">
        <div class="room-stage-col">
          <section
            class="room-scene"
            :class="{
              'room-scene--editing': panel === 'layout',
              'room-scene--night': cozyNight,
              'room-scene--lamp-lit': isLampIlluminated,
              'room-scene--moonlit': cozyNight && !isLampIlluminated,
            }"
            :style="lampSceneVars"
            aria-label="共享房间"
          >
            <div v-if="cozyNight" class="room-window-glow" aria-hidden="true"></div>
            <div v-if="cozyNight" class="room-night-veil" aria-hidden="true"></div>

            <div class="room-scene-hud">
              <p v-if="liveReady && peerOnline" class="room-presence" role="status" aria-label="对方在房间">
                同在<span class="room-presence-label"> · 对方在房间</span>
              </p>
              <button
                type="button"
                class="room-light-toggle"
                data-test="room-light-toggle"
                :aria-pressed="cozyNight"
                @click="toggleNight()"
              >{{ !cozyNight ? '☀️ 晴昼' : (isLampIlluminated ? '🌙 暖夜' : '🌙 月夜') }}</button>
            </div>

            <div class="room-wall" :class="`room-wall--self-${character}`">
              <div class="room-character room-character--self" :class="{ 'room-character--wave': selfWave > 0 }">
                <AvatarFigure
                  class="room-figure-self"
                  :slots="ownSlots"
                  label="我的形象"
                  :character="character"
                  :gender="ownGender ?? originalGender(originalSide)"
                  :bald="ownBald"
                />
                <button
                  type="button"
                  class="room-character__trigger"
                  data-test="interact-self"
                  :disabled="panel === 'layout'"
                  :aria-expanded="activeBubble === 'self'"
                  :aria-label="`${selfName}，点此换装或换位`"
                  @click="interactSelf()"
                >
                  <span class="room-nameplate">{{ selfName }}</span>
                </button>
              </div>

              <div v-if="room.partner_appearance" class="room-character room-character--partner">
                <span v-if="heartBurst > 0" :key="heartBurst" class="room-heart-burst" aria-hidden="true">💗</span>
                <AvatarFigure
                  class="room-figure-partner"
                  :slots="room.partner_appearance.slots"
                  label="对方的形象"
                  :character="oppositeSide(character)"
                  :gender="room.partner_appearance.character_gender ?? originalGender(oppositeSide(originalSide))"
                  :bald="room.partner_appearance.hair_hidden"
                />
                <button
                  type="button"
                  class="room-character__trigger"
                  data-test="interact-partner"
                  :disabled="panel === 'layout'"
                  :aria-expanded="activeBubble === 'partner'"
                  :aria-label="`${partnerName}，点此打招呼或看留言`"
                  @click="interactPartner()"
                >
                  <span class="room-nameplate" :class="{ 'room-nameplate--online': liveReady && peerOnline }">{{ partnerName }}</span>
                </button>
              </div>
            </div>

            <div
              ref="floorEl"
              class="room-floor"
              :class="{ 'room-floor--editing': panel === 'layout' }"
              data-test="room-floor"
              aria-label="家具位置"
              @click="onFloorClick($event)"
            >
              <button
                v-for="placement in layout"
                :key="placement.item"
                type="button"
                class="room-item"
                :class="{
                  'room-item--selected': panel === 'layout' && selection === placement.item,
                  'room-item--dragging': pointerDrag?.item === placement.item && pointerDrag.moved,
                  'room-item--interactive-lamp': panel !== 'layout' && placement.item === 'lamp',
                  'room-item--lamp-lit': isLampIlluminated && placement.item === 'lamp',
                }"
                :style="itemStyle(placement)"
                :data-item="placement.item"
                :data-test="`room-item-${placement.item}`"
                :data-flipped="placement.flip ? 'true' : undefined"
                :disabled="busy || needsReload"
                :tabindex="panel === 'layout' || placement.item === 'lamp' ? 0 : -1"
                :aria-label="`${ROOM_ITEMS[placement.item]}（${describeSpot(placement)}）`"
                @click.stop="onItemClick(placement)"
                @pointerdown.stop="onItemPointerDown(placement, $event)"
              >
                <span v-if="isLampIlluminated && placement.item === 'lamp'" class="room-lamp-glow" aria-hidden="true"></span>
                <span v-if="isLampIlluminated && placement.item === 'lamp'" class="room-lamp-cone" aria-hidden="true"></span>
                <span class="room-art room-floor-art" :data-item="placement.item" aria-hidden="true"></span>
                <span v-if="isLampIlluminated && placement.item === 'lamp'" class="room-lamp-bulb" aria-hidden="true"></span>
                <span class="room-item-label">{{ ROOM_ITEMS[placement.item] }}</span>
              </button>

              <span
                v-if="previewSpot && purchase"
                class="room-item room-item--preview"
                :data-item="purchase.furniture.item"
                :style="{ left: `${previewSpot.x / 10}%`, top: `${previewSpot.y / 10}%`, zIndex: 150 }"
                aria-hidden="true"
              >
                <span class="room-art room-floor-art room-preview" :data-item="purchase.furniture.item"></span>
              </span>

              <div
                v-if="panel === 'layout' && selectedPlacement && !pointerDrag?.moved"
                class="room-item-toolbar"
                data-test="room-item-toolbar"
                :style="toolbarStyle"
                @click.stop
                @pointerdown.stop
              >
                <span class="room-toolbar-name">{{ ROOM_ITEMS[selectedPlacement.item] }}</span>
                <button
                  type="button"
                  class="room-toolbar-btn"
                  data-test="room-item-flip"
                  :aria-pressed="Boolean(selectedPlacement.flip)"
                  :disabled="!canEdit"
                  @click="flipSelected()"
                >↔ 翻转</button>
                <button
                  type="button"
                  class="room-toolbar-btn room-toolbar-btn--danger"
                  data-test="room-item-remove"
                  :disabled="!canEdit"
                  @click="removeSelected()"
                >收起</button>
              </div>
            </div>
          </section>

          <!-- 人物互动气泡卡（仅非布置态下点击自己/对方展开） -->
          <div v-if="panel === null && activeBubble === 'partner'" class="card card--note stack room-bubble" data-test="partner-bubble">
            <div class="row" style="justify-content: space-between;">
              <strong>{{ partnerName }}的小纸条与动态</strong>
              <span class="badge" :class="liveReady && peerOnline ? '' : 'badge--muted'">
                {{ liveReady && peerOnline ? '此刻也在房间里' : '当前不在房间' }}
              </span>
            </div>
            <p v-if="partnerNoteStatus === 'loading'" class="card__hint">正在读取对方最近一条小纸条…</p>
            <p v-else-if="partnerNoteStatus === 'error'" class="hint hint--error">{{ partnerNoteError }}</p>
            <p v-else-if="partnerNote" class="room-bubble__quote">「{{ messagePreview(partnerNote.body) }}」</p>
            <p v-else class="card__hint">对方还没有写新纸条，先给对方写一张吧。</p>
            <div class="row">
              <RouterLink
                v-if="partnerNote"
                class="btn btn--small"
                :to="{ name: 'message-detail', params: { id: partnerNote.id } }"
                data-test="room-partner-read-note"
              >看纸条</RouterLink>
              <RouterLink
                class="btn btn--small btn--soft"
                :to="{ name: 'messages' }"
                data-test="room-partner-write-note"
              >写小纸条</RouterLink>
              <button type="button" class="btn btn--small btn--ghost" @click="activeBubble = null">收起</button>
            </div>
          </div>

          <div v-if="panel === null && activeBubble === 'self'" class="card stack room-bubble" data-test="self-bubble">
            <div class="row" style="justify-content: space-between;">
              <strong>{{ selfName }}</strong>
              <span class="badge badge--accent">当前站在{{ sideLabel(character) }}</span>
            </div>
            <p v-if="swapNotice" class="hint hint--ok">{{ swapNotice }}</p>
            <p v-if="swapError" class="hint hint--error">{{ swapError }}</p>
            <div class="row">
              <RouterLink
                class="btn btn--small btn--soft"
                :to="{ name: 'avatar-wardrobe' }"
                data-test="room-self-wardrobe"
              >去衣柜换装</RouterLink>
              <template v-if="room.positions?.paired">
                <button
                  v-if="swapUnresolved"
                  type="button"
                  class="btn btn--small"
                  :disabled="swapBusy"
                  data-test="room-swap-retry"
                  @click="runSwap(swapUnresolved.action, swapUnresolved.id)"
                >重试换位操作</button>
                <template v-else-if="room.positions.pending?.incoming">
                  <button
                    type="button"
                    class="btn btn--small"
                    :disabled="swapBusy"
                    data-test="room-swap-accept"
                    @click="respondIncomingSwap('accept')"
                  >同意换位</button>
                  <button
                    type="button"
                    class="btn btn--small btn--ghost"
                    :disabled="swapBusy"
                    data-test="room-swap-decline"
                    @click="respondIncomingSwap('decline')"
                  >拒绝</button>
                </template>
                <button
                  v-else-if="room.positions.pending"
                  type="button"
                  class="btn btn--small btn--soft"
                  :disabled="swapBusy"
                  data-test="room-swap-cancel"
                  @click="cancelOutgoingSwap()"
                >撤回换位申请</button>
                <button
                  v-else
                  type="button"
                  class="btn btn--small btn--soft"
                  :disabled="swapBusy"
                  data-test="room-swap-request"
                  @click="runSwap('request')"
                >申请和对方换位</button>
              </template>
              <button type="button" class="btn btn--small btn--ghost" @click="activeBubble = null">收起</button>
            </div>
          </div>

          <nav class="room-dock" aria-label="小房间功能">
            <button
              class="room-dock-action"
              type="button"
              :aria-expanded="panel === 'layout'"
              aria-controls="room-layout-panel"
              @click="togglePanel('layout')"
            >
              <span class="room-art" data-item="armchair" aria-hidden="true"></span>布置
            </button>
            <button
              v-if="room.furniture?.length"
              class="room-dock-action"
              type="button"
              :aria-expanded="panel === 'shop'"
              aria-controls="room-shop-panel"
              @click="togglePanel('shop')"
            >
              <span class="room-art" data-item="shop" aria-hidden="true"></span>商店
            </button>
            <RouterLink class="room-dock-action" :to="{ name: 'avatar-wardrobe' }" data-test="room-dock-wardrobe">
              <svg
                class="room-dock-svg"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="1.8"
                stroke-linecap="round"
                stroke-linejoin="round"
                aria-hidden="true"
              >
                <path d="M12 4a2.2 2.2 0 0 1 2 2.2c0 1-.8 1.6-2 2.3L4.5 14h15L12 8.5" />
                <path d="M5 14v4.5h14V14" />
              </svg>
              <span>衣柜</span>
            </RouterLink>
            <RouterLink class="room-dock-action" :to="{ name: 'shared-garden' }" data-test="room-dock-garden">
              <span class="room-art" data-item="garden" aria-hidden="true"></span>花园
            </RouterLink>
          </nav>

          <p v-if="notice && !error && !remoteUpdate && !syncUnavailable" role="status" class="room-notice">{{ notice }}</p>
        </div>

        <div class="room-workbench-col">
          <section v-show="panel === 'layout'" id="room-layout-panel" class="room-controls" aria-label="布置家具">
            <div class="room-panel-heading">
              <h2>布置工作台</h2>
              <span class="badge" data-test="room-capacity-badge">已摆放 {{ layout.length }} / {{ furnitureChoices.length }}</span>
              <button type="button" class="btn btn--ghost btn--small" @click="panel = null">收起</button>
            </div>
            <p class="card__hint">点选下方家具一键放进房间，直接在木地板上自由拖动或点地板落位；选中家具可翻转朝向或收起。</p>
            <div class="room-tools">
              <button
                v-for="item in furnitureChoices"
                :key="item"
                type="button"
                class="btn btn--soft btn--small room-furniture-btn"
                :data-placed="placedItems.has(item) || undefined"
                :aria-pressed="selection === item"
                :disabled="busy || needsReload"
                @click="selectFurniture(item)"
              ><span class="room-art" :data-item="item" aria-hidden="true"></span>{{ ROOM_ITEMS[item] }}</button>
              <button
                type="button"
                class="btn btn--soft btn--small"
                data-test="workbench-flip-btn"
                :disabled="!canEdit || !selectedPlacement"
                @click="flipSelected()"
              >翻转朝向</button>
              <button
                type="button"
                class="btn btn--ghost btn--small"
                data-test="workbench-remove-btn"
                :disabled="!canEdit || !selectedPlacement"
                @click="removeSelected()"
              >移除选中家具</button>
              <div class="room-draft-history" role="group" aria-label="草稿操作">
                <button
                  type="button"
                  class="btn btn--ghost btn--small"
                  :disabled="!canEdit || liveEditing || !undoLayouts.length"
                  @click="stepDraft(undoLayouts, redoLayouts, '已撤销一步。')"
                >撤销</button>
                <button
                  type="button"
                  class="btn btn--ghost btn--small"
                  :disabled="!canEdit || liveEditing || !redoLayouts.length"
                  @click="stepDraft(redoLayouts, undoLayouts, '已重做一步。')"
                >重做</button>
              </div>
            </div>
          </section>

          <section v-if="room.furniture?.length" v-show="panel === 'shop'" id="room-shop-panel" class="room-controls room-shop" aria-labelledby="room-shop-title">
            <div class="room-shop-heading">
              <h2 id="room-shop-title">家具商店</h2>
              <div class="row">
                <span v-if="wallet.balances" class="wallet-pill wallet-pill--green">我的阳光 {{ wallet.balances.sunshine }}</span>
                <button v-else-if="wallet.status === 'error'" type="button" class="btn btn--small btn--ghost" @click="wallet.load()">重试读取阳光</button>
                <span v-else class="card__hint">正在读取阳光…</span>
                <button type="button" class="btn btn--ghost btn--small" @click="panel = null">收起</button>
              </div>
            </div>
            <div class="room-tools">
              <button
                v-for="entry in room.furniture"
                :key="entry.item"
                type="button"
                class="btn btn--soft btn--small"
                :aria-pressed="purchase?.furniture.item === entry.item"
                :disabled="entry.owned || busy || !!purchase"
                @click="selectPurchase(entry)"
              >
                <span class="room-art" :data-item="entry.item" aria-hidden="true"></span>{{ ROOM_ITEMS[entry.item] }} · {{ entry.owned ? '空间已拥有' : `${entry.price} 阳光` }}
              </button>
            </div>
            <div v-if="purchase" class="room-shop-confirm">
              <p>用我的 {{ purchase.furniture.price }} 阳光购买{{ ROOM_ITEMS[purchase.furniture.item] }}，两人均可摆放。</p>
              <p v-if="previewSpot" class="hint">房间里的半透明家具是试摆预览。</p>
              <p v-if="purchase.unconfirmed" role="status">结果待确认。重试会使用同一笔购买请求。</p>
              <div class="row">
                <button type="button" class="btn btn--primary btn--small" :disabled="busy" @click="confirmPurchase">
                  {{ purchase.unconfirmed ? '确认购买结果' : '确认购买' }}
                </button>
                <button v-if="!purchase.unconfirmed" type="button" class="btn btn--ghost btn--small" :disabled="busy" @click="purchase = null">取消</button>
              </div>
            </div>
          </section>

          <section v-show="panel === 'layout'" class="room-controls room-layout-actions" aria-label="保存布置">
            <details class="room-templates">
              <summary>布置模板</summary>
              <p class="hint">套用后会替换当前草稿，可撤销。</p>
              <div class="room-template-list">
                <button
                  v-for="preset in ROOM_LAYOUT_PRESETS"
                  :key="preset.name"
                  type="button"
                  class="btn btn--soft btn--small room-template"
                  :disabled="!canEdit || sameLayout(layout, preset.layout)"
                  :aria-label="presetLabel(preset)"
                  @click="applyPreset(preset)"
                >
                  <span class="room-template-board" aria-hidden="true">
                    <span
                      v-for="placement in preset.layout"
                      :key="placement.item"
                      class="room-art"
                      :data-item="placement.item"
                      :style="{ left: `${placement.x / 10}%`, top: `${placement.y / 10}%`, zIndex: placement.item === 'rug' ? 1 : 2 }"
                    ></span>
                  </span>
                  <span>套用{{ preset.name }}</span>
                </button>
              </div>
            </details>
            <div class="room-actions">
              <button type="button" class="btn btn--primary btn--block" :disabled="!canSave" @click="save">保存布局</button>
            </div>
          </section>
        </div>
      </div>
    </template>
  </section>
</template>

<style scoped>
.shared-room {
  margin: 0 auto;
  max-width: 38rem;
  transition: max-width 200ms ease;
}

.shared-room--studio {
  max-width: 64rem;
}

.room-heading {
  position: relative;
  z-index: 6;
}

.room-settings {
  position: relative;
}

.room-settings > summary {
  list-style: none;
  cursor: pointer;
}

.room-settings summary::-webkit-details-marker {
  display: none;
}

.room-refresh {
  position: absolute;
  top: calc(100% + 0.5rem);
  right: 0;
  z-index: 8;
  width: min(88vw, 19.5rem);
  display: grid;
  gap: 0.65rem;
  padding: 0.95rem;
  background: var(--surface);
  border: 1px solid var(--line);
  border-radius: var(--radius);
  box-shadow: var(--shadow);
}

.room-preferences {
  display: grid;
  gap: 0.6rem;
}

.room-preferences label {
  display: flex;
  align-items: center;
  gap: 0.45rem;
  font-size: 0.92rem;
}

.room-preferences input {
  accent-color: var(--green);
}

.room-swap-banner {
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 10px;
  margin-bottom: 12px;
  padding: 12px 14px;
  background: #fff7eb;
  border-color: rgba(163, 77, 43, .3);
  font-size: 0.92rem;
}

.room-studio-layout {
  display: grid;
  grid-template-columns: minmax(0, 1fr);
  gap: 14px;
  align-items: start;
}

@media (min-width: 900px) {
  .shared-room--studio .room-studio-layout {
    grid-template-columns: minmax(0, 1.18fr) minmax(19rem, 0.82fr);
    gap: 18px;
  }
}

.room-stage-col {
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.room-workbench-col {
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.room-scene {
  position: relative;
  isolation: isolate;
  aspect-ratio: 1.04;
  background: #e4ad6f url('/room-art/sunlit-room.jpg') center / 100% 100% no-repeat;
  border: 1px solid #c58d5b;
  border-radius: 1.3rem;
  box-shadow: var(--shadow);
  overflow: hidden;
  transition: border-color 280ms ease;
}

.room-scene--night {
  background-color: #2f2a3e;
  background-image: url('/room-art/night-room.jpg');
  border-color: #4f4b66;
}

.room-window-glow {
  position: absolute;
  left: 3.5%;
  top: 6%;
  width: 21%;
  height: 45%;
  z-index: 1;
  pointer-events: none;
  border-radius: 48% 48% 10% 10%;
  background: radial-gradient(ellipse at 54% 38%, rgba(168, 202, 255, 0.22), transparent 72%);
}

.room-night-veil {
  position: absolute;
  inset: 0;
  z-index: 2;
  pointer-events: none;
  transition: background 280ms ease;
}

.room-scene--lamp-lit .room-night-veil {
  background: radial-gradient(
    circle at var(--lamp-x, 79%) var(--lamp-y, 58%),
    rgba(255, 224, 146, 0.28) 0%,
    rgba(255, 186, 92, 0.12) 32%,
    rgba(20, 24, 46, 0.28) 76%
  );
}

.room-scene--moonlit .room-night-veil {
  background: linear-gradient(180deg, rgba(18, 24, 50, 0.24), rgba(14, 18, 38, 0.40));
}

.room-scene-hud {
  position: absolute;
  top: 0.7rem;
  left: 0.7rem;
  right: 0.7rem;
  z-index: 6;
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: 8px;
  pointer-events: none;
}

.room-scene-hud > * {
  pointer-events: auto;
}

.room-presence {
  margin: 0 auto 0 0;
  padding: 0.28rem 0.72rem;
  border: 1px solid rgba(74, 103, 70, .22);
  border-radius: 2rem;
  background: rgba(255, 251, 243, .92);
  color: var(--green);
  font-size: 0.82rem;
  font-weight: 650;
}

.room-presence::before {
  content: '';
  display: inline-block;
  width: 0.5rem;
  height: 0.5rem;
  margin-right: 0.35rem;
  border-radius: 50%;
  background: var(--green);
}

.room-presence-label {
  position: absolute;
  width: 1px;
  height: 1px;
  overflow: hidden;
  clip-path: inset(50%);
  white-space: nowrap;
}

.room-light-toggle {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  min-height: 34px;
  padding: 4px 11px;
  border: 1px solid var(--line);
  border-radius: 999px;
  background: rgba(255, 251, 243, .9);
  backdrop-filter: blur(6px);
  color: var(--ink);
  font-size: 0.8rem;
  font-weight: 600;
  cursor: pointer;
  box-shadow: 0 2px 8px rgba(96, 62, 31, .08);
  transition: transform 120ms ease, background-color 140ms ease;
}

.room-light-toggle:hover {
  background: var(--surface);
}

.room-light-toggle[aria-pressed="true"] {
  background: rgba(54, 42, 36, .88);
  border-color: rgba(244, 228, 184, .45);
  color: #f9ebc7;
}

.room-wall {
  position: absolute;
  inset: auto 19% 35%;
  z-index: 3;
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  align-items: end;
  justify-items: center;
  gap: 1rem;
  pointer-events: none;
}

.room-scene--moonlit .room-wall .room-character :deep(.avatar-figure) {
  filter: brightness(0.84) saturate(0.88);
}

.room-scene--lamp-lit .room-wall .room-character :deep(.avatar-figure) {
  filter: brightness(0.96) drop-shadow(0 0 8px rgba(255, 214, 142, 0.22));
}

.room-character {
  position: relative;
  display: grid;
  justify-items: center;
  grid-row: 1;
}

.room-wall--self-left .room-character--self,
.room-wall--self-right .room-character--partner {
  grid-column: 1;
}

.room-wall--self-right .room-character--self,
.room-wall--self-left .room-character--partner {
  grid-column: 2;
}

.room-character::before {
  content: '';
  position: absolute;
  bottom: 0.2rem;
  width: 68%;
  height: 12px;
  border-radius: 50%;
  background: radial-gradient(ellipse at center, rgba(84, 52, 24, 0.34), transparent 72%);
  pointer-events: none;
}

.room-character :deep(.avatar-figure) {
  width: clamp(5rem, 23vw, 8.3rem);
  margin: 0;
  transform-origin: bottom center;
  animation: roomBreathe 4.2s ease-in-out infinite;
}

.room-character--partner :deep(.avatar-figure) {
  animation-delay: -2.1s;
}

.room-character--wave :deep(.avatar-figure) {
  animation: roomWave 520ms ease-out 1, roomBreathe 4.2s ease-in-out infinite 520ms;
}

.room-character :deep(.avatar-canvas) {
  background: transparent;
  border-radius: 0;
}

.room-character :deep(figcaption) {
  position: absolute;
  width: 1px;
  height: 1px;
  overflow: hidden;
  clip-path: inset(50%);
}

.room-character :deep(.hint) {
  padding: 0.25rem;
  background: var(--surface);
  font-size: 0.7rem;
}

.room-character__trigger {
  position: absolute;
  inset: 0 -0.25rem -1.2rem;
  z-index: 3;
  display: flex;
  align-items: flex-end;
  justify-content: center;
  padding: 0;
  border: 0;
  background: transparent;
  cursor: pointer;
  pointer-events: auto;
}

.room-character__trigger:disabled {
  cursor: default;
  pointer-events: none;
}

.room-nameplate {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 2px 9px;
  border: 1px solid rgba(132, 92, 54, .22);
  border-radius: 999px;
  background: rgba(255, 251, 243, .92);
  color: var(--ink);
  font-size: 0.73rem;
  font-weight: 650;
  line-height: 1.35;
  box-shadow: 0 2px 6px rgba(96, 62, 31, .08);
  transition: transform 140ms ease, border-color 140ms ease;
}

.room-character__trigger:not(:disabled):hover .room-nameplate,
.room-character__trigger[aria-expanded="true"] .room-nameplate {
  border-color: var(--accent);
  transform: translateY(-1px);
}

.room-nameplate--online::before {
  content: '';
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: var(--green);
}

.room-heart-burst {
  position: absolute;
  top: 6%;
  z-index: 7;
  font-size: 1.25rem;
  pointer-events: none;
  animation: roomHeartFloat 900ms ease-out forwards;
}

.room-floor {
  position: absolute;
  inset: 61% 7% 5%;
  z-index: 4;
  pointer-events: none;
  border-radius: 0.9rem;
  border: 1.5px dashed transparent;
  transition: border-color 180ms ease, background-color 180ms ease;
}

.room-floor--editing {
  pointer-events: auto;
  border-color: rgba(132, 86, 48, 0.34);
  background: rgba(255, 248, 232, 0.08);
  cursor: crosshair;
  touch-action: none;
}

.room-item {
  position: absolute;
  display: grid;
  align-items: end;
  justify-items: center;
  width: 22%;
  padding: 0;
  border: 0;
  background: transparent;
  color: var(--ink);
  pointer-events: none;
  transform: translate(-50%, -88%);
  transition: left 90ms linear, top 90ms linear, filter 160ms ease;
}

.room-item--dragging {
  transition: none !important;
  cursor: grabbing !important;
}

.room-item[data-item="rug"] {
  width: 28%;
  transform: translate(-50%, -50%);
}

.room-item[data-item="vase"] {
  width: 14%;
}

.room-item[data-item="lamp"] {
  width: 26%;
}

.room-item--interactive-lamp {
  pointer-events: auto;
  cursor: pointer;
}

.room-scene--editing .room-item {
  pointer-events: auto;
  cursor: grab;
  touch-action: none;
}

.room-scene--editing .room-item:focus-visible {
  outline: 2px solid var(--green);
  outline-offset: 4px;
  border-radius: 0.7rem;
}

.room-item--selected::after {
  content: '';
  position: absolute;
  bottom: -4px;
  left: 10%;
  right: 10%;
  height: 14px;
  border-radius: 50%;
  border: 2px solid var(--accent);
  background: rgba(244, 228, 184, 0.45);
  box-shadow: 0 0 10px rgba(163, 77, 43, 0.32);
  pointer-events: none;
  z-index: 0;
}

.room-item:not([data-item="rug"]) .room-floor-art {
  filter: drop-shadow(0 5px 5px rgba(72, 42, 16, 0.24));
}

.room-scene--moonlit .room-item:not([data-item="rug"]) .room-floor-art {
  filter: brightness(0.84) saturate(0.88) drop-shadow(0 5px 5px rgba(14, 18, 36, 0.34));
}

.room-item--lamp-lit .room-floor-art {
  filter: brightness(1.22) saturate(1.15) drop-shadow(0 0 14px rgba(255, 214, 118, 0.92)) drop-shadow(0 5px 6px rgba(40, 24, 12, 0.36)) !important;
}

.room-floor-art {
  position: relative;
  z-index: 1;
  width: 100%;
  pointer-events: none;
  transform: scale(var(--depth-scale, 1)) scaleX(var(--item-flip, 1));
  transform-origin: bottom center;
  transition: transform 140ms ease;
}

.room-item[data-item="rug"] .room-floor-art {
  transform-origin: center center;
}

.room-lamp-glow {
  position: absolute;
  left: 50%;
  top: 18%;
  width: 280%;
  height: 220%;
  border-radius: 50%;
  transform: translate(-50%, -35%);
  background: radial-gradient(circle, rgba(255, 224, 140, 0.56) 0%, rgba(255, 188, 92, 0.24) 44%, transparent 72%);
  mix-blend-mode: screen;
  pointer-events: none;
  z-index: 0;
  animation: roomLampPulse 3.4s ease-in-out infinite;
}

.room-lamp-cone {
  position: absolute;
  top: 26%;
  left: 50%;
  width: 210%;
  height: 92%;
  transform: translateX(-50%);
  border-radius: 50% 50% 48% 48%;
  background: radial-gradient(
    ellipse at 50% 18%,
    rgba(255, 232, 168, 0.44) 0%,
    rgba(255, 202, 118, 0.24) 38%,
    rgba(255, 184, 96, 0.08) 66%,
    transparent 82%
  );
  mix-blend-mode: screen;
  pointer-events: none;
  z-index: 0;
}

.room-lamp-bulb {
  position: absolute;
  top: 3%;
  left: 50%;
  width: 58%;
  height: 34%;
  border-radius: 50%;
  transform: translateX(-50%);
  background: radial-gradient(ellipse at center, rgba(255, 252, 220, 0.92) 0%, rgba(255, 224, 134, 0.58) 48%, transparent 78%);
  mix-blend-mode: screen;
  pointer-events: none;
  z-index: 2;
}

.room-item--preview {
  pointer-events: none;
}

.room-preview {
  opacity: 0.6;
  filter: drop-shadow(0 0 6px #fff8dc);
}

.room-item-label {
  display: none;
  position: absolute;
  bottom: -2px;
  z-index: 3;
  border-radius: 1rem;
  padding-inline: 0.35rem;
  font-size: 0.65rem;
  line-height: 1.35;
  background: rgba(255, 251, 243, 0.95);
  box-shadow: 0 1px 4px rgba(72, 42, 16, 0.14);
  pointer-events: none;
  white-space: nowrap;
}

.room-scene--editing .room-item-label {
  display: inline;
}

.room-item-toolbar {
  position: absolute;
  z-index: 220;
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 4px 6px;
  border: 1px solid var(--line);
  border-radius: 999px;
  background: rgba(255, 251, 243, 0.96);
  backdrop-filter: blur(8px);
  box-shadow: 0 6px 18px rgba(72, 42, 16, 0.20);
  transform: translate(-50%, var(--toolbar-shift-y, calc(-100% - 3.6rem)));
  white-space: nowrap;
}

.room-toolbar-name {
  padding-inline: 5px;
  font-size: 0.74rem;
  font-weight: 650;
  color: var(--ink-soft);
}

.room-toolbar-btn {
  border: 1px solid rgba(132, 92, 54, 0.24);
  border-radius: 999px;
  padding: 2px 8px;
  background: var(--surface-soft);
  color: var(--ink);
  font-size: 0.74rem;
  font-weight: 600;
  cursor: pointer;
}

.room-toolbar-btn[aria-pressed="true"] {
  background: var(--honey);
  border-color: var(--accent);
}

.room-toolbar-btn--danger {
  color: #8f3524;
  border-color: rgba(143, 53, 36, 0.25);
}

.room-bubble {
  padding: 14px 16px;
}

.room-bubble__quote {
  margin: 0;
  padding: 8px 12px;
  border-radius: var(--radius-sm);
  background: rgba(244, 228, 184, .35);
  color: var(--ink);
  font-size: 0.94rem;
}

.room-dock {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 8px;
}

.room-dock-action {
  display: grid;
  align-content: center;
  justify-items: center;
  gap: 2px;
  min-height: 58px;
  padding: 6px 4px;
  border: 1px solid var(--line);
  border-radius: var(--radius);
  background: var(--surface);
  box-shadow: 0 2px 8px rgba(96, 62, 31, .05);
  color: var(--ink);
  text-decoration: none;
  font: inherit;
  font-size: 0.86rem;
  font-weight: 650;
  cursor: pointer;
  transition: transform 120ms ease, background-color 140ms ease, border-color 140ms ease;
}

.room-dock-action:hover {
  background: var(--surface-soft);
  border-color: rgba(163, 77, 43, .35);
}

.room-dock-action:active {
  transform: scale(0.98);
}

.room-dock-action[aria-expanded="true"] {
  background: var(--honey);
  border-color: #b1854e;
}

.room-dock-action .room-art {
  width: 2.15rem;
}

.room-dock-svg {
  width: 1.55rem;
  height: 1.55rem;
  margin-block: 4px;
  color: var(--accent);
}

.room-controls {
  padding: 14px 16px;
  border: 1px solid var(--line);
  border-radius: var(--radius);
  background: var(--surface);
  box-shadow: var(--shadow);
}

.room-panel-heading,
.room-shop-heading {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  margin-bottom: 10px;
}

.room-panel-heading h2,
.room-shop-heading h2 {
  margin: 0 auto 0 0;
  font-size: 1.02rem;
}

.room-controls p {
  margin-bottom: 10px;
}

.room-controls .room-art {
  width: 1.6rem;
}

.room-tools,
.room-actions,
.room-draft-history {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px;
}

.room-furniture-btn[data-placed="true"] {
  position: relative;
  border-color: rgba(74, 103, 70, .42);
}

.room-furniture-btn[data-placed="true"]::after {
  content: '';
  width: 6px;
  height: 6px;
  margin-left: 4px;
  border-radius: 50%;
  background: var(--green);
}

.room-shop-confirm {
  margin-top: 12px;
  padding-top: 12px;
  border-top: 1px dashed var(--line);
}

.room-actions {
  margin-top: 12px;
}

.room-tools [aria-pressed="true"] {
  outline: 2px solid var(--green);
}

.room-resolution {
  margin-bottom: 1rem;
  padding: 0.9rem;
  background: var(--surface);
  border: 1px solid var(--line);
  border-radius: var(--radius);
}

.room-resolution h2 {
  margin: 0 0 0.65rem;
  font-size: 1.05rem;
}

.room-resolution fieldset {
  min-width: 0;
  margin: 0 0 0.55rem;
  padding: 0.45rem 0.55rem 0.65rem;
  border: 1px solid var(--line);
  border-radius: 0.6rem;
}

.room-resolution-options {
  display: flex;
  flex-wrap: wrap;
  gap: 0.45rem;
}

.room-resolution-options button {
  white-space: normal;
}

.room-resolution-options [aria-pressed="true"] {
  outline: 2px solid var(--green);
  outline-offset: -2px;
}

.room-templates summary {
  display: inline-flex;
  align-items: center;
  cursor: pointer;
  width: fit-content;
  min-height: 40px;
  font-weight: 600;
}

.room-template-list {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(min(100%, 6rem), 1fr));
  gap: 0.55rem;
}

.room-template {
  display: grid;
  gap: 0.3rem;
  min-width: 0;
  text-align: center;
  white-space: normal;
}

.room-template-board {
  position: relative;
  width: 100%;
  height: 3.1rem;
  border-radius: 0.45rem;
  background: rgba(244, 228, 184, 0.28);
  border: 1px solid rgba(132, 92, 54, 0.16);
  overflow: hidden;
}

.room-template-board .room-art {
  position: absolute;
  width: 1.05rem;
  transform: translate(-50%, -62%);
}

.room-sync-action {
  margin-bottom: 0.7rem;
}

.room-notice {
  margin: 0;
  color: var(--green);
  font-size: 0.88rem;
}

@keyframes roomBreathe {
  0%, 100% { transform: scaleY(1); }
  50% { transform: scaleY(1.018); }
}

@keyframes roomWave {
  0%, 100% { transform: translateY(0) scale(1); }
  45% { transform: translateY(-6px) scale(1.03); }
}

@keyframes roomHeartFloat {
  0% { opacity: 0; transform: translateY(6px) scale(0.75); }
  25% { opacity: 1; transform: translateY(-6px) scale(1.08); }
  100% { opacity: 0; transform: translateY(-28px) scale(0.95); }
}

@keyframes roomLampPulse {
  0%, 100% { opacity: 0.85; transform: scale(1); }
  50% { opacity: 1; transform: scale(1.05); }
}

@media (max-width: 480px) {
  .room-scene {
    aspect-ratio: 0.88;
  }
  .room-character :deep(.avatar-figure) {
    width: clamp(4.6rem, 23vw, 7.4rem);
  }
}

@media (prefers-reduced-motion: reduce) {
  .room-character :deep(.avatar-figure),
  .room-character--wave :deep(.avatar-figure),
  .room-heart-burst,
  .room-lamp-glow {
    animation: none !important;
  }
}
</style>
