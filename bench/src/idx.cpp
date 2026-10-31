#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/taglib.h>
#include <sqlite3.h>
#include <filesystem>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
namespace fs=std::filesystem;
// Usage: idx MUSIC_DIR DB      index (read-only on MUSIC_DIR)
//        idx --evict MUSIC_DIR  posix_fadvise(DONTNEED) every file: best-effort page-cache drop, no root needed
int main(int c,char**v){ if(c<3) return 2;
 if(!strcmp(v[1],"--evict")){ int n=0; for(auto&e:fs::recursive_directory_iterator(v[2],fs::directory_options::skip_permission_denied)){ if(!e.is_regular_file())continue;
   int fd=open(e.path().c_str(),O_RDONLY); if(fd<0)continue; posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED); close(fd); n++; }
   printf("evicted %d files\n",n); return 0; }
 sqlite3*db=nullptr; if(sqlite3_open(v[2],&db)!=SQLITE_OK){fprintf(stderr,"open: %s\n",db?sqlite3_errmsg(db):"oom");return 1;}
 if(sqlite3_exec(db,"PRAGMA journal_mode=WAL;CREATE TABLE IF NOT EXISTS track(id INTEGER PRIMARY KEY,path TEXT UNIQUE,mtime INT,size INT,title,artist,album,trackno INT,dur INT,bitrate INT,sr INT);BEGIN",0,0,0)!=SQLITE_OK){fprintf(stderr,"schema: %s\n",sqlite3_errmsg(db));return 1;}
 sqlite3_stmt*st=nullptr; if(sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO track(path,mtime,size,title,artist,album,trackno,dur,bitrate,sr) VALUES(?,?,?,?,?,?,?,?,?,?)",-1,&st,0)!=SQLITE_OK){fprintf(stderr,"prepare: %s\n",sqlite3_errmsg(db));return 1;}
 auto t0=std::chrono::steady_clock::now(); int n=0,fail=0;
 for(auto&e:fs::recursive_directory_iterator(v[1],fs::directory_options::skip_permission_denied)){ if(!e.is_regular_file())continue;
  TagLib::FileRef f(e.path().c_str(),true,TagLib::AudioProperties::Fast); if(f.isNull()||!f.tag())continue;
  auto*t=f.tag(); auto*a=f.audioProperties();
  sqlite3_bind_text(st,1,e.path().c_str(),-1,SQLITE_TRANSIENT);
  struct stat sb; if(stat(e.path().c_str(),&sb)!=0)continue;
  sqlite3_bind_int64(st,2,(sqlite3_int64)sb.st_mtime); sqlite3_bind_int64(st,3,(sqlite3_int64)sb.st_size); // real Unix mtime
  sqlite3_bind_text(st,4,t->title().toCString(true),-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,5,t->artist().toCString(true),-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(st,6,t->album().toCString(true),-1,SQLITE_TRANSIENT); sqlite3_bind_int(st,7,t->track());
  sqlite3_bind_int(st,8,a?a->lengthInSeconds():0); sqlite3_bind_int(st,9,a?a->bitrate():0); sqlite3_bind_int(st,10,a?a->sampleRate():0);
  if(sqlite3_step(st)!=SQLITE_DONE){ if(!fail++) fprintf(stderr,"insert: %s\n",sqlite3_errmsg(db)); } sqlite3_reset(st); n++; }
 sqlite3_finalize(st);
 if(sqlite3_exec(db,"COMMIT",0,0,0)!=SQLITE_OK){fprintf(stderr,"commit: %s\n",sqlite3_errmsg(db));return 1;} auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
 if(fail){printf("RESULT error=insert_failed count=%d\n",fail);sqlite3_close(db);return 1;}
 printf("RESULT files=%d ms=%.1f files_per_s=%.0f taglib=%d.%d sqlite=%s\n",n,ms,ms>0?n*1000/ms:0,TAGLIB_MAJOR_VERSION,TAGLIB_MINOR_VERSION,sqlite3_libversion()); sqlite3_close(db); }
