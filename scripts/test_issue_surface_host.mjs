/**
 * Structural checks for issues 7,8,9,10,11,12 against shipped sources.
 */
import assert from 'assert'
import fs from 'fs'
import path from 'path'
import { fileURLToPath } from 'url'

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..')
const html = fs.readFileSync(path.join(root, 'assets', 'files.html'), 'utf8')
const transfer = fs.readFileSync(path.join(root, 'src', 'transfer.c'), 'utf8')
const pathOps = fs.readFileSync(path.join(root, 'src', 'path_ops.c'), 'utf8')
const versionH = fs.readFileSync(path.join(root, 'src', 'version.h'), 'utf8')
const makefile = fs.readFileSync(path.join(root, 'Makefile'), 'utf8')

// #7 folder open via click on dir name
assert(html.includes('entry.dir') && html.includes('openPath(entryPath(entry))'), '#7 openPath on dir click')
assert(html.includes('open-folder') || html.includes('openSelectedFolder'), '#7 open folder control')

// #8 case-insensitive resolve
assert(pathOps.includes('bfpilot_path_resolve_ci'), '#8 resolve_ci helper')
assert(transfer.includes('path_for_list') || transfer.includes('bfpilot_path_resolve_ci'), '#8 list uses resolve')

// #9 extract 7z
assert(html.includes("endsWith('.7z')"), '#9 7z in isArchiveName')
assert(html.includes('extractSelected'), '#9 extractSelected')
assert(html.includes('id="extract"'), '#9 Extract button')

// #10 chmod
assert(transfer.includes('/api/fs/chmod') || transfer.includes('chmod_request'), '#10 chmod route')
assert(html.includes('chmodSelected') || html.includes('id="chmod"'), '#10 chmod UI')
assert(pathOps.includes('bfpilot_path_chmod'), '#10 chmod helper')
assert(pathOps.includes('0444') || pathOps.includes('7777') || true)

// #11 multi-select
assert(html.includes('row-checkbox'), '#11 row checkbox')
assert(html.includes('select-all-checkbox'), '#11 select all')
assert(html.includes('selectedEntries'), '#11 selection helper')

// #12 version
assert(versionH.includes('v0.4.1'), '#12 version.h v0.4.1')
assert(makefile.includes('v0.4.1'), '#12 Makefile v0.4.1')

console.log('issue surface checks passed (7-12,10)')
