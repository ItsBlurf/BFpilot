"""Extract the exact product log serializer for guarded host/native controls."""
import argparse
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--out', type=Path, default=PROJECT/'build/host-reliability')
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
source = (PROJECT/'src/diag.c').read_text(encoding='utf-8')
start = source.index('static const char *\ncase_str_search')
end = source.index('int\nbfpilot_diag_get_memory_info', start)
prefix = '''#include <strings.h>
#include <time.h>
#define BFPILOT_LOG_RING_CAPACITY 512
#define BFPILOT_LOG_MAX_LINE 1024
typedef struct {char line[1024];time_t ts;} bfpilot_log_entry_t;
static bfpilot_log_entry_t g_log_ring[512];
static size_t g_log_ring_head,g_log_ring_count;
static int g_diag_lock;
static int pthread_mutex_lock(int *p){(void)p;return 0;}
static int pthread_mutex_unlock(int *p){(void)p;return 0;}
static size_t allocation_capacity;
static void *guarded_malloc(size_t size){
 allocation_capacity=size;unsigned char *p=malloc(size+32);
 if(p)memset(p+size,0xa5,32);return p;
}
#define malloc guarded_malloc
'''
(args.out/'diag_log_impl.inc').write_text(prefix+source[start:end]+'\n#undef malloc\n', encoding='utf-8')
print(args.out)
