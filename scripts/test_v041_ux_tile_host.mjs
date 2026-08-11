/**
 * Structural checks for BFpilot UX, one data dir, and the all-in-one payload.
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
const appinst = read('src/appinst_direct.c')
const pkgWorker = read('src/pkg_worker_main.c')
const pkgClient = read('src/pkg_worker_client.c')
const transfer = read('src/transfer.c')
const pathOps = read('src/path_ops.c')
const versionH = read('src/version.h')

// --- Action bar: compact at desktop, wraps at the responsive breakpoint ---
assert(/\.actions\s*\{[^}]*flex-wrap:\s*nowrap/s.test(html), 'desktop actions stay on one row')
assert(!/\.actions\s*\{[^}]*overflow-x:\s*auto/s.test(html), 'no overflow-x auto on .actions')
const mediaActions = html.match(/@media[^{]+\{[\s\S]*?\.actions\s*\{([^}]*)\}/)
if (mediaActions) {
  assert(/flex-wrap:\s*wrap/.test(mediaActions[1]), 'responsive actions wrap')
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

// --- One distributed payload: direct tile path plus private embedded worker ---
for (const source of ['src/appinst_direct.c', 'src/pkg_file.c',
                      'src/pkg_install.c', 'src/pkg_worker_client.c']) {
  assert(makefile.includes(source), `${source} is linked into bfpilot.elf`)
}
assert(!makefile.includes('BFPILOT_ENABLE_LAUNCHER') &&
       !makefile.includes('BFPILOT_DISABLE_LAUNCHER'),
       'obsolete optional-launcher build modes are gone')
assert(!makefile.includes('scrub_main_payload.py'), 'runtime AppInst strings are not corrupted')
assert(!makefile.includes('launcher-installer:'), 'no launcher helper target')
assert(!makefile.includes('pkg-installer:'), 'no PKG helper target')
assert(liteMain.includes('bfpilot_tile_bootstrap_try'), 'integrated tile bootstrap')
assert(liteMain.includes('single_distribution_elf'), 'runtime all-in-one diagnostic')
assert(makefile.includes('PKG_WORKER_BIN := build/internal/') &&
       pkgClient.includes('INCASSET(bfpilot_pkg_worker_elf') &&
       pkgClient.includes('bfpilot_launch_payload_memory'),
       'private worker is embedded in the one distributed ELF')
assert(makefile.includes('-lSceIpmi -lSceAppInstUtil'),
       'main ELF directly links AppInst with its IPMI dependency')
assert(!appinst.includes('dlclose(') && !appinst.includes('AppInstUtilTerminate'),
       'tile AppInst state is not torn down under a running file manager')
assert(appinst.includes('sceAppInstUtilInitialize()') &&
       pkgWorker.includes('sceAppInstUtilInstallByPackage('),
       'tile and full-content PKG AppInst calls are built into one payload')
assert(appinst.includes('sysctlbyname("kern.version"') &&
       appinst.includes('PS5_SHELLCORE_AUTHID') &&
       appinst.includes('PS5_SYSTEM_INSTALL_AUTHID'),
       'AppInst selects the identity from a safe firmware query')
assert(appinst.includes('authid_acquire_locked') &&
       appinst.includes('authid_release_locked') &&
       appinst.includes('for(int attempt = 0; attempt < 3; attempt++)'),
       'AppInst limits auth-ID changes to a verified, retry-restored window')

// --- The embedded Media tile and PKG installer use the same runtime ---
const paramObj = JSON.parse(param)
assert.strictEqual(paramObj.applicationCategoryType, 65536, 'Media category 65536')
assert.strictEqual(paramObj.titleId, 'BFPL00001')
assert.ok(String(paramObj.deeplinkUri).includes('5905'))
assert(appinst.includes('bfpilot_param_json') && appinst.includes('bfpilot_icon0_png'),
       'main ELF embeds app assets')
assert(appinst.includes('.bfpilot-registered') &&
       appinst.includes('g_tile_registration_marker'),
       'tile skip requires a marker written only after successful registration')
assert(pkgWorker.includes('sceAppInstUtilInstallByPackage'), 'integrated PKG install')
assert(pkgWorker.includes('"/user%s"') &&
       !pkgWorker.includes('"file:///user%s"'),
       'local PKGs use absolute AppInst paths and map /data to /user/data')
assert(!pkgWorker.includes('sceAppInstUtilAppInstallPkg') &&
       pkgWorker.includes('.uri = install_path') &&
       pkgWorker.includes('fresh elfldr'),
       'local PKGs use a fresh full-content worker without hollow-tile fallback')
assert(pkgWorker.includes('languages[30][8]') &&
       pkgWorker.includes('sizeof(appinst_playgo_info_t) == 9984'),
       'correct AppInst PlayGo ABI')
assert(pkgWorker.includes('htonl(INADDR_LOOPBACK)') &&
       pkgWorker.includes('PS5_SYSTEM_INSTALL_AUTHID') &&
       pkgWorker.includes('major >= 10'),
       'private worker is loopback-only and uses the FW10+ SYSTEM gate')
assert(pkgWorker.includes('INIT_TIMEOUT_SENTINEL') &&
       pkgWorker.includes('start_appinst_initialize') &&
       pkgClient.includes('bfpilot_pkg_worker_stop') &&
       liteMain.includes('bfpilot_pkg_worker_stop'),
       'AppInst init is bounded and clean shutdown retires the private worker')
assert(tileBoot.includes('bfpilot_appinst_install_tile'), 'tile uses direct AppInst')
assert(html.includes("lower.endsWith('.shn')") && transfer.includes('".shn"'),
       'upstream .shn text support exists in both UI and server validation')

// --- Prior issue surface (#7-12, #10) ---
assert(html.includes("row.addEventListener('dblclick'") &&
       html.includes('openPath(next)'), '#7')
assert(pathOps.includes('bfpilot_path_resolve_ci'), '#8')
assert(html.includes("endsWith('.7z')"), '#9')
assert(transfer.includes('chmod_request') || transfer.includes('/api/fs/chmod'), '#10')
assert(html.includes('chmodSelected') || html.includes('id="chmod"'), '#10 ui')
assert(html.includes('row-checkbox') && html.includes('select-all-checkbox'), '#11')
assert(versionH.includes('v0.4.4-alpha.1') && makefile.includes('v0.4.4-alpha.1'),
       'current release version')

console.log('v0.4.4-alpha.1 all-in-one + UX host checks passed')
