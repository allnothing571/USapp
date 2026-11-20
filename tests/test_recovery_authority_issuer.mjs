import { generateKeyPairSync, verify } from 'node:crypto'
import { deepEqual, equal, rejects, throws } from 'node:assert/strict'
import { test } from 'node:test'
import { spawnSync } from 'node:child_process'
import { mkdtemp, rmdir, unlink, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { issue, issueAfterPause } from '../tools/recovery-authority-issuer.mjs'

const id = n => '00000000-0000-0000-0000-' + String(n).padStart(12, '0')
const target = {
  databaseId: id(2), attemptId: id(3), journalId: id(1),
  instanceToken: '1'.repeat(64), archiveSha256: '2'.repeat(64),
}
const checkpoint = { journalId: id(1), seq: 8, hash: 'a'.repeat(64) }
const state = { target, checkpoint, observedAt: 100, writersPausedUntil: 120 }
const { privateKey, publicKey } = generateKeyPairSync('ed25519')

test('signature covers registered binding and is capped to the quiet window', () => {
  const proof = issue(state, target, privateKey, 110)
  deepEqual(Object.keys(proof), ['issuedAt', 'expiresAt', 'signature'])
  equal(proof.expiresAt, 120)
  const body = Buffer.from(['usapp-recovery-attestation-v1', id(2), id(3), id(1), '1'.repeat(64),
    '2'.repeat(64), 8, 'a'.repeat(64), 110, 120, ''].join('\n'), 'ascii')
  equal(verify(null, body, publicKey, Buffer.from(proof.signature, 'hex')), true)
  equal(issue({ ...state, writersPausedUntil: 500 }, target, privateKey, 110).expiresAt, 410)
})

test('target mismatches and caller-supplied checkpoint or pause claims are refused', () => {
  for (const request of [{ ...target, attemptId: id(9) }, { ...target, instanceToken: '3'.repeat(64) },
    { ...target, archiveSha256: '3'.repeat(64) }, { ...target, checkpoint }, { ...target, writersPausedUntil: 1000 },
    { ...target, archiveSha256: target.archiveSha256 + '\n' }]) {
    throws(() => issue(state, request, privateKey, 110), /RESTORE_TARGET_REFUSED/)
  }
})

test('missing pause, old or future observation, and invalid checkpoint are refused', () => {
  for (const update of [{ writersPausedUntil: 110 }, { observedAt: 79 }, { observedAt: 111 }]) {
    throws(() => issue({ ...state, ...update }, target, privateKey, 110), /CHECKPOINT_WINDOW_REFUSED/)
  }
  for (const update of [{ journalId: id(6) }, { seq: 0 }, { hash: checkpoint.hash + '\n' },
    { seq: Number.MAX_SAFE_INTEGER + 1 }, { seq: '9223372036854775808' }, { seq: '01' }]) {
    throws(() => issue({ ...state, checkpoint: { ...checkpoint, ...update } }, target, privateKey, 110), /CHECKPOINT_REFUSED/)
  }
  throws(() => issue(state, target, publicKey, 110), /SIGNING_KEY_REFUSED/)
  equal(issue({ ...state, checkpoint: { ...checkpoint, seq: '9223372036854775807' } },
    target, privateKey, 110).signature.length, 128)
})

test('offline CLI returns only a proof and uses a refusal exit on an unregistered target', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'usapp-recovery-issuer-test-'))
  const stateFile = join(directory, 'state.json')
  const requestFile = join(directory, 'request.json')
  const keyFile = join(directory, 'synthetic.pem')
  const issuer = fileURLToPath(new URL('../tools/recovery-authority-issuer.mjs', import.meta.url))
  const run = () => spawnSync(process.execPath, [issuer, stateFile, requestFile, keyFile], { encoding: 'utf8' })
  const now = Math.floor(Date.now() / 1000)
  try {
    await writeFile(stateFile, JSON.stringify({ ...state, observedAt: now, writersPausedUntil: now + 60 }))
    await writeFile(requestFile, JSON.stringify(target))
    await writeFile(keyFile, privateKey.export({ format: 'pem', type: 'pkcs8' }), { mode: 0o600 })
    const accepted = run()
    equal(accepted.status, 0, accepted.stderr)
    deepEqual(Object.keys(JSON.parse(accepted.stdout)), ['issuedAt', 'expiresAt', 'signature'])
    await writeFile(requestFile, JSON.stringify({ ...target, databaseId: id(9) }))
    const refused = run()
    equal(refused.status, 2)
    equal(refused.stdout, '')
    equal(refused.stderr.trim(), 'RESTORE_TARGET_REFUSED')
  } finally {
    for (const path of [keyFile, requestFile, stateFile]) {
      try { await unlink(path) } catch (error) { if (error.code !== 'ENOENT') throw error }
    }
    await rmdir(directory)
  }
})

test('coordinated issuance observes the checkpoint between two matching pause confirmations', async () => {
  const events = []
  const proof = await issueAfterPause(target, target, privateKey, {
    confirmPause(journalId, pauseId) {
      events.push('pause')
      equal(journalId, target.journalId)
      equal(pauseId, target.attemptId)
      return { journalId, pauseId, state: 'paused' }
    },
    observeCheckpoint(journalId) {
      events.push('checkpoint')
      equal(journalId, target.journalId)
      return { checkpoint, observedAt: 100, writersPausedUntil: 120 }
    },
    clock: () => 110,
  })
  deepEqual(events, ['pause', 'checkpoint', 'pause'])
  deepEqual(proof, issue(state, target, privateKey, 110))
})

test('coordinated issuance blocks an absent, wrong or changed pause', async () => {
  const paused = { journalId: target.journalId, pauseId: target.attemptId, state: 'paused' }
  for (const responses of [[null, paused], [{ ...paused, pauseId: id(6) }, paused],
    [paused, { ...paused, state: 'draining' }], [paused, { ...paused, journalId: id(6) }]]) {
    let observations = 0
    let confirmations = 0
    await rejects(issueAfterPause(target, target, privateKey, {
      confirmPause: () => responses[confirmations++],
      observeCheckpoint() {
        observations++
        return { checkpoint, observedAt: 100, writersPausedUntil: 120 }
      },
      clock: () => 110,
    }), /PUBLICATION_PAUSE_UNCONFIRMED/)
    equal(observations, responses[0] === paused ? 1 : 0)
  }
})

test('coordinated issuance refuses an observation that expired during confirmation', async () => {
  const paused = { journalId: target.journalId, pauseId: target.attemptId, state: 'paused' }
  await rejects(issueAfterPause(target, target, privateKey, {
    confirmPause: () => paused,
    observeCheckpoint: () => ({ checkpoint, observedAt: 100, writersPausedUntil: 120 }),
    clock: () => 120,
  }), /CHECKPOINT_WINDOW_REFUSED/)
})
