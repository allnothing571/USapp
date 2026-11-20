/**
 * P2-5a/c opaque device marker (docs/phase2-web-push.md Sections 7.1 and 16).
 *
 * The marker `{generationId, marker}` lives in IndexedDB (`usapp-push`, store `marker`, key
 * `marker`) so that the service worker can tell whether a push belongs to the generation registered
 * on this device. It carries no business text, user id, email or session token and authenticates
 * nothing.
 *
 * Across tabs and the service worker, each operation is ONE readwrite IndexedDB transaction, so
 * nothing can run between its read and its write:
 * - `clearMarker` increments a persisted fence and deletes the marker;
 * - `clearMarkerFor` deletes only while the marker is that generation's, and remembers the
 *   generation, so a delayed write of a revoked generation can no longer restore it;
 * - `writeMarker` applies only while the fence is still the one captured at the action's start, the
 *   generation is not remembered as cleared, and the stored marker is not of a later activation.
 *
 * Within one tab: every operation runs on one serialized chain, in call order, and an in-memory
 * epoch moves synchronously at `clearMarker`, before anything is awaited.
 *
 * `captureMarker` is synchronous, so a user gesture reaches the permission prompt with nothing awaited
 * first. The fence read it hands back was started earlier by `primeMarkerFence` (at load and at every
 * list refresh), so it is older than the action: a clear in between skips the write, the safe side.
 *
 * Limit, stated: if a clear lands in the moment between `primeMarkerFence` starting and its read running
 * (milliseconds, at load), that read sees the new fence. The server remains the authority: a logout
 * ends the session, so its generation is inactive and receives nothing.
 */

export interface MarkerRecord {
  generationId: string
  marker: string
  /** Orders two generations written by different tabs; absent only in older records. */
  activatedAt?: string
}

export interface MarkerBackend {
  fence(): Promise<number>
  /** Atomic. Resolves true when written. */
  putIfFence(record: MarkerRecord, fence: number): Promise<boolean>
  /** Atomic: fence + 1 and no marker. */
  clearAll(): Promise<void>
  /** Atomic: removes the marker only if it is this generation's, and remembers the generation. */
  compareDelete(generationId: string): Promise<void>
  get(): Promise<MarkerRecord | null>
}

/** What an action holds from its start until its write. */
export interface MarkerCapture {
  epoch: number
  fence: Promise<number>
}

const DB_NAME = 'usapp-push'
const STORE = 'marker'
const KEY = 'marker'
const FENCE_KEY = 'fence'
const CLEARED_KEY = 'cleared'
const MAX_CLEARED = 16

function openDb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, 1)
    req.onupgradeneeded = () => { req.result.createObjectStore(STORE) }
    req.onsuccess = () => resolve(req.result)
    req.onerror = () => reject(req.error)
  })
}

/**
 * One transaction. `body` runs inside it and must only issue requests from their callbacks: an
 * awaited promise would let the transaction commit early, so this is callback-style on purpose.
 */
async function transact<T>(mode: IDBTransactionMode, body: (store: IDBObjectStore, finish: (value: T) => void) => void): Promise<T> {
  const db = await openDb()
  try {
    return await new Promise<T>((resolve, reject) => {
      const tx = db.transaction(STORE, mode)
      let result: T
      tx.oncomplete = () => resolve(result)
      tx.onerror = () => reject(tx.error)
      tx.onabort = () => reject(tx.error)
      body(tx.objectStore(STORE), (value) => { result = value })
    })
  } finally {
    db.close()
  }
}

function isLater(a: string | undefined, b: string | undefined): boolean {
  if (!a || !b) return false
  return Date.parse(a) > Date.parse(b)
}

