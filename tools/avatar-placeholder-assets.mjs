// Developer-made SVG placeholders, not final art. No database or network access.
import { createHash } from 'node:crypto'
import { readFile, writeFile, mkdir, readdir } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'
import path from 'node:path'

const root = fileURLToPath(new URL('../', import.meta.url))
const directory = path.join(root, 'apps/web/public/avatars')
const manifestPath = path.join(root, 'apps/web/src/assets/avatar-manifest.json')
const hash = value => createHash('sha256').update(value).digest('hex')
const svg = shape => `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512">${shape}</svg>\n`
const shapes = {
  'base.placeholder.v1.svg': '<ellipse cx="256" cy="454" rx="105" ry="12" fill="#d8d2c9"/><path d="M210 385h38v62h-38zm54 0h38v62h-38z" fill="#8b9ca0"/><path d="M196 270h120l19 127H177z" fill="#c3cecd"/><path d="M192 286l-36 83 24 12 38-75m102-20 36 83-24 12-38-75" fill="#efd0ba"/><rect x="236" y="245" width="40" height="44" rx="16" fill="#efd0ba"/><circle cx="256" cy="189" r="77" fill="#f5dbc7"/><circle cx="230" cy="199" r="4" fill="#73605b"/><circle cx="282" cy="199" r="4" fill="#73605b"/><path d="M246 228h20" stroke="#947d71" stroke-width="4" stroke-linecap="round"/>',
  'hair.placeholder_bob.v1.back.svg': '<path d="M173 174q0-90 83-90t83 90v151H173z" fill="#6c5149"/>',
  'hair.placeholder_bob.v1.front.svg': '<path d="M176 179q-5-90 80-90t80 90l-41-25-17 34-24-42-25 37-16-32z" fill="#6c5149"/>',
  'hair.placeholder_bob.v1.front_under_hat.svg': '<path d="M180 165l32-17 17 35 25-37 24 42 17-34 37 20v-30H180z" fill="#6c5149"/>',
  'hair.placeholder_wave.v1.back.svg': '<path d="M170 175q-12-95 86-95t86 95q-24 40 0 80t-6 82l-30-8-23 12-30-12-30 12-24-13-29 9q-24-35-4-82t4-80z" fill="#9b6d4f"/>',
  'hair.placeholder_wave.v1.front.svg': '<path d="M174 184q-8-97 82-97t82 97q-35-1-56-42-25 48-108 42z" fill="#9b6d4f"/>',
  'hair.placeholder_wave.v1.front_under_hat.svg': '<path d="M177 170q66 10 105-28 19 35 52 38v-37H177z" fill="#9b6d4f"/>',
  'clothes.placeholder_cream.v1.main.svg': '<path d="M220 269l36 17 36-17 43 33-23 40-12-13 17 72H195l17-72-12 13-23-40z" fill="#ece0c3" stroke="#c3b699" stroke-width="4"/><path d="M239 282l17 22 17-22" fill="none" stroke="#c3b699" stroke-width="4"/>',
  'clothes.placeholder_teal.v1.main.svg': '<path d="M220 269l36 17 36-17 43 33-23 40-12-13 17 72H195l17-72-12 13-23-40z" fill="#6e9e98" stroke="#4c7974" stroke-width="4"/><rect x="235" y="327" width="42" height="32" rx="5" fill="#a9ccc5"/>',
  'expression.placeholder_smile.v1.main.svg': '<ellipse cx="256" cy="211" rx="49" ry="35" fill="#f5dbc7"/><path d="M222 199q8-9 16 0m36 0q8-9 16 0m-48 26q14 18 28 0" fill="none" stroke="#70564e" stroke-width="5" stroke-linecap="round"/>',
  'expression.placeholder_wink.v1.main.svg': '<ellipse cx="256" cy="211" rx="49" ry="35" fill="#f5dbc7"/><circle cx="230" cy="199" r="5" fill="#70564e"/><path d="M273 199h17m-48 27q16 14 28-2" fill="none" stroke="#70564e" stroke-width="5" stroke-linecap="round"/>',
  'hat.placeholder_cap.v1.main.svg': '<path d="M175 142q8-72 81-72t81 72z" fill="#cf9278"/><path d="M166 142h180l-15 18H181z" fill="#ae7460"/><path d="M256 77v64" stroke="#e6b69b" stroke-width="4"/>',
  'handheld.placeholder_flower.v1.main.svg': '<path d="M350 376l20-60" stroke="#72916a" stroke-width="7"/><path d="M358 351q-32-22-23-31 31 4 23 31z" fill="#8bab79"/><g fill="#deb86d"><circle cx="370" cy="298" r="13"/><circle cx="354" cy="311" r="13"/><circle cx="361" cy="329" r="13"/><circle cx="381" cy="328" r="13"/><circle cx="388" cy="310" r="13"/></g><circle cx="371" cy="314" r="12" fill="#977354"/>',
  'accessory.placeholder_bow.v1.main.svg': '<path d="M252 283l-31-16v31zm8 0 31-16v31z" fill="#c58d92" stroke="#a96d77" stroke-width="3"/><circle cx="256" cy="283" r="8" fill="#a96d77"/>',
}
const categories = ['hair', 'clothes', 'expression', 'hat', 'handheld', 'accessory']
const assets = Object.keys(shapes).filter(n => !n.startsWith('base.')).reduce((out, filename) => {
  const [category, name, version, layer] = filename.split('.')
  const key = `${category}.${name}.${version}`
  out[key] ??= { category, family: `${category}.${name}`, revision: 1, format: 'svg',
    covers_hair_top: category === 'hat' ? true : null,
    accessory_position: category === 'accessory' ? 'neck' : null, layers: {} }
  out[key].layers[layer] = { file: filename, sha256: hash(svg(shapes[filename])) }
  return out
}, {})
const manifest = { version: 1, placeholder: true, canvas: [512, 512],
  base: { file: 'base.placeholder.v1.svg', sha256: hash(svg(shapes['base.placeholder.v1.svg'])) }, assets }
