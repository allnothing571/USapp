import { defineStore } from 'pinia'
import type { Placement, SharedRoom } from '@/api/shared-room'

interface RoomDraft {
  base: Pick<SharedRoom, 'version' | 'layout'>
  layout: Placement[]
  undoLayouts: Placement[][]
  redoLayouts: Placement[][]
}

export const useRoomDraftStore = defineStore('room-draft', () => {
  // Retain only layout data for same-tab navigation. Nothing is persisted.
  let saved: { user: string; space: string; draft: RoomDraft } | null = null

  function remember(user: string, space: string, draft: RoomDraft) {
    saved = { user, space, draft }
  }

  function read(user: string, space: string) {
    if (saved && (saved.user !== user || saved.space !== space)) reset()
    return saved?.draft ?? null
  }

  function reset() {
    saved = null
  }

  return { remember, read, reset }
})
