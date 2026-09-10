/** Tests for Fullscreen WebApp, PKG info/icon dialog, CHMOD modal, and v1.8 parity. */
import assert from 'assert'
import fs from 'fs'
import path from 'path'
import { fileURLToPath } from 'url'

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..')
const read = (name) => fs.readFileSync(path.join(root, name), 'utf8')

const param = JSON.parse(read('assets-app/param.json'))
const appinst = read('src/appinst_direct.c')
const transfer = read('src/transfer.c')
const websrv = read('src/websrv_lite.c')
const pkgFileH = read('src/pkg_file.h')
const pkgFileC = read('src/pkg_file.c')
const html = read('assets/files.html')

// 1. Fullscreen WebApp
assert.strictEqual(param.applicationCategoryType, 66048, 'applicationCategoryType must be 66048 (Web Based Media App)')
assert.ok(param.webAppUri && param.webAppUri.includes('5905'), 'webAppUri must point to BFpilot port 5905')
assert.ok(param.deeplinkUri && param.deeplinkUri.includes('5905'), 'deeplinkUri must point to BFpilot port 5905')

// 2. Universal tile installation fallback
assert.ok(appinst.includes('sceAppInstUtilAppInstallAll(NULL)'), 'appinst_direct must have fallback to sceAppInstUtilAppInstallAll')
assert.ok(appinst.includes('chmod(app_dir, 0755)'), 'appinst_direct must set directory permissions to 0755')
assert.ok(appinst.includes('chmod(param_path, 0644)'), 'appinst_direct must set asset permissions to 0644')

// 3. PKG Info & Icon Services
assert.ok(pkgFileH.includes('bfpilot_pkg_parse_details'), 'pkg_file.h declares bfpilot_pkg_parse_details')
assert.ok(pkgFileH.includes('bfpilot_pkg_read_icon'), 'pkg_file.h declares bfpilot_pkg_read_icon')
assert.ok(transfer.includes('/api/fs/pkg/info'), 'transfer.c routes /api/fs/pkg/info')
assert.ok(transfer.includes('/api/fs/pkg/icon'), 'transfer.c routes /api/fs/pkg/icon')
assert.ok(websrv.includes('/api/fs/pkg/info'), 'websrv_lite routes list includes /api/fs/pkg/info')

// 4. CHMOD API & recursive support
assert.ok(transfer.includes('/api/fs/chmod'), 'transfer.c routes /api/fs/chmod')
assert.ok(transfer.includes('chmod_recursive_dir'), 'transfer.c implements recursive directory chmod')
assert.ok(websrv.includes('/api/fs/chmod'), 'websrv_lite POST routes handle /api/fs/chmod')

// 5. UI: PKG modal, CHMOD modal, clickable storage, mode pill
assert.ok(html.includes('id="pkg-overlay"'), 'files.html contains #pkg-overlay')
assert.ok(html.includes('id="pkg-modal-icon"'), 'files.html contains #pkg-modal-icon')
assert.ok(html.includes('id="pkg-modal-install"'), 'files.html contains #pkg-modal-install button')
assert.ok(html.includes('id="chmod-overlay"'), 'files.html contains #chmod-overlay')
assert.ok(html.includes('id="chmod-octal"'), 'files.html contains #chmod-octal input')
assert.ok(html.includes('mode-clickable'), 'files.html supports clickable mode column')
assert.ok(html.includes('storage-item-btn'), 'files.html implements clickable storage buttons')
assert.ok(html.includes('psbase:shell?c=play_go_resume'), 'files.html navigates to home screen on exit')

console.log('All v1.8 superiority, fullscreen webapp, and universal tile installer checks passed!')
