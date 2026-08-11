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
assert(html.includes("window.addEventListener('popstate'") &&
  html.includes('seedHistoryPath') && html.includes('pathNavigationBlocked'),
  'Circle/browser back follows guarded directory history')
assert(html.includes('revealPathInList') && html.includes("row.dataset.path"),
  'returning to a parent reveals the folder that was left')
assert(html.includes('input.scrollLeft = input.scrollWidth'),
  'long editable paths reveal their rightmost component')
assert(html.includes("const PATH_STORAGE_KEY = 'bfpilot:last-path:v1'") &&
  html.includes('readSavedPath()') && html.includes('savePath(state.path)'),
  'the last successful folder survives a browser reload')

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

assert(html.includes("setContextAction('open-folder'") &&
  html.includes("setContextAction('extract'"),
  'Open and Extract visibility is selection-aware')
assert(html.includes("setContextAction('load-payload'") &&
  html.includes("setContextAction('install-pkg'") &&
  html.includes('const allPackages = hasSelection && pkgCount === entries.length'),
  'mutually exclusive payload and PKG actions only appear when applicable')
assert(html.includes("browser.addEventListener('dragenter'"), 'whole browser reveals drag state')
assert(html.includes("browser.addEventListener('drop'"), 'whole browser accepts dropped files')
assert(html.includes('Select items, choose a destination, then copy or move directly.'),
  'direct target workflow is explained')
const actionsBlock = html.slice(
  html.indexOf('<div class="actions">'),
  html.indexOf('<div id="upload-drop"')
)
const morePanel = actionsBlock.slice(
  actionsBlock.indexOf('<div class="action-menu-panel">'),
  actionsBlock.indexOf('</details>')
)
assert(actionsBlock.indexOf('id="new-text"') < actionsBlock.indexOf('<details class="action-menu">'),
  'New Text File is a direct toolbar action')
assert(actionsBlock.indexOf('id="delete"') < actionsBlock.indexOf('<details class="action-menu">'),
  'Delete is a direct toolbar action')
assert(!morePanel.includes('id="new-text"') && !morePanel.includes('id="delete"'),
  'New Text File and Delete are not hidden in More')
assert(actionsBlock.indexOf('id="selection"') < actionsBlock.indexOf('id="context-actions"') &&
  actionsBlock.includes('class="action-group context-actions" hidden'),
  'selection count stays visible before the dynamic contextual action group')
assert(/\.context-actions\[hidden\]\s*\{[^}]*display:\s*none/s.test(html),
  'hidden contextual actions consume no toolbar space')
assert(version.includes('v0.4.4') && makefile.includes('v0.4.4') &&
  !version.includes('alpha') && !makefile.includes('alpha'),
  'release version is consistent')

const scriptMatch = html.match(/<script>([\s\S]*)<\/script>/)
assert(scriptMatch, 'inline application script found')
new Function(scriptMatch[1])

console.log('v0.4.4 UI checks passed')
