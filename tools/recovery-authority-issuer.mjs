import { createPrivateKey, sign } from 'node:crypto'
import { readFile } from 'node:fs/promises'
import { resolve } from 'node:path'
import { pathToFileURL } from 'node:url'

const uuid = /^[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}(?![\s\S])/u
const digest = /^[0-9a-f]{64}(?![\s\S])/u
const targetFields = ['databaseId', 'attemptId', 'journalId', 'instanceToken', 'archiveSha256']
const observationMaximumAge = 30
const maximumLifetime = 300

function exactKeys(value, fields) {
  return value !== null && typeof value === 'object' && !Array.isArray(value) &&
    Object.keys(value).length === fields.length && fields.every(field => Object.hasOwn(value, field))
}

function matchesPattern(value, pattern) {
  return typeof value === 'string' && pattern.test(value)
}

function validTarget(value) {
  return exactKeys(value, targetFields) &&
    targetFields.slice(0, 3).every(field => matchesPattern(value[field], uuid)) &&
    targetFields.slice(3).every(field => matchesPattern(value[field], digest))
}

function sequence(value) {
  if (Number.isSafeInteger(value) && value >= 0) return String(value)
  if (matchesPattern(value, /^(?:0|[1-9][0-9]{0,18})(?![\s\S])/u) &&
      BigInt(value) <= 9223372036854775807n) return value
  return null
}

// The signing host owns the state. The restore request supplies no checkpoint or quiet-window facts.
export function issue(state, request, privateKey, now = Math.floor(Date.now() / 1000)) {
  if (!exactKeys(state, ['target', 'checkpoint', 'observedAt', 'writersPausedUntil']) ||
      !validTarget(state.target) || !validTarget(request) ||
      targetFields.some(field => request[field] !== state.target[field])) {
    throw new Error('RESTORE_TARGET_REFUSED')
  }
  const { checkpoint, observedAt, writersPausedUntil } = state
  const seq = sequence(checkpoint?.seq)
  if (!exactKeys(checkpoint, ['journalId', 'seq', 'hash']) || checkpoint.journalId !== state.target.journalId ||
      seq === null || !matchesPattern(checkpoint.hash, digest) ||
      (seq === '0') !== (checkpoint.hash === '0'.repeat(64))) {
    throw new Error('CHECKPOINT_REFUSED')
  }
  if (!Number.isSafeInteger(now) || !Number.isSafeInteger(observedAt) ||
      !Number.isSafeInteger(writersPausedUntil) || observedAt <= 0 || observedAt > now ||
      now - observedAt > observationMaximumAge || writersPausedUntil <= now) {
    throw new Error('CHECKPOINT_WINDOW_REFUSED')
  }
  if (privateKey?.type !== 'private' || privateKey.asymmetricKeyType !== 'ed25519') {
    throw new Error('SIGNING_KEY_REFUSED')
  }
  const expiresAt = now + Math.min(maximumLifetime, writersPausedUntil - now)
  const body = ['usapp-recovery-attestation-v1', ...targetFields.map(field => state.target[field]),
    seq, checkpoint.hash, now, expiresAt, ''].join('\n')
  return { issuedAt: now, expiresAt, signature: sign(null, Buffer.from(body, 'ascii'), privateKey).toString('hex') }
}

// The coordinator adapters return committed observations from the enrolled authority.
export async function issueAfterPause(target, request, privateKey, {
  confirmPause, observeCheckpoint, clock = () => Math.floor(Date.now() / 1000),
}) {
  if (!validTarget(target) || !validTarget(request) ||
      targetFields.some(field => request[field] !== target[field])) throw new Error('RESTORE_TARGET_REFUSED')

  const pauseConfirmed = async () => {
    const observed = await confirmPause(target.journalId, target.attemptId)
    if (!exactKeys(observed, ['journalId', 'pauseId', 'state']) ||
        observed.journalId !== target.journalId || observed.pauseId !== target.attemptId ||
        observed.state !== 'paused') throw new Error('PUBLICATION_PAUSE_UNCONFIRMED')
  }
  await pauseConfirmed()
  const observation = await observeCheckpoint(target.journalId)
  if (!exactKeys(observation, ['checkpoint', 'observedAt', 'writersPausedUntil']))
    throw new Error('CHECKPOINT_REFUSED')
  await pauseConfirmed()
  return issue({ target, ...observation }, request, privateKey, clock())
}

async function main() {
  const [, , statePath, requestPath, keyPath, ...extra] = process.argv
  if (!statePath || !requestPath || !keyPath || extra.length) throw new Error('ISSUER_ARGUMENTS_REFUSED')
  const state = JSON.parse(await readFile(statePath, 'utf8'))
  const request = JSON.parse(await readFile(requestPath, 'utf8'))
  const privateKey = createPrivateKey(await readFile(keyPath))
  process.stdout.write(JSON.stringify(issue(state, request, privateKey)) + '\n')
}

if (process.argv[1] && pathToFileURL(resolve(process.argv[1])).href === import.meta.url) {
  main().catch(error => {
    const message = ['RESTORE_TARGET_REFUSED', 'CHECKPOINT_REFUSED', 'CHECKPOINT_WINDOW_REFUSED', 'SIGNING_KEY_REFUSED',
      'ISSUER_ARGUMENTS_REFUSED'].includes(error.message) ? error.message : 'RECOVERY_ISSUER_FAILED'
    process.stderr.write(message + '\n')
    process.exitCode = 2
  })
}
