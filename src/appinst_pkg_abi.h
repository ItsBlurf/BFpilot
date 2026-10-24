#pragma once
#include <stddef.h>
#include <stdint.h>
/* AppInst metadata ABI: keep the six-pointer prefix and explicitly initialize
 * the trailing slot/PlayGo fields. SDK export stubs do not describe this
 * private structure. */
typedef struct appinst_metadata {
  const char *uri;
  const char *ex_uri;
  const char *playgo_scenario_id;
  const char *content_id;
  const char *content_name;
  const char *icon_url;
  uint32_t slot;
  uint32_t is_playgo_enabled;
} appinst_metadata_t;
typedef struct appinst_pkg_info {
  char content_id[48];
  int32_t content_type;
  int32_t content_platform;
} appinst_pkg_info_t;
typedef struct appinst_playgo_info {
  char languages[30][8];
  char playgo_scenario_ids[64][3];
  char content_ids[64][48];
  unsigned char unknown[6480];
} appinst_playgo_info_t;
_Static_assert(offsetof(appinst_metadata_t, slot) == 0x30 &&
                   offsetof(appinst_metadata_t, is_playgo_enabled) == 0x34 &&
                   sizeof(appinst_metadata_t) == 0x38,
               "AppInst metadata ABI");
_Static_assert(sizeof(appinst_pkg_info_t) == 56, "AppInst pkg-info ABI");
_Static_assert(sizeof(appinst_playgo_info_t) == 9984, "AppInst PlayGo ABI");
