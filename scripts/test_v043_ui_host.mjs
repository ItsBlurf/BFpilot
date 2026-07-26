import assert from 'assert'
import fs from 'fs'
import path from 'path'
import { fileURLToPath } from 'url'

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..')
const html = fs.readFileSync(path.join(root, 'assets', 'files.html'), 'utf8')
const version = fs.readFileSync(path.join(root, 'src', 'version.h'), 'utf8')
const makefile = fs.readFileSync(path.join(root, 'Makefile'), 'utf8')

assert(
  /\.browser\s*\{[^}]*grid-template-rows:\s*auto auto auto auto minmax\(0,\s*1fr\)/s.test(html),
  'upload row cannot consume the flexible file-list height'
)
assert(!html.includes('var(--accent)') && !html.includes('var(--text)'),
  'drag state uses defined theme colors')
assert(html.includes("const parent = list;"), 'virtual list listens to its own scroller')
assert(html.includes("row.addEventListener('dblclick'"), 'double-click handler exists')

const clickBlock = html.slice(
  html.indexOf("row.addEventListener('click'"),
  html.indexOf("row.addEventListener('dblclick'")
)
assert(!clickBlock.includes('if (entry.dir'), 'single click selects instead of opening a folder')

for (const marker of [
  'id="clipboard-status"',
  'id="paste"',
  'stageClipboard',
  'pasteClipboard',
  'clearClipboard',
  'Paste Here'
]) {
  assert(html.includes(marker), `clipboard flow includes ${marker}`)
}

assert(html.includes("el('open-folder').disabled"), 'Open disabled-state is selection-aware')
assert(html.includes("el('extract').disabled"), 'Extract disabled-state is selection-aware')
assert(html.includes("browser.addEventListener('dragenter'"), 'whole browser reveals drag state')
assert(html.includes("browser.addEventListener('drop'"), 'whole browser accepts dropped files')
assert(!html.includes('id="target-current"') && !html.includes('id="open-target"'),
  'old direct-target controls removed')
assert(version.includes('v0.4.3-alpha.1') && makefile.includes('v0.4.3-alpha.1'),
  'alpha version is consistent')

const scriptMatch = html.match(/<script>([\s\S]*)<\/script>/)
assert(scriptMatch, 'inline application script found')
new Function(scriptMatch[1])

console.log('v0.4.3 alpha UI checks passed')
