// Throwaway fake API + Vite browser rehearsal. Loopback only; no real credentials, DB, providers or enrollment.
import { readFile } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'
import path from 'node:path'
const root = fileURLToPath(new URL('../', import.meta.url))
const { createServer } = await import(new URL('../apps/web/node_modules/vite/dist/node/index.js', import.meta.url))
const manifest = JSON.parse(await readFile(path.join(root, 'apps/web/src/assets/avatar-manifest.json'), 'utf8'))
const uuid = n => `10000000-0000-4000-8000-${String(n).padStart(12, '0')}`
const slots = ['hair', 'clothes', 'expression', 'hat', 'handheld', 'accessory']
const names = ['栗色短发', '暖棕卷发', '奶油上衣', '青绿上衣', '微笑', '眨眼', '珊瑚帽', '小花', '蝴蝶结']
const items = Object.entries(manifest.assets).map(([key, a], n) => ({ id: uuid(n + 1), category: a.category,
  starter: ['hair.placeholder_bob.v1', 'clothes.placeholder_cream.v1', 'expression.placeholder_smile.v1'].includes(key),
  version: 1, name: names[n], price_sunshine: null,
  asset: { key, format: a.format, layers: Object.keys(a.layers), covers_hair_top: a.covers_hair_top, accessory_position: a.accessory_position }, listed: true }))
