/* Execute exact production manifest code under a test-only data-root override. */
#include "../src/paths.h"
#undef BFPILOT_ARCHIVE_INTEGRATED_DIR
#define BFPILOT_ARCHIVE_INTEGRATED_DIR "/data/test/bfpilot-manifest-controls-r8"
#include "../src/archive_worker.cpp"
#include <ps5/payload.h>
static bool fail_sync=false;
extern "C" int bfpilot_full_archive_ack(uint64_t){return 0;}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){if(fail_sync){errno=EIO;return -1;}return __real_fsync(fd);}
static FILE *report;static int failures;
static void check(const char *name,bool pass){fprintf(report,"%s %s\n",pass?"PASS":"FAIL",name);fflush(report);failures+=!pass;}
static bool replace_marker(const std::string &text){FILE *f=fopen(BFPILOT_FULL_ACTIVE,"wb");if(!f)return false;bool ok=fwrite(text.data(),1,text.size(),f)==text.size();return fclose(f)==0 && ok;}
int main(){
 const std::string root=BFPILOT_ARCHIVE_DIR;
 if(mkdir(root.c_str(),0700)!=0)return 2;
 report=fopen((root+"/results.txt").c_str(),"wx");if(!report)return 3;
 const std::string parent=root+"/.bfpilot-staging",stage=parent+"/extract-owned";
 if(mkdir(parent.c_str(),0700)!=0 || mkdir(stage.c_str(),0700)!=0)return 4;
 g_status.source="/data/test/bfpilot-engine-fixtures-r1/aes.zip";
 g_status.destination=root+"/published";g_status.stage=stage;
 int rc=SaveActiveFullArchive(12345);
 ActiveFullArchive active;std::string error,original;
 bool loaded=LoadActiveFullArchive(active,error);
 check("durable marker roundtrip exact ownership",rc==0 && loaded && active.id==12345 && active.stage==stage && active.source==g_status.source && active.destination==g_status.destination);
 if(!ReadSmallFile(BFPILOT_FULL_ACTIVE,original,16384))return 5;
 fail_sync=true;rc=SaveActiveFullArchive(54321);fail_sync=false;
 std::string after;ReadSmallFile(BFPILOT_FULL_ACTIVE,after,16384);
 check("failed marker sync preserves prior published marker",rc==-EIO && after==original);
 std::string moved=stage+"-moved";rename(stage.c_str(),moved.c_str());mkdir(stage.c_str(),0700);
 active=ActiveFullArchive();error.clear();loaded=LoadActiveFullArchive(active,error);
 check("replaced stage inode refused",!loaded && error.find("identity changed")!=std::string::npos);
 rmdir(stage.c_str());rename(moved.c_str(),stage.c_str());
 rename(stage.c_str(),g_status.destination.c_str());
 active=ActiveFullArchive();error.clear();loaded=LoadActiveFullArchive(active,error);
 check("post-publication marker recognizes exact destination inode",loaded && active.stage_missing && active.published);
 rename(g_status.destination.c_str(),moved.c_str());mkdir(g_status.destination.c_str(),0700);
 active=ActiveFullArchive();error.clear();loaded=LoadActiveFullArchive(active,error);
 check("missing stage never adopts unrelated destination",loaded && active.stage_missing && !active.published);
 rmdir(g_status.destination.c_str());rename(moved.c_str(),stage.c_str());
 replace_marker(original.substr(0,12));active=ActiveFullArchive();error.clear();
 check("truncated marker refused",!LoadActiveFullArchive(active,error));
 replace_marker(original+"source=/data/unrelated.zip\n");active=ActiveFullArchive();error.clear();
 check("duplicate identity field refused",!LoadActiveFullArchive(active,error) && error.find("duplicate")!=std::string::npos);
 std::string bad=original;size_t start=bad.find("source=");size_t end=bad.find('\n',start);
 bad.replace(start,end-start,"source=/data/../unrelated.zip");replace_marker(bad);active=ActiveFullArchive();error.clear();
 check("parent traversal source refused",!LoadActiveFullArchive(active,error));
 replace_marker(original);g_status.helper_quiescent=false;ClearActiveFullArchive();struct stat st;
 check("uncertain helper cannot clear recovery marker",lstat(BFPILOT_FULL_ACTIVE,&st)==0);
 unlink(BFPILOT_FULL_ACTIVE);mkdir(BFPILOT_FULL_ACTIVE,0700);g_status.helper_quiescent=true;
 ClearActiveFullArchive();check("failed marker removal retains archive ownership",g_full_marker_pending);
 rmdir(BFPILOT_FULL_ACTIVE);replace_marker(original);
 g_status.helper_quiescent=true;ClearActiveFullArchive();
 check("confirmed quiescent helper clears marker",lstat(BFPILOT_FULL_ACTIVE,&st)!=0 && errno==ENOENT);
 rmdir(stage.c_str());rmdir(parent.c_str());
 fprintf(report,"SUMMARY failures=%d\n",failures);fclose(report);payload_exit(failures!=0);return failures!=0;
}
