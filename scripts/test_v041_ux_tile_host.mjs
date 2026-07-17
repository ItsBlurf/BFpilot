/**
 * Structural checks for v0.4.1: UX, single data dir, separate Media installer.
 */
import assert from 'assert'
import fs from 'fs'
import path from 'path'
import { fileURLToPath } from 'url'

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..')
const read = (p) => fs.readFileSync(path.join(root, p), 'utf8')

const html = read('assets/files.html')
const pathsH = read('src/paths.h')
const liteMain = read('src/lite_main.c')
const param = read('assets-app/param.json')
const makefile = read('Makefile')
const tileBoot = read('src/tile_bootstrap.c')
const launcher = read('src/launcher_installer_force_main.c')
const transfer = read('src/transfer.c')
const pathOps = read('src/path_ops.c')
const versionH = read('src/version.h')

// --- Action bar: wrap, not horizontal-scroll-only ---
assert(/\.actions\s*\{[^}]*flex-wrap:\s*wrap/s.test(html), 'actions flex-wrap wrap')
assert(!/\.actions\s*\{[^}]*overflow-x:\s*auto/s.test(html), 'no overflow-x auto on .actions')
assert(/overflow-x:\s*hidden/.test(html), 'overflow-x hidden present')
const mediaActions = html.match(/@media[^{]+\{[\s\S]*?\.actions\s*\{([^}]*)\}/)
if (mediaActions) {
  assert(!/flex-wrap:\s*nowrap/.test(mediaActions[1]), 'media .actions must not force nowrap')
}

// --- Single data root ---
assert(/#define\s+BFPILOT_DATA_DIR\s+"\/data\/BFpilot"/.test(pathsH), 'canonical data dir')
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

// --- Separate launcher model (main must NOT embed AppInst install) ---
assert(!fs.existsSync(path.join(root, 'src/app_installer.c')), 'no app_installer.c in main tree')
assert(!makefile.includes('app_installer.c'), 'app_installer not in Makefile WEB_SRCS')
assert(makefile.includes('BFPILOT_ENABLE_LAUNCHER=0') || makefile.includes('ENABLE_LAUNCHER=0'),
       'launcher disabled in main flags')
assert(makefile.includes('scrub_main_payload.py'), 'scrub main payload for AppInst fingerprints')
assert(liteMain.includes('bfpilot_tile_bootstrap_try'), 'tile bootstrap for separate installer')
assert(liteMain.includes('separate_payload') || liteMain.includes('separate'),
       'runtime separate_payload messaging')
assert(!liteMain.includes('bfpilot_install_app_if_needed'), 'main must not call embedded install')

// --- Media tile lives in separate installer ---
const paramObj = JSON.parse(param)
assert.strictEqual(paramObj.applicationCategoryType, 65536, 'Media category 65536')
assert.strictEqual(paramObj.titleId, 'BFPL00001')
assert.ok(String(paramObj.deeplinkUri).includes('5905'))
assert(launcher.includes('BFPILOT_APP_TITLE_ID') || launcher.includes('BFPL00001') ||
       launcher.includes('bfpilot_param_json'), 'installer embeds app assets')
assert(launcher.includes('sceAppInstUtilInitialize') || launcher.includes('AppInst'),
       'installer uses AppInst')
assert(tileBoot.includes('BFPILOT_INSTALLER_ELF_PATH') ||
       tileBoot.includes('/data/BFpilot/bfpilot-launcher-installer.elf'))

// --- Prior issue surface (#7-12, #10) ---
assert(html.includes('openPath(entryPath(entry))'), '#7')
assert(pathOps.includes('bfpilot_path_resolve_ci'), '#8')
assert(html.includes("endsWith('.7z')"), '#9')
assert(transfer.includes('chmod_request') || transfer.includes('/api/fs/chmod'), '#10')
assert(html.includes('chmodSelected') || html.includes('id="chmod"'), '#10 ui')
assert(html.includes('row-checkbox') && html.includes('select-all-checkbox'), '#11')
assert(versionH.includes('v0.4.1') && makefile.includes('v0.4.1'), '#12')

console.log('v0.4.1 separate-launcher + UX host checks passed')
