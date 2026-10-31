// minimal thread-per-conn HTTP/1.1 + Range + sendfile (prototype only; no auth). Usage: sf ROOT PORT
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <strings.h>
#include <thread>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <csignal>
static std::string root;
static void reply(int c,const char*status){ char hd[256]; int hl=snprintf(hd,sizeof hd,"HTTP/1.1 %s\r\nContent-Length: 0\r\n\r\n",status); send(c,hd,hl,0); }
static bool pctdecode(const std::string&in,std::string&out){ out.clear();
 for(size_t i=0;i<in.size();i++){ if(in[i]!='%'){out+=in[i];continue;}
  if(i+2>=in.size()||!isxdigit((unsigned char)in[i+1])||!isxdigit((unsigned char)in[i+2]))return false;
  char hx[3]={in[i+1],in[i+2],0}; char ch=(char)strtol(hx,0,16); if(ch==0)return false; out+=ch; i+=2; }
 return true; }
void h(int c){ std::string req; char b[8192];
 while(req.find("\r\n\r\n")==std::string::npos){ int n=recv(c,b,sizeof b,0); if(n<=0||req.size()>65536){close(c);return;} req.append(b,n); } // full request head
 char raw[1024]; if(sscanf(req.c_str(),"GET /rest/stream/%1000s",raw)!=1){reply(c,"400 Bad Request");close(c);return;}
 std::string path; if(!pctdecode(raw,path)||path.find("..")!=std::string::npos||path.find('/')!=std::string::npos){reply(c,"400 Bad Request");close(c);return;}
 std::string p=root+"/"+path; int fd=open(p.c_str(),O_RDONLY); if(fd<0){reply(c,"404 Not Found");close(c);return;}
 struct stat st; fstat(fd,&st); long long size=st.st_size,s=0,e=size-1; bool rg=false;
 // case-insensitive "Range:" header line
 for(size_t pos=req.find("\r\n");pos!=std::string::npos&&pos+2<req.size();pos=req.find("\r\n",pos+2)){
  const char*l=req.c_str()+pos+2; if(strncasecmp(l,"range:",6))continue;
  l+=6; while(*l==' '||*l=='\t')l++; if(strncasecmp(l,"bytes=",6))break; l+=6;
  char*q; bool bad=false;
  if(*l=='-'){ long long n=strtoll(l+1,&q,10); if(q==l+1||n<=0)bad=true; else {s=n>=size?0:size-n; e=size-1;} } // suffix bytes=-N
  else { s=strtoll(l,&q,10); if(q==l||*q!='-')bad=true; else { const char*d=q+1; if(*d>='0'&&*d<='9'){ e=strtoll(d,0,10); if(e>size-1)e=size-1; } else e=size-1; } }
  if(bad||s>=size||s>e){ char hd[256]; int hl=snprintf(hd,sizeof hd,"HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%lld\r\nContent-Length: 0\r\n\r\n",size); send(c,hd,hl,0); close(fd); close(c); return; }
  rg=true; break; }
 char cr[128]=""; if(rg) snprintf(cr,sizeof cr,"Content-Range: bytes %lld-%lld/%lld\r\n",s,e,size);
 char hd[512]; int hl=snprintf(hd,sizeof hd,"HTTP/1.1 %s\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\nContent-Length: %lld\r\n%s\r\n",rg?"206 Partial Content":"200 OK",size?e-s+1:0,cr);
 send(c,hd,hl,MSG_MORE);
 off_t off=s; size_t left=size?e-s+1:0; while(left){ssize_t w=sendfile(c,fd,&off,left>(1<<20)?(1<<20):left); if(w<=0)break; left-=w;}
 close(fd); close(c);}
int main(int ac,char**av){if(ac<3)return 2; signal(SIGPIPE,SIG_IGN); root=av[1]; int s=socket(AF_INET,SOCK_STREAM,0),o=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&o,4);
 sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(atoi(av[2])); a.sin_addr.s_addr=htonl(0x7f000001);
 if(bind(s,(sockaddr*)&a,sizeof a)<0){perror("bind");return 1;} listen(s,64); printf("sendfile listening\n"); fflush(stdout); for(;;){int c=accept(s,0,0); if(c>=0) std::thread(h,c).detach();}}
