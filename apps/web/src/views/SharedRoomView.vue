<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { getAvatarAppearance, isAppearance, type CharacterGender } from '@/api/avatar'
import { emptyFigureSlots, originalGender, roomCharacter, type FigureSlots } from '@/api/avatar-art'
import { describeError, errorCodeOf } from '@/api/error-messages'
import { ApiError } from '@/api/client'
import { newIdempotencyKey } from '@/api/tasks'
import { buyRoomFurniture, connectSharedRoom, FREE_ROOM_ITEMS, getSharedRoom, oppositeSide, ROOM_ITEMS, saveSharedRoom, setRoomAvatarSharing, type Placement, type RoomItem, type RoomFurniture, type SharedRoom } from '@/api/shared-room'
import AvatarFigure from '@/components/AvatarFigure.vue'
import { conflictItems, mergeRoomLayout, ROOM_LAYOUT_PRESETS, sameLayout, type LayoutConflict, type RoomMergeChoices } from '@/shared-room-layout'
import { useRoomDraftStore } from '@/stores/room-draft'
import { useWalletStore } from '@/stores/wallet'
import { useSessionStore } from '@/stores/session'

const session = useSessionStore()
const originalSide = computed(() => roomCharacter(session.space, session.user?.id))
const roomDraft = useRoomDraftStore()
const wallet = useWalletStore()
const panel = ref<'layout' | 'shop' | null>(null)
function togglePanel(next: 'layout' | 'shop') {
  panel.value = panel.value === next ? null : next
  if (panel.value === 'shop') void wallet.load()
}
const room = ref<SharedRoom | null>(null)
const character = computed(() => room.value?.positions?.side ?? originalSide.value)
const ownGender = ref<CharacterGender | undefined>()
const ownBald = ref(false)
const ownSlots = ref<FigureSlots>(emptyFigureSlots())
const layout = ref<Placement[]>([])
const undoLayouts = ref<Placement[][]>([])
const redoLayouts = ref<Placement[][]>([])
const selection = ref<RoomItem | null>(null)
const busy = ref(false)
const purchase = ref<{ furniture: RoomFurniture; key: string; unconfirmed: boolean } | null>(null)
const furnitureChoices = computed<RoomItem[]>(() => [
  ...FREE_ROOM_ITEMS,
  ...(room.value?.furniture ?? []).filter(entry => entry.owned).map(entry => entry.item),
])
const notice = ref('')
const error = ref('')
const needsReload = ref(false)
const remoteUpdate = ref(false)
const syncUnavailable = ref(false)
const pendingMerge = ref<{ version: number; layout: Placement[]; items: RoomItem[] } | null>(null)
const mergeChoices = ref<RoomMergeChoices>({})
const cells = Array.from({ length: 12 }, (_, i) => ({ x: i % 4, y: Math.floor(i / 4) }))
const byCell = computed(() => new Map(layout.value.map(placement => [placement.x + ':' + placement.y, placement])))
const tiles = computed(() => cells.map(cell => ({ ...cell, item: byCell.value.get(cell.x + ':' + cell.y)?.item })))
const previewCell = computed(() => panel.value === 'shop' && purchase.value && layout.value.length < 4
  ? [...tiles.value].reverse().find(cell => !cell.item) : undefined)
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
  const placement = placements.find(entry => entry.item === item)
  return placement ? `第${placement.y + 1}排第${placement.x + 1}格` : '不摆放'
}

