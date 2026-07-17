/**
 * Structural checks for v0.4.1 UX / single data dir / Media tile parity.
 * Drives real shipped sources (no mocks of the paths under test).
 */
import assert from 'assert'
import fs from 'fs'
import path from 'path'
import { fileURLToPath } from 'url'

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..')
const read = (p) => fs.readFileSync(path.join(root, p), 'utf8')

const html = read('assets/files.html')
const pathsH = read('src/paths.h')
const appInst = read('src/app_installer.c')
const liteMain = read('src/lite_main.c')
const param = read('assets-app/param.json')
const makefile = read('Makefile')
const tileBoot = read('src/tile_bootstrap.c')
const transfer = read('src/transfer.c')
const pathOps = read('src/path_ops.c')
const versionH = read('src/version.h')

// --- Action bar: wrap, not horizontal-scroll-only ---
assert(html.includes('.actions'), 'actions class present')
assert(/\.actions\s*\{[^}]*flex-wrap:\s*wrap/s.test(html), 'actions flex-wrap wrap')
assert(!/\.actions\s*\{[^}]*overflow-x:\s*auto/s.test(html), 'actions must not use overflow-x auto')
assert(/overflow-x:\s*hidden/.test(html), 'actions overflow-x hidden present')
// mobile media query must not force nowrap on .actions
const mediaActions = html.match(/@media[^{]+\{[\s\S]*?\.actions\s*\{([^}]*)\}/)
if (mediaActions) {
  assert(!/flex-wrap:\s*nowrap/.test(mediaActions[1]), 'media .actions must not force nowrap')
}

// --- Single data root ---
assert(pathsH.includes('#define BFPILOT_DATA_DIR           "/data/BFpilot"') ||
       pathsH.includes('#define BFPILOT_DATA_DIR "/data/BFpilot"') ||
       /#define\s+BFPILOT_DATA_DIR\s+"\/data\/BFpilot"/.test(pathsH),
       'canonical data dir')
assert(!pathsH.includes('"/data/bfpilot"'), 'no lowercase data root in paths.h')
assert(!pathsH.includes('"/data/BFPILOT"'), 'no UPPERCASE data root in paths.h')
assert(!pathsH.includes('"/data/bfpilot-launcher-installer"'), 'no installer data root')

// Sources must not mkdir alternate roots (scan C/C++ under src)
const srcFiles = []
function walk(d) {
  for (const ent of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, ent.name)
    if (ent.isDirectory()) walk(p)
    else if (/\.(c|h|cpp)$/.test(ent.name)) srcFiles.push(p)
  }
}
walk(path.join(root, 'src'))
for (const f of srcFiles) {
  const t = fs.readFileSync(f, 'utf8')
  assert(!t.includes('"/data/bfpilot"'), `no /data/bfpilot in ${path.relative(root, f)}`)
  assert(!t.includes('"/data/BFPILOT"'), `no /data/BFPILOT in ${path.relative(root, f)}`)
  assert(!t.includes('"/data/bfpilot-launcher-installer"'),
         `no installer dir root in ${path.relative(root, f)}`)
}

// --- Media tile in primary payload (pldmgr parity) ---
const paramObj = JSON.parse(param)
assert.strictEqual(paramObj.applicationCategoryType, 65536, 'Media category 65536')
assert.strictEqual(paramObj.titleId, 'BFPL00001')
assert.ok(String(paramObj.deeplinkUri).includes('5905'))
assert(appInst.includes('bfpilot_install_app_if_needed'), 'install API')
assert(appInst.includes('INCASSET') || appInst.includes('incbin'), 'embedded assets')
assert(appInst.includes('assets-app/param.json'), 'param embedded')
assert(liteMain.includes('bfpilot_install_app_if_needed'), 'main calls install')
assert(makefile.includes('app_installer.c'), 'app_installer linked in main')
assert(makefile.includes('SceAppInstUtil') || makefile.includes('lSceAppInstUtil'),
       'AppInst linked')
// recovery installer path is a file under data dir only
assert(tileBoot.includes('BFPILOT_INSTALLER_ELF_PATH') ||
       tileBoot.includes('/data/BFpilot/bfpilot-launcher-installer.elf'))
assert(!tileBoot.includes('/data/homebrew/bfpilot-launcher-installer.elf'),
       'no loose /data/homebrew installer root path')

// --- Prior issue surface (#7-12, #10) ---
assert(html.includes('openPath(entryPath(entry))'), '#7')
assert(pathOps.includes('bfpilot_path_resolve_ci'), '#8')
assert(html.includes("endsWith('.7z')"), '#9')
assert(transfer.includes('chmod_request') || transfer.includes('/api/fs/chmod'), '#10')
assert(html.includes('chmodSelected') || html.includes('id="chmod"'), '#10 ui')
assert(html.includes('row-checkbox') && html.includes('select-all-checkbox'), '#11')
assert(versionH.includes('v0.4.1') && makefile.includes('v0.4.1'), '#12')

console.log('v0.4.1 UX/tile/data-dir host checks passed')
