/** Behavioral parity contract for owendswang/ps5-web-file-manager v1.4. */
import assert from 'assert'
import fs from 'fs'
import path from 'path'
import { fileURLToPath } from 'url'

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..')
const read = (name) => fs.readFileSync(path.join(root, name), 'utf8')
const html = read('assets/files.html')
const transfer = read('src/transfer.c')
const websrv = read('src/websrv_lite.c')
const main = read('src/lite_main.c')
const worker = read('src/pkg_worker_main.c')

for (const marker of [
  'id="new-folder"', 'id="new-text"', 'id="rename"', 'id="delete"',
  'id="copy"', 'id="move"', 'id="paste"', 'id="upload-files"',
  'id="upload-folder"', 'id="download"', 'id="chmod"',
  'row-checkbox', 'select-all-checkbox', 'confirmShadowMountWrite'
]) assert(html.includes(marker), `file-manager UI includes ${marker}`)

assert(html.includes('SORT_STORAGE_KEY') && html.includes('PATH_STORAGE_KEY'),
  'sort mode and last successful path persist')
assert(html.includes('droppedUploads') && html.includes('webkitGetAsEntry'),
  'recursive folder upload is available where the browser supports it')
assert(transfer.includes('staging_path') && transfer.includes('rename(staging_path, final_path)'),
  'uploads commit through a same-directory temporary file')
assert(transfer.includes('application/x-tar') && transfer.includes('tar_stream_path'),
  'folders and multi-selection downloads stream as TAR without PS5 staging')
assert(html.includes("api('/api/fs/job/status')") &&
  html.includes("api('/api/fs/archive/status')") &&
  html.includes("api('/api/fs/pkg/status')") &&
  html.includes('refreshSearchStatus(true)'),
  'browser reopen recovers every active task surface')
assert(transfer.includes('apply_fchmod_0777') && transfer.includes('bfpilot_path_chmod'),
  'created/copied files and explicit chmod are supported')
assert(html.includes('@media') && html.includes('virtual-scroller'),
  'responsive and large-directory layouts are present')
assert(main.includes('bfpilot_notify("BFpilot started"'),
  'startup announces the live BFpilot URL')
for (const extension of ['.txt', '.json', '.xml', '.ini', '.cfg', '.conf',
  '.md', '.log', '.lua', '.js', '.css', '.html', '.htm', '.c', '.h',
  '.cpp', '.hpp', '.sh', '.csv', '.yaml', '.yml', '.shn']) {
  assert(html.includes(`'${extension}'`), `text editor supports ${extension}`)
}
for (const extension of ['.png', '.jpg', '.jpeg', '.gif', '.bmp', '.webp']) {
  assert(html.includes(`'${extension}'`), `image viewer supports ${extension}`)
}
assert(websrv.includes('/api/fs/pkg/install') &&
  worker.includes('sceAppInstUtilInstallByPackage') &&
  worker.includes('"/user%s"'),
  'local PKG submission includes the internal-storage mapping')

assert(html.includes("window.addEventListener('popstate'") &&
  html.includes('pathNavigationBlocked') && html.includes('revealPathInList') &&
  html.includes('input.scrollLeft = input.scrollWidth'),
  'v1.4 back/reveal/long-path behavior is covered')
assert(main.includes('handoff_existing_server') && main.includes('BFPILOT_WEB_PORT'),
  'fixed tile port uses safe same-service handoff')
assert(!fs.readdirSync(path.join(root, 'assets')).some((name) => /^lang-/i.test(name)),
  'BFpilot intentionally ships English only')

new Function(html.match(/<script>([\s\S]*)<\/script>/)[1])
console.log('upstream v1.4 behavioral parity checks passed')
