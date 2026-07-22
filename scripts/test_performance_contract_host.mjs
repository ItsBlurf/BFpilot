/** Structural performance/stability checks for the PS5 I/O hot paths. */
import assert from 'assert'
import fs from 'fs'
import path from 'path'
import { fileURLToPath } from 'url'

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..')
const read = (name) => fs.readFileSync(path.join(root, name), 'utf8')

const transfer = read('src/transfer.c')
const fileServer = read('src/fs.c')
const webServer = read('src/websrv_lite.c')
const archive = read('src/archive_worker.cpp')
const search = read('src/search.c')

assert(transfer.includes('BFPILOT_TRANSFER_BUF_SIZE (8 * 1024 * 1024)'),
  'copy buffer stays 8 MiB')
assert(transfer.includes('BFPILOT_COPY_PIPELINE_MIN_SIZE'),
  'large-file pipeline threshold')
assert(transfer.includes('buffers[2]'), 'copy pipeline is bounded to two slots')
assert(transfer.includes('copy_file_pipelined'), 'large-file pipelined copy')
assert(transfer.includes('copy_file_serial'), 'small-file/fallback serial copy')
assert(transfer.includes('BFPILOT_WRITEBACK_SYNC_BYTES'),
  'large writes have bounded writeback checkpoints')
assert(transfer.includes('posix_memalign(&buffer, BFPILOT_IO_ALIGNMENT'),
  'copy/upload buffers request PS5-page alignment')
assert(transfer.includes('POSIX_FADV_SEQUENTIAL'), 'copy source sequential hint')
assert(transfer.includes('source became shorter during copy'),
  'copy detects source truncation')
assert(transfer.includes('source changed during copy'),
  'copy revalidates source after transfer')
assert(!/transfer_upload_request[\s\S]*posix_fallocate\(/.test(
  transfer.slice(transfer.indexOf('transfer_upload_request'))),
  'live HTTP upload must not preallocate the whole body')

assert(fileServer.includes('BFPILOT_STREAM_BUF_SIZE (2 * 1024 * 1024)'),
  'download buffer is 2 MiB')
assert(fileServer.includes('POSIX_FADV_SEQUENTIAL'),
  'download source sequential hint')
assert(fileServer.includes('buffer_aligned'), 'download alignment telemetry')
assert(!fileServer.includes('sendfile('),
  'download avoids filesystem-dependent PS5 sendfile kernel risk')

assert(webServer.includes('BFPILOT_CONN_SNDBUF (4 * 1024 * 1024)'),
  'download send-window request is 4 MiB')
assert(!/setsockopt\([^;]*TCP_NODELAY/.test(webServer),
  'bulk HTTP never enables TCP_NODELAY')

assert(archive.includes('BFPILOT_ARCHIVE_IO_ALIGNMENT 0x4000U'),
  'ZIP buffers use PS5 page alignment')
assert(archive.includes('BFPILOT_ZIP_IN_BUFFER_SIZE (16U * 1024U * 1024U)'),
  'ZIP input buffer remains 16 MiB')
assert(archive.includes('BFPILOT_ZIP_OUT_BUFFER_SIZE (64U * 1024U * 1024U)'),
  'ZIP output buffer remains 64 MiB')
assert(archive.includes('ArchiveWritebackCheckpoint'),
  'large ZIP outputs bound dirty-page accumulation')
assert(archive.includes('posix_fallocate(fd, 0, (off_t)size)'),
  'known-size archive outputs reserve storage before decode')
const preallocate = archive.slice(
  archive.indexOf('PreallocateOutputFile'),
  archive.indexOf('CheckDestinationSpace'))
assert(!preallocate.includes('fsync('),
  'successful preallocation does not add a redundant blocking fsync')
assert(archive.includes('POSIX_FADV_SEQUENTIAL'),
  'ZIP volumes use sequential read hints')

assert(search.includes('fstatat(dirfd(dir), ent->d_name, &st, AT_SYMLINK_NOFOLLOW)'),
  'index metadata lookup is parent-anchored and no-follow')
assert(search.includes('stopped_early'),
  'search stops once the requested page plus one match is known')
assert(search.includes('matchedExact'), 'search reports early-stop count semantics')

console.log('performance/stability contract host checks passed')