if (process.argv.includes('--write')) {
  // A versioned file/key is write-once. Adding a correction means a new version, not regenerating v1 in place.
  let previous
  try { previous = JSON.parse(await readFile(manifestPath, 'utf8')) } catch (error) { if (error.code !== 'ENOENT') throw error }
  if (previous && (JSON.stringify(previous.base) !== JSON.stringify(manifest.base) || Object.entries(previous.assets).some(([key, value]) =>
    JSON.stringify(value) !== JSON.stringify(assets[key])))) throw new Error('Existing manifest entry changed; create a new version')
  const missing = []
  for (const [filename, shape] of Object.entries(shapes)) {
    try {
      if (hash(await readFile(path.join(directory, filename))) !== hash(svg(shape))) throw new Error(`Refuse to overwrite versioned asset: ${filename}`)
    } catch (error) { if (error.code === 'ENOENT') missing.push(filename); else throw error }
  }
  await mkdir(directory, { recursive: true })
  await mkdir(path.dirname(manifestPath), { recursive: true })
  for (const filename of missing) await writeFile(path.join(directory, filename), svg(shapes[filename]), { flag: 'wx' })
  if (!previous || JSON.stringify(previous) !== JSON.stringify(manifest)) await writeFile(manifestPath, JSON.stringify(manifest, null, 2) + '\n')
}
// Checking is always performed, including on the enrollment-output path.
const actual = JSON.parse(await readFile(manifestPath, 'utf8'))
if (JSON.stringify(actual) !== JSON.stringify(manifest)) throw new Error('Manifest differs from the deterministic placeholder definition')
const files = (await readdir(directory)).sort()
if (JSON.stringify(files) !== JSON.stringify(Object.keys(shapes).sort())) throw new Error('Unexpected placeholder file set')
for (const [filename, shape] of Object.entries(shapes)) {
  const bytes = await readFile(path.join(directory, filename))
  if (hash(bytes) !== hash(svg(shape))) throw new Error(`Asset changed: ${filename}`)
  if (/<(?:script|foreignObject|image)\b|\b(?:href|onload|onclick)=|https?:\/\/(?!www\.w3\.org\/2000\/svg)/i.test(bytes.toString())) throw new Error('External or executable SVG content')
}
for (const [key, a] of Object.entries(assets)) {
  const expected = a.category === 'hair' ? ['back', 'front', 'front_under_hat'] : ['main']
  if (!categories.includes(a.category) || JSON.stringify(Object.keys(a.layers).sort()) !== JSON.stringify(expected)) throw new Error(`Invalid layer set: ${key}`)
}
if (process.argv.includes('--registry-json')) {
  // Prepared enrollment data ONLY. Loading it is a separate owner-controlled maintenance step.
  console.log(JSON.stringify(Object.entries(assets).map(([asset_key, a]) => ({ asset_key, ...a,
    layers: Object.fromEntries(Object.entries(a.layers).map(([k, v]) => [k, v.sha256])) })), null, 2))
} else console.log(`PLACEHOLDER ASSETS OK: ${files.length} files, ${Object.keys(assets).length} registry entries; no database touched`)
