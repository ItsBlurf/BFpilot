#ifndef BFPILOT_SMB_H
#define BFPILOT_SMB_H
#include <stddef.h>
#include <stdint.h>
typedef struct bfpilot_smb_record {
 char id[24],label[128],server[256],user[128],domain[128],password[256];
 /* V1 wire layout retained. Protection word:0 required signing,1 required
  * encryption,2 explicit unsigned/guest compatibility. Not a boolean. */
 uint32_t dialect,encrypt;
} bfpilot_smb_record_t;
typedef struct bfpilot_smb_entry {
 const char *name;
 const char *display_name;
 uint64_t size,mtime,inode;
 int directory,link;
} bfpilot_smb_entry_t;
typedef struct bfpilot_smb_session bfpilot_smb_session_t;
int bfpilot_smb_path(const char *path);
int bfpilot_smb_validate_path(const char *path);
/* Record callbacks receive a scrubbed copy: no password leaves this module. */
int bfpilot_smb_records(int (*entry)(const bfpilot_smb_record_t *,void *),void *context);
int bfpilot_smb_save(bfpilot_smb_record_t *record,int replace_password);
int bfpilot_smb_remove(const char *id);
int bfpilot_smb_list(const char *path,int (*entry)(const bfpilot_smb_entry_t *,void *),void *context);
/* Sessions are owned by one calling worker, not shared between threads. */
int bfpilot_smb_connect(const char *path,bfpilot_smb_session_t **out);
void bfpilot_smb_disconnect(bfpilot_smb_session_t *session);
const char *bfpilot_smb_relative(bfpilot_smb_session_t *session);
void *bfpilot_smb_context(bfpilot_smb_session_t *session);
#endif