function describeConflicts(conflicts: LayoutConflict[]) {
  return conflicts.map(conflict => {
    if (conflict.kind === 'item') return `${ROOM_ITEMS[conflict.item]}有两种改法`
    if (conflict.kind === 'capacity') return '合并后的家具超过 4 件，请选择要保留的家具'
    const { first, second } = conflict
    return `第${first.y + 1}排第${first.x + 1}格同时放了${ROOM_ITEMS[first.item]}和${ROOM_ITEMS[second.item]}`
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
  showPresence.value = false
  purchase.value = null
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
  selection.value = item
}

function place(x: number, y: number) {
  const occupant = byCell.value.get(x + ':' + y)
  if (!selection.value || occupant) {
    if (occupant) selection.value = occupant.item
    return
  }
  const item = selection.value
  if (layout.value.length >= 4 && !layout.value.some(entry => entry.item === item)) {
    error.value = '房间最多可摆放 4 件家具。请先移除一件。'
    return
  }
  error.value = ''
  editDraft([...layout.value.filter(placement => placement.item !== item), { item, x, y }])
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
  <section class="shared-room" aria-labelledby="room-title">
    <header class="room-heading scene-page-heading">
      <RouterLink class="scene-back" :to="{ name: 'home' }" aria-label="返回首页">‹</RouterLink>
      <h1 id="room-title">我们的小房间</h1>
      <details class="room-settings">
        <summary>设置</summary>
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
    <button v-if="room && (remoteUpdate || needsReload || syncUnavailable) && !pendingMerge" type="button" class="btn btn--soft btn--small room-sync-action" :disabled="busy" @click="load(dirty)">同步最新布局</button>
    <p v-if="!room && busy" role="status">正在读取房间…</p>
    <template v-if="room">
      <p v-if="room.positions?.pending?.incoming" class="hint" role="status">
        对方发来了换位申请。<RouterLink :to="{ name: 'avatar-wardrobe', hash: '#room-position-title' }">去衣柜查看</RouterLink>
      </p>
      <section v-if="pendingMerge" class="room-resolution" aria-labelledby="room-resolution-title">
        <h2 id="room-resolution-title">选择要保留的布置</h2>
        <fieldset v-for="item in resolutionItems" :key="item" :disabled="busy">
          <legend>{{ ROOM_ITEMS[item] }}</legend>
          <div class="room-resolution-options">
            <button type="button" class="btn btn--soft btn--small" :aria-pressed="mergeChoices[item] === 'draft'"
              @click="mergeChoices[item] = 'draft'">我的布置 · {{ locationOf(layout, item) }}</button>
            <button type="button" class="btn btn--soft btn--small" :aria-pressed="mergeChoices[item] === 'latest'"
              @click="mergeChoices[item] = 'latest'">共享布局 · {{ locationOf(pendingMerge.layout, item) }}</button>
          </div>
        </fieldset>
        <p v-if="remainingConflicts" class="hint" role="status">{{ remainingConflicts }}。</p>
        <button type="button" class="btn btn--primary btn--small" :disabled="busy || !resolution?.layout"
          @click="applyResolution">应用到草稿</button>
      </section>
      <section class="room-scene" :class="{ 'room-scene--editing': panel === 'layout' }" aria-label="共享房间">
        <p v-if="liveReady && peerOnline" class="room-presence" role="status" aria-label="对方在房间">同在<span class="room-presence-label"> · 对方在房间</span></p>
        <div class="room-wall" :class="`room-wall--self-${character}`">
          <AvatarFigure class="room-figure-self" :slots="ownSlots" label="我的形象" :character="character"
            :gender="ownGender ?? originalGender(originalSide)" :bald="ownBald" />
          <AvatarFigure v-if="room.partner_appearance" :slots="room.partner_appearance.slots" label="对方的形象"
            class="room-figure-partner" :character="oppositeSide(character)"
            :gender="room.partner_appearance.character_gender ?? originalGender(oppositeSide(originalSide))"
            :bald="room.partner_appearance.hair_hidden" />
        </div>
        <div class="room-floor" aria-label="家具位置">
          <button v-for="cell in tiles" :key="cell.x + ':' + cell.y" type="button" class="room-cell"
            :disabled="busy || needsReload"
            :tabindex="panel === 'layout' ? 0 : -1"
            :aria-label="'第' + (cell.y + 1) + '排第' + (cell.x + 1) + '格 ' + (cell.item ? ROOM_ITEMS[cell.item] : '空位')"
            @click="panel === 'layout' && place(cell.x, cell.y)">
            <template v-if="cell.item">
              <span class="room-art room-floor-art" :data-item="cell.item" aria-hidden="true"></span>
              <span class="room-cell-label">{{ ROOM_ITEMS[cell.item] }}</span>
            </template>
            <span v-else-if="previewCell?.x === cell.x && previewCell?.y === cell.y && purchase"
              class="room-art room-floor-art room-preview" :data-item="purchase.furniture.item" aria-hidden="true"></span>
          </button>
        </div>
      </section>

      <nav class="room-dock" aria-label="小房间功能">
        <button class="room-dock-action" type="button" :aria-expanded="panel === 'layout'" aria-controls="room-layout-panel" @click="togglePanel('layout')">
          <span class="room-art" data-item="armchair" aria-hidden="true"></span>布置
        </button>
        <button v-if="room.furniture?.length" class="room-dock-action" type="button" :aria-expanded="panel === 'shop'" aria-controls="room-shop-panel" @click="togglePanel('shop')">
          <span class="room-art" data-item="shop" aria-hidden="true"></span>商店
        </button>
        <RouterLink class="room-dock-action" :to="{ name: 'shared-garden' }">
          <span class="room-art" data-item="garden" aria-hidden="true"></span>花园
        </RouterLink>
      </nav>
      <section v-show="panel === 'layout'" id="room-layout-panel" class="room-controls" aria-label="布置家具">
        <p>选择家具，再点一个空位摆放或移动。</p>
        <div class="room-tools">
          <button v-for="item in furnitureChoices" :key="item" type="button" class="btn btn--soft btn--small"
            :aria-pressed="selection === item" :disabled="busy || needsReload" @click="selectFurniture(item)"><span class="room-art" :data-item="item" aria-hidden="true"></span>{{ ROOM_ITEMS[item] }}</button>
          <button type="button" class="btn btn--ghost btn--small"
            :disabled="busy || needsReload || !layout.some(item => item.item === selection)"
            @click="editDraft(layout.filter(item => item.item !== selection))">移除选中家具</button>
          <div class="room-draft-history" role="group" aria-label="草稿操作">
            <button type="button" class="btn btn--ghost btn--small" :disabled="!canEdit || liveEditing || !undoLayouts.length"
              @click="stepDraft(undoLayouts, redoLayouts, '已撤销一步。')">撤销</button>
            <button type="button" class="btn btn--ghost btn--small" :disabled="!canEdit || liveEditing || !redoLayouts.length"
              @click="stepDraft(redoLayouts, undoLayouts, '已重做一步。')">重做</button>
          </div>
        </div>
      </section>
      <section v-if="room.furniture?.length" v-show="panel === 'shop'" id="room-shop-panel" class="room-controls room-shop" aria-labelledby="room-shop-title">
          <div class="room-shop-heading">
            <h2 id="room-shop-title">家具商店</h2>
            <span v-if="wallet.balances">我的阳光 {{ wallet.balances.sunshine }}</span>
            <button v-else-if="wallet.status === 'error'" type="button" class="btn btn--small btn--ghost" @click="wallet.load()">重试读取阳光</button>
            <span v-else>正在读取阳光…</span>
          </div>
          <div class="room-tools">
            <button v-for="entry in room.furniture" :key="entry.item" type="button" class="btn btn--soft btn--small"
              :aria-pressed="purchase?.furniture.item === entry.item"
              :disabled="entry.owned || busy || !!purchase" @click="selectPurchase(entry)">
              <span class="room-art" :data-item="entry.item" aria-hidden="true"></span>{{ ROOM_ITEMS[entry.item] }} · {{ entry.owned ? '空间已拥有' : `${entry.price} 阳光` }}
            </button>
          </div>
          <div v-if="purchase">
            <p>用我的 {{ purchase.furniture.price }} 阳光购买{{ ROOM_ITEMS[purchase.furniture.item] }}，两人均可摆放。</p>
            <p v-if="previewCell" class="hint">房间里的半透明家具是试摆预览。</p>
            <p v-if="purchase.unconfirmed" role="status">结果待确认。重试会使用同一笔购买请求。</p>
            <button type="button" class="btn btn--primary btn--small" :disabled="busy" @click="confirmPurchase">
              {{ purchase.unconfirmed ? '确认购买结果' : '确认购买' }}
            </button>
            <button v-if="!purchase.unconfirmed" type="button" class="btn btn--ghost btn--small" :disabled="busy" @click="purchase = null">取消</button>
          </div>
      </section>
      <section v-show="panel === 'layout'" class="room-controls room-layout-actions" aria-label="保存布置">
        <details class="room-templates">
          <summary>布置模板</summary>
          <p class="hint">套用后会替换当前草稿，可撤销。</p>
          <div class="room-template-list">
            <button v-for="preset in ROOM_LAYOUT_PRESETS" :key="preset.name" type="button"
              class="btn btn--soft btn--small room-template"
              :disabled="!canEdit || sameLayout(layout, preset.layout)"
              :aria-label="presetLabel(preset)"
              @click="applyPreset(preset)">
              <span class="room-template-board" aria-hidden="true">
                <span v-for="placement in preset.layout" :key="placement.item" class="room-art"
                  :data-item="placement.item" :style="{ gridColumn: placement.x + 1, gridRow: placement.y + 1 }"></span>
              </span>
              <span>套用{{ preset.name }}</span>
            </button>
          </div>
        </details>
        <div class="room-actions">
          <button type="button" class="btn btn--primary" :disabled="!canSave" @click="save">保存布局</button>
        </div>
      </section>
      <p v-if="notice && !error && !remoteUpdate && !syncUnavailable" role="status" class="room-notice">{{ notice }}</p>
    </template>
  </section>
</template>

<style scoped>
.shared-room { margin: 0 auto; max-width: 32rem; }
.room-heading { position: relative; z-index: 3; }
.room-settings > summary { display: grid; place-items: center; min-height: 44px; border: 1px solid var(--line); border-radius: var(--radius-sm); background: var(--surface); color: var(--ink); text-decoration: none; cursor: pointer; }
.room-settings summary { list-style: none; }
.room-settings summary::-webkit-details-marker { display: none; }
.room-refresh { position: absolute; top: calc(100% + .45rem); right: 0; z-index: 1; width: min(100%, 19rem); display: grid; gap: .6rem; padding: .9rem; background: var(--surface); border: 1px solid var(--line); border-radius: var(--radius); box-shadow: var(--shadow); }
.room-preferences { display: grid; gap: .6rem; }
.room-preferences label { display: flex; align-items: center; gap: .4rem; }
.room-preferences input { accent-color: var(--green); }
.room-scene { position: relative; isolation: isolate; aspect-ratio: 1.04; background: #e4ad6f url('/room-art/sunlit-room.jpg') center / 100% 100% no-repeat; border: 1px solid #c58d5b; border-radius: 1.3rem; overflow: hidden; }
.room-presence { position: absolute; z-index: 3; inset: .7rem .7rem auto auto; margin: 0; padding: .25rem .7rem; border-radius: 2rem; background: var(--surface); color: var(--green); font-size: .82rem; font-weight: 650; }
.room-presence::before { content: ''; display: inline-block; width: .5rem; height: .5rem; margin-right: .32rem; border-radius: 50%; background: var(--green); }
.room-presence-label { position: absolute; width: 1px; height: 1px; overflow: hidden; clip-path: inset(50%); white-space: nowrap; }
.room-wall { position: absolute; inset: auto 21% 35%; display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); align-items: end; justify-items: center; gap: 1rem; pointer-events: none; }
.room-wall :deep(.avatar-figure) { grid-row: 1; }
.room-wall--self-left :deep(.room-figure-self), .room-wall--self-right :deep(.room-figure-partner) { grid-column: 1; }
.room-wall--self-right :deep(.room-figure-self), .room-wall--self-left :deep(.room-figure-partner) { grid-column: 2; }
.room-wall :deep(.avatar-figure) { width: clamp(4.5rem, 23vw, 7.8rem); margin: 0; }
.room-wall :deep(.avatar-canvas) { background: transparent; border-radius: 0; }
.room-wall :deep(figcaption) { position: absolute; width: 1px; height: 1px; overflow: hidden; clip-path: inset(50%); }
.room-wall :deep(.hint) { padding: .25rem; background: var(--surface); font-size: .7rem; }
.room-floor { position: absolute; inset: 61% 7% 5%; display: grid; grid-template: repeat(3, 1fr) / repeat(4, minmax(0, 1fr)); gap: .2rem; }
.room-cell { position: relative; display: grid; align-items: end; justify-items: center; min-width: 0; min-height: 0; padding: 0; background: transparent; border: 1px solid transparent; border-radius: .35rem; color: var(--ink); pointer-events: none; }
.room-scene--editing .room-cell { pointer-events: auto; border: 1px dashed #704122; background: #fff0c846; cursor: pointer; }
.room-scene--editing .room-cell:not(:disabled):hover,
.room-scene--editing .room-cell:focus-visible { background: #fff5d699; outline: 2px solid var(--green); outline-offset: -2px; }
.room-floor-art { position: absolute; bottom: 2px; width: 97%; pointer-events: none; }
.room-floor-art[data-item="rug"] { width: 112%; bottom: -16%; }
.room-floor-art[data-item="vase"] { width: 58%; }
.room-floor-art[data-item="lamp"] { width: 120%; }
.room-preview { opacity: .58; filter: drop-shadow(0 0 4px #fff8dc); }
.room-cell-label { display: none; position: relative; z-index: 1; border-radius: 1rem; padding-inline: .25rem; font-size: .65rem; line-height: 1.35; background: var(--surface); pointer-events: none; white-space: nowrap; }
.room-scene--editing .room-cell-label { display: inline; }
.room-dock { display: flex; gap: .6rem; margin: .75rem 0; }
.room-dock-action { flex: 1 1 0; display: grid; justify-items: center; min-height: 54px; padding: .28rem .25rem .38rem; border: 1px solid var(--line); border-radius: var(--radius); background: var(--surface-soft); color: var(--ink); text-decoration: none; font: inherit; font-size: .9rem; font-weight: 650; cursor: pointer; }
.room-dock-action[aria-expanded="true"] { background: var(--honey); border-color: #b1854e; }
.room-dock-action .room-art { width: 2.6rem; }
.room-controls { margin-top: .65rem; padding: .8rem; border: 1px solid var(--line); border-radius: var(--radius); background: var(--surface); }
.room-controls p { margin-bottom: .55rem; }
.room-controls .room-art { width: 1.75rem; }
.room-tools, .room-actions, .room-draft-history, .room-shop-heading { display: flex; flex-wrap: wrap; align-items: center; gap: .45rem; }
.room-shop-heading { justify-content: space-between; margin-bottom: .65rem; }
.room-shop-heading h2 { margin: 0; font-size: 1rem; }
.room-shop-heading span { color: var(--accent-ink); font-size: .84rem; }
.room-shop .room-tools [aria-pressed="true"] { opacity: 1; }
.room-shop .room-tools + div { margin-top: .85rem; }
.room-actions { margin-top: .65rem; justify-content: end; }
.room-tools [aria-pressed="true"] { outline: 2px solid var(--green); }
.room-resolution { margin-bottom: 1rem; padding: .85rem; background: var(--surface); border: 1px solid var(--line); border-radius: var(--radius); }
.room-resolution h2 { margin: 0 0 .65rem; font-size: 1.05rem; }
.room-resolution fieldset { min-width: 0; margin: 0 0 .55rem; padding: .45rem .55rem .65rem; border: 1px solid var(--line); border-radius: .6rem; }
.room-resolution-options { display: flex; flex-wrap: wrap; gap: .45rem; }
.room-resolution-options button { white-space: normal; }
.room-resolution-options [aria-pressed="true"] { outline: 2px solid var(--green); outline-offset: -2px; }
.room-templates summary { cursor: pointer; width: fit-content; min-height: 40px; }
.room-template-list { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 6rem), 1fr)); gap: .55rem; }
.room-template { display: grid; gap: .3rem; min-width: 0; text-align: center; white-space: normal; }
.room-template-board { display: grid; grid-template: repeat(3, 1rem) / repeat(4, minmax(0, 1fr)); place-items: center; }
.room-template-board .room-art { width: 1rem; }
.room-sync-action { margin-bottom: .7rem; }
.room-notice { margin: .55rem 0; color: var(--green); font-size: .88rem; }
@media (max-width: 480px) {
  .room-scene { aspect-ratio: .87; }
}
</style>
