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
assert(
  /button\.primary:disabled,[\s\S]*?\{[^}]*background:\s*#151d27[^}]*color:\s*#607080/s.test(html),
  'disabled operations use an explicit neutral gray treatment'
)
assert(
  /\.upload-drop\s*\{[^}]*display:\s*none/s.test(html) &&
  /\.browser\.drag-active \.upload-drop\s*\{[^}]*display:\s*block/s.test(html),
  'drag upload prompt is hidden until a file drag enters the browser'
)
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
  'id="copy-target"',
  'id="move-target"',
  'id="target-current"',
  'id="open-target"',
  'stageClipboard',
  'pasteClipboard',
  'transferToTarget',
  'clearClipboard',
  'Paste Here'
]) {
  assert(html.includes(marker), `clipboard flow includes ${marker}`)
}

const targetButtonBlock = html.slice(
  html.indexOf('function targetButton'),
  html.indexOf('function renderTargetButtons')
)
assert(targetButtonBlock.includes("el('target-path').value = path"),
  'target shortcut fills the direct-transfer target')
assert(!targetButtonBlock.includes('openPath(path)'),
  'target shortcut does not change the current folder')
assert(
  /function directTargetPath\(\)[\s\S]*?raw\[0\] !== '\/'[\s\S]*?Target must be an absolute path/.test(html),
  'direct transfer rejects non-absolute targets'
)
assert(
  /async function transferToTarget\(verb\)[\s\S]*?transferWouldRecurse[\s\S]*?executeTransfer\(verb, entries, target\)/.test(html),
  'direct transfer shares recursion checks and the existing transfer engine'
)

assert(html.includes("el('open-folder').disabled"), 'Open disabled-state is selection-aware')
assert(html.includes("el('extract').disabled"), 'Extract disabled-state is selection-aware')
assert(html.includes("browser.addEventListener('dragenter'"), 'whole browser reveals drag state')
assert(html.includes("browser.addEventListener('drop'"), 'whole browser accepts dropped files')
assert(html.includes('Select items, choose a destination, then copy or move directly.'),
  'direct target workflow is explained')
assert(version.includes('v0.4.3-alpha.4') && makefile.includes('v0.4.3-alpha.4'),
  'alpha version is consistent')

const scriptMatch = html.match(/<script>([\s\S]*)<\/script>/)
assert(scriptMatch, 'inline application script found')
new Function(scriptMatch[1])

console.log('v0.4.3 alpha UI checks passed')
