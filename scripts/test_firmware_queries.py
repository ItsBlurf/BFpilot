"""Compile both firmware-query functions with controlled SDK/sysctl responses."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cases = [
    ('appinst_direct.c', 'int\nbfpilot_appinst_firmware_major(void)', 'static uint64_t', 'bfpilot_appinst_firmware_major'),
    ('pkg_worker_main.c', 'static int\nfirmware_major(void)', 'static int\nacquire_install_identity', 'firmware_major'),
]
with tempfile.TemporaryDirectory(prefix='bfpilot-firmware-') as temporary:
    for name, signature, next_function, call in cases:
        text = (root/'src'/name).read_text(encoding='utf-8')
        start = text.index('static int\nparse_firmware_major(')
        end = text.index(next_function, text.index(signature))
        body = text[start:end]
        prelude = '''#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "firmware.h"
static uint32_t sdk_version;
static int sysctl_failed;
static const char *reported = "FreeBSD kernel releases/9.60";
static unsigned int kernel_get_fw_version(void) { return sdk_version; }
static int sysctlbyname(const char *name, void *value, size_t *size, const void *set, size_t count) {
  (void)name; (void)set; (void)count;
  if(sysctl_failed) return -1;
  size_t length = strlen(reported);
  if(length > *size) length = *size;
  memcpy(value, reported, length); *size = length;
  return 0;
}
'''
        main = '''int main(void) {
  sdk_version=0x13600000; assert(QUERY()==13);
  sdk_version=0x11600000; assert(QUERY()==11);
  sdk_version=0x14100000; assert(QUERY()==14);
  sysctl_failed=1; assert(QUERY()==14);
  sysctl_failed=0; sdk_version=0; assert(QUERY()==9);
  sdk_version=0xffffffff; assert(QUERY()==9);
  reported="FreeBSD kernel releases/13.60"; assert(QUERY()==13);
  reported="unknown"; assert(QUERY()==0);
  sysctl_failed=1; assert(QUERY()==0);
  puts("Firmware source priority and fallback: 9 controls passed");
  return 0;
}
'''.replace('QUERY()', call+'()')
        source = Path(temporary)/(name+'.c')
        binary = Path(temporary)/(name+'.exe')
        source.write_text(prelude+body+main, encoding='utf-8')
        subprocess.run([os.environ.get('HOST_CC','gcc'),'-Wall','-Wextra','-Werror',
                        '-I'+str(root/'src'),str(source),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
