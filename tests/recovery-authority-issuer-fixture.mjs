// Separate synthetic authority process. No real enrolled key or provider access.
import { generateKeyPairSync } from 'node:crypto'
import { execFileSync } from 'node:child_process'
import { setTimeout as delay } from 'node:timers/promises'
import { performance } from 'node:perf_hooks'
import { issueAfterPause } from '../tools/recovery-authority-issuer.mjs'

const [, , instanceToken, archiveSha256, coordinator, runtime, directory] = process.argv
if (![coordinator, runtime].every(id => /^[a-f0-9]{64}$/u.test(id ?? '')) ||
    !/^\/tmp\/usapp-journal-service-usapp-(e1|backup-probe)-[a-f0-9]{10}(-[a-f0-9]{10})?$/u.test(directory ?? '')) {
  throw new Error('invalid disposable fixture reference')
}
const target = {
  databaseId: '00000000-0000-0000-0000-00000000002d',
  attemptId: '00000000-0000-0000-0000-00000000002e',
  journalId: '00000000-0000-0000-0000-00000000002c',
  instanceToken,
  archiveSha256,
}
const run = (...args) => execFileSync('docker', ['exec', ...args], { encoding: 'utf8', timeout: 5000 }).trim()
const sources = {
  confirmPause(journalId, pauseId) {
    const statement = `SELECT recovery_fence.confirm_pause('${journalId}','${pauseId}');`
    const state = run(coordinator, 'psql', '-XqAt', '-v', 'ON_ERROR_STOP=1', '-U', 'fence_controller',
      '-d', 'usapp_recovery_fence', '-c', statement)
    return { journalId, pauseId, state }
  },
  async observeCheckpoint() {
    run(runtime, 'touch', directory + '/observe-paused-checkpoint')
    const deadline = performance.now() + 5000
    do {
      const payload = run(runtime, 'python3', '-B', '-c',
        'import sys; from pathlib import Path; p=Path(sys.argv[1]); print(p.read_text() if p.exists() else "null")',
        directory + '/pause-observation.json')
      const observation = JSON.parse(payload)
      if (observation) return observation
      await delay(100)
    } while (performance.now() < deadline)
    throw new Error('checkpoint observation unavailable')
  },
}
const { publicKey, privateKey } = generateKeyPairSync('ed25519')
const enrolledKey = Buffer.from(publicKey.export({ format: 'jwk' }).x, 'base64url').toString('hex')
const proof = await issueAfterPause(target, target, privateKey, sources)
process.stdout.write(JSON.stringify({ enrolledKey, ...proof }) + '\n')