items.forEach(i => { i.price_sunshine = i.starter ? null : 20 })
const defaults = Object.fromEntries(slots.map(s => [s, items.find(i => i.category === s && i.starter)?.id ?? null]))
const users = [0, 1].map(n => ({ id: uuid(500 + n), email: `avatar-b4-${n}@example.test`, display_name: n ? '合成账号乙' : '合成账号甲', created_at: '2026-11-10T00:00:00Z' }))
const data = users.map(() => ({ balance: 100, purchases: [], events: [], purchaseKeys: new Map(), saveKeys: new Map(), lostPurchase: false, lostSave: false }))
let who = 0, emptyCatalog = false, calls = []
function appearance() {
  const saved = data[who].events.at(-1)
  return { source: saved ? 'saved_loadout' : 'starter_defaults', complete: true, starter_version: saved ? saved.starter_version : 1,
    loadout_version: saved?.version ?? 0, slots: Object.fromEntries(slots.map(s => {
      const id = (saved?.slots ?? defaults)[s]; return [s, id ? items.find(i => i.id === id) : null]
    })) }
}
function respond(res, body, status = 200) { res.statusCode = status; res.setHeader('Cache-Control', 'no-store'); res.setHeader('Content-Type', 'application/json'); res.end(JSON.stringify(body)) }
function fail(res, code, status = 409) { respond(res, { error: { code, message: code, details: {} } }, status) }
const plugin = { name: 'avatar-b4-synthetic-only', configureServer(server) {
  server.middlewares.use(async (req, res, next) => {
    const url = new URL(req.url, 'http://127.0.0.1:3764'), p = url.pathname
    if (p === '/__b4') {
      if (url.searchParams.has('user')) who = url.searchParams.get('user') === 'b' ? 1 : 0
      if (url.searchParams.has('empty')) emptyCatalog = url.searchParams.get('empty') === 'true'
      res.setHeader('Content-Type', 'text/html; charset=utf-8')
      res.end('<h1>B4 synthetic-only controls</h1><p>No real database, credentials or money.</p><a href="/avatar">Shop</a> | <a href="/avatar/wardrobe">Wardrobe</a> | <a href="/home">Home</a><p><a href="/__b4?user=a">Account A</a> | <a href="/__b4?user=b">Account B</a> | <a href="/__b4?empty=true">Empty catalog</a> | <a href="/__b4?empty=false">Normal catalog</a></p><p><a href="/__b4/frame?width=375">375 px shop</a> | <a href="/__b4/frame?width=375&page=wardrobe">375 px wardrobe</a> | <a href="/__b4/frame?width=1100">1100 px shop</a></p>'); return
    }
    if (p === '/__b4/frame') {
      const width = url.searchParams.get('width') === '1100' ? 1100 : 375
      const page = url.searchParams.get('page') === 'wardrobe' ? '/avatar/wardrobe' : '/avatar'
      res.setHeader('Content-Type', 'text/html; charset=utf-8')
      res.end(`<title>B4 ${width}px synthetic rehearsal</title><style>body{margin:12px;background:#ddd;font-family:sans-serif}iframe{border:0;width:${width}px;max-width:100%;height:calc(100vh - 110px);background:white}output{display:block;max-width:1100px;overflow-wrap:anywhere;font-size:13px}</style><a href="/__b4">Synthetic controls</a><p>${width}px browser viewport. No production services.</p><output id="metrics"></output><iframe title="B4 application" src="${page}"></iframe><script>setInterval(async()=>{const w=document.querySelector('iframe').contentWindow;try{document.getElementById('metrics').textContent=JSON.stringify({viewport:w.innerWidth,scrollWidth:w.document.documentElement.scrollWidth,bodyScroll:w.document.body.scrollWidth,localStorageKeys:Object.keys(w.localStorage),sessionStorageKeys:Object.keys(w.sessionStorage),cacheNames:await w.caches.keys()})}catch{}},1000)</script>`); return
    }
    if (!p.startsWith('/api/')) return next()
    let body = {}
    if (req.method === 'POST') { let text = ''; for await (const chunk of req) text += chunk; try { body = JSON.parse(text) } catch { return fail(res, 'VALIDATION_ERROR', 400) } }
    calls.push({ method: req.method, path: p, user: who })
    const u = users[who], d = data[who]
    if (p === '/api/v1/session') return respond(res, { authenticated: true, csrf_token: 'synthetic-csrf-not-a-real-secret', user: u,
      space: { id: uuid(600), name: '合成测试空间', timezone: 'Asia/Shanghai', member_count: 2, created_at: '2026-11-10T00:00:00Z' } })
    if (p === '/api/v1/me') return respond(res, { user: u })
    if (p === '/api/v1/wallet') return respond(res, { wallet: { flower: 7, sunshine: d.balance } })
    if (p === '/api/v1/avatar/catalog') return respond(res, { items: emptyCatalog ? [] : items.filter(i => i.listed) })
    if (p === '/api/v1/avatar') return respond(res, { appearance: appearance() })
    if (p === '/api/v1/avatar/owned') return respond(res, { items: d.purchases.map(p => ({ purchase: p, current_item: items.find(i => i.id === p.item_id) })), next_cursor: null })
    if (p === '/api/v1/avatar/purchases' && req.method === 'POST') {
      const key = req.headers['idempotency-key'], prior = d.purchaseKeys.get(key)
      if (prior) { if (prior.body !== JSON.stringify(body)) return fail(res, 'IDEMPOTENCY_KEY_REUSED'); return respond(res, { ...prior.response, replayed: true }) }
      const item = items.find(i => i.id === body.item_id)
      if (!item) return fail(res, 'AVATAR_ITEM_NOT_FOUND', 404)
      if (item.starter || !item.listed) return fail(res, 'AVATAR_ITEM_UNAVAILABLE')
      if (body.expected_version !== item.version) return fail(res, 'AVATAR_VERSION_CONFLICT')
      if (d.purchases.some(p => p.item_id === item.id)) return fail(res, 'AVATAR_ALREADY_OWNED')
      if (d.balance < item.price_sunshine) return fail(res, 'INSUFFICIENT_BALANCE')
      d.balance -= item.price_sunshine
      const purchase = { id: uuid(1000 + who * 100 + d.purchases.length), item_id: item.id, item_version: item.version, name: item.name,
        category: item.category, asset_key: item.asset.key, price_sunshine: item.price_sunshine, debited_sunshine: item.price_sunshine,
        sunshine_balance_after: d.balance, operation_id: uuid(2000 + who * 100 + d.purchases.length), created_at: new Date().toISOString() }
      d.purchases.unshift(purchase)
      const response = { replayed: false, purchase, current: { sunshine_balance: d.balance, item } }
      d.purchaseKeys.set(key, { body: JSON.stringify(body), response })
      if (!d.lostPurchase && who === 0) { d.lostPurchase = true; return fail(res, 'RESULT_UNKNOWN', 503) }
      return respond(res, response, 201)
    }
    if (p === '/api/v1/avatar/loadout' && req.method === 'POST') {
      const key = req.headers['idempotency-key'], prior = d.saveKeys.get(key)
      if (prior) { if (prior.body !== JSON.stringify(body)) return fail(res, 'IDEMPOTENCY_KEY_REUSED'); return respond(res, { replayed: true, loadout: prior.loadout, current: { appearance: appearance() } }) }
      if (body.expected_version !== d.events.length) return fail(res, 'AVATAR_VERSION_CONFLICT')
      for (const s of slots) {
        const id = body.slots[s]
        if (!id) continue
        const i = items.find(i => i.id === id)
        if (!i || i.category !== s) return fail(res, 'AVATAR_SLOT_INVALID', 400)
        if (!i.starter && !d.purchases.some(p => p.item_id === id)) return fail(res, 'AVATAR_NOT_OWNED')
      }
      const defaultsNeeded = ['hair', 'clothes', 'expression'].some(s => body.slots[s] === null)
      const loadout = { id: uuid(3000 + who * 100 + d.events.length), version: d.events.length + 1, requested: body.slots,
        slots: Object.fromEntries(slots.map(s => [s, body.slots[s] ?? defaults[s]])), starter_version: defaultsNeeded ? 1 : null, created_at: new Date().toISOString() }
      d.events.push(loadout); d.saveKeys.set(key, { body: JSON.stringify(body), loadout })
      if (!d.lostSave && who === 0) { d.lostSave = true; return fail(res, 'RESULT_UNKNOWN', 503) }
      return respond(res, { replayed: false, loadout, current: { appearance: appearance() } }, 201)
    }
    if (p === '/api/v1/anniversaries/home') return respond(res, { anniversary: null, source: 'none', as_of_date: '2026-11-10' })
    if (p === '/api/v1/tasks') return respond(res, { tasks: [], next_cursor: null, as_of: new Date().toISOString() })
    if (p.includes('/redemptions')) return respond(res, { redemptions: [], next_cursor: null })
    if (p === '/api/v1/messages') return respond(res, { messages: [], next_cursor: null })
    if (p === '/api/v1/notifications/unread-count') return respond(res, { unread_count: 0 })
    if (p === '/api/v1/push/config') return respond(res, { enabled: false })
    return fail(res, 'NOT_FOUND', 404)
  })
} }
const server = await createServer({ root: path.join(root, 'apps/web'), configFile: path.join(root, 'apps/web/vite.config.ts'),
  plugins: [plugin], server: { host: '127.0.0.1', port: 3764, strictPort: true } })
await server.listen()
console.log('B4 FAKE BROWSER READY http://127.0.0.1:3764/__b4 (loopback only; first A purchase/save deliberately return unknown after fake success)')
const close = async () => { console.log(JSON.stringify({ syntheticOnly: true, purchases: data.map(d => d.purchases.length), savedVersions: data.map(d => d.events.length), requests: calls.length })); await server.close(); process.exit(0) }
process.on('SIGINT', close); process.on('SIGTERM', close)