export const indexedDbBackend: MarkerBackend = {
  fence: () => transact<number>('readonly', (store, finish) => {
    const req = store.get(FENCE_KEY)
    req.onsuccess = () => finish(typeof req.result === 'number' ? req.result : 0)
  }),
  putIfFence: (record, fence) => transact<boolean>('readwrite', (store, finish) => {
    finish(false)
    const fenceReq = store.get(FENCE_KEY)
    const clearedReq = store.get(CLEARED_KEY)
    const currentReq = store.get(KEY)
    currentReq.onsuccess = () => {
      const stored = typeof fenceReq.result === 'number' ? fenceReq.result : 0
      const cleared = Array.isArray(clearedReq.result) ? clearedReq.result as string[] : []
      const current = currentReq.result as MarkerRecord | undefined
      if (stored !== fence || cleared.includes(record.generationId) || isLater(current?.activatedAt, record.activatedAt)) return
      store.put(record, KEY)
      finish(true)
    }
  }),
  clearAll: () => transact<void>('readwrite', (store, finish) => {
    finish(undefined)
    const req = store.get(FENCE_KEY)
    req.onsuccess = () => {
      store.put((typeof req.result === 'number' ? req.result : 0) + 1, FENCE_KEY)
      store.delete(KEY)
    }
  }),
  compareDelete: (generationId) => transact<void>('readwrite', (store, finish) => {
    finish(undefined)
    const clearedReq = store.get(CLEARED_KEY)
    const currentReq = store.get(KEY)
    currentReq.onsuccess = () => {
      const cleared = Array.isArray(clearedReq.result) ? clearedReq.result as string[] : []
      if (!cleared.includes(generationId)) store.put([...cleared, generationId].slice(-MAX_CLEARED), CLEARED_KEY)
      if ((currentReq.result as MarkerRecord | undefined)?.generationId === generationId) store.delete(KEY)
    }
  }),
  get: () => transact<MarkerRecord | null>('readonly', (store, finish) => {
    const req = store.get(KEY)
    req.onsuccess = () => finish((req.result as MarkerRecord | undefined) ?? null)
  }),
}

/** Without IndexedDB there is nothing to keep, so nothing can leak across identities. */
const noBackend: MarkerBackend = {
  fence: async () => 0, putIfFence: async () => false, clearAll: async () => {}, compareDelete: async () => {}, get: async () => null,
}

let backend: MarkerBackend = typeof indexedDB === 'undefined' ? noBackend : indexedDbBackend
let epoch = 0
let chain: Promise<unknown> = Promise.resolve()
let primed: Promise<number> | null = null

function enqueue<T>(op: () => Promise<T>): Promise<T> {
  const next = chain.then(op, op)
  chain = next.catch(() => undefined)
  return next
}

function readFence(): Promise<number> {
  const read = enqueue(() => backend.fence())
  // A failed read fails the later write, never an unhandled rejection here.
  read.catch(() => undefined)
  return read
}

/** Starts a fence read now, for the next `captureMarker`. */
export function primeMarkerFence(): void {
  primed = readFence()
}

/** Synchronous: never puts an await between a user gesture and the permission prompt. */
export function captureMarker(): MarkerCapture {
  return { epoch, fence: primed ?? readFence() }
}

/** Stores the marker unless a clear happened since the capture. Resolves true when written. */
export function writeMarker(record: MarkerRecord, capture: MarkerCapture): Promise<boolean> {
  return enqueue(async () => {
    if (capture.epoch !== epoch) return false
    return backend.putIfFence({ ...record }, await capture.fence)
  })
}

/** Identity/session clear: every write captured before it is now invalid, in any context. */
export function clearMarker(): Promise<void> {
  epoch += 1
  primed = null
  return enqueue(() => backend.clearAll())
}

/**
 * Generation-targeted clear (an explicit revoke, or a list showing that generation off): an old revoke
 * can never clear a newer generation's marker, and a newer write is never skipped.
 */
export function clearMarkerFor(generationId: string): Promise<void> {
  return enqueue(() => backend.compareDelete(generationId))
}

export function readMarker(): Promise<MarkerRecord | null> {
  return enqueue(() => backend.get())
}

/** Waits for every queued write and clear (tests and orderly shutdown). */
export function markerIdle(): Promise<void> {
  return enqueue(async () => {})
}

/** Tests only: replace the storage backend and reset coordination state. */
export function setMarkerBackendForTests(next: MarkerBackend | null): void {
  backend = next ?? (typeof indexedDB === 'undefined' ? noBackend : indexedDbBackend)
  epoch = 0
  chain = Promise.resolve()
  primed = null
}
