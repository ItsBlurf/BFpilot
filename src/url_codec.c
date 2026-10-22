#include "url_codec.h"
#include "path_ops.h"
#include <errno.h>
#include <string.h>

static int hex_value(unsigned char ch) {
  if(ch >= '0' && ch <= '9') return ch - '0';
  if(ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if(ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}
int bfpilot_url_decode(char *out, size_t capacity, const char *input, size_t length) {
  if(!out || !capacity || !input) { errno = EINVAL; return -1; }
  size_t used = 0;
  for(size_t i = 0; i < length; i++) {
    unsigned char ch = (unsigned char)input[i];
    if(ch == '%') {
      if(length - i < 3) goto invalid;
      int high = hex_value((unsigned char)input[i+1]);
      int low = hex_value((unsigned char)input[i+2]);
      if(high < 0 || low < 0) goto invalid;
      ch = (unsigned char)((high << 4) | low); i += 2;
    } else if(ch == '+') ch = ' ';
    if(!ch) goto invalid;
    if(used + 1 >= capacity) { out[0] = 0; errno = ENAMETOOLONG; return -1; }
    out[used++] = (char)ch;
  }
  out[used] = 0;
  return 0;
invalid:
  out[0] = 0; errno = EINVAL; return -1;
}
int bfpilot_form_arg(const char *body, const char *name, char *out, size_t capacity) {
  if(!body || !name || !out || !capacity) { errno = EINVAL; return 0; }
  out[0] = 0;
  for(const char *p = body; *p;) {
    const char *amp = strchr(p, '&');
    size_t length = amp ? (size_t)(amp - p) : strlen(p);
    const char *equal = memchr(p, '=', length);
    size_t key_size = equal ? (size_t)(equal - p) : length;
    char key[128];
    if(bfpilot_url_decode(key, sizeof(key), p, key_size) != 0) return 0;
    if(!strcmp(key, name)) {
      char decoded[4097];
      size_t value_size = equal ? length - key_size - 1 : 0;
      if(bfpilot_url_decode(decoded, sizeof(decoded), equal ? equal + 1 : "", value_size) != 0) return 0;
      if(!strcmp(name, "path") || !strcmp(name, "src") ||
         !strcmp(name, "dst") || !strcmp(name, "root")) {
        (void)bfpilot_path_decode_byte_tokens(decoded);
      }
      length = strlen(decoded);
      if(length >= capacity) { errno = ENAMETOOLONG; return 0; }
      memcpy(out, decoded, length + 1); return 1;
    }
    if(!amp) break;
    p = amp + 1;
  }
  return 0;
}
