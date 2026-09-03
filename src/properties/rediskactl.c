#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char *path(void){const char*p=getenv("TB330FU_REDISKA_ADMIN_SOCKET");return p&&*p?p:"/run/tb330fu/rediska-admin.sock";}
static int connect_admin(void){int fd=socket(AF_UNIX,SOCK_STREAM,0);struct sockaddr_un a={.sun_family=AF_UNIX};if(fd<0||strlen(path())>=sizeof a.sun_path)return-1;strncpy(a.sun_path,path(),sizeof a.sun_path-1);if(connect(fd,(struct sockaddr*)&a,sizeof a)<0){close(fd);return-1;}return fd;}
static int request(const char *s,int stream){int fd=connect_admin();if(fd<0){perror(path());return 1;}if(write(fd,s,strlen(s))<0){perror("rediskactl");close(fd);return 1;}char b[4096];ssize_t n;int status=0;while((n=read(fd,b,sizeof b-1))>0){b[n]=0;fputs(b,stdout);if(stream)fflush(stdout);if(strstr(b,"NOT_FOUND"))status=1;}close(fd);if(!stream&&status)return status;return 0;}
static int get_value(const char *s){int fd=connect_admin();if(fd<0){perror(path());return 1;}if(write(fd,s,strlen(s))<0){close(fd);return 1;}char b[4096];ssize_t n=read(fd,b,sizeof b-1);close(fd);if(n<=0)return 1;b[n]=0;if(!strncmp(b,"NOT_FOUND",9))return 1;char*value=strstr(b,"OK ");if(!value)return 1;value+=3;char*e=strchr(value,'\n');if(e)*e=0;char*last=strrchr(value,' ');if(last)*last=0;last=strrchr(value,' ');if(last)*last=0;puts(value);return 0;}
static void usage(void){fprintf(stderr,"usage: rediskactl get|set|find|dump|watch|stats ...\n");}
int main(int argc,char **argv){if(argc<2){usage();return 2;}char req[2048];if(!strcmp(argv[1],"get")||!strcmp(argv[1],"find")){if(argc!=3){usage();return 2;}snprintf(req,sizeof req,"%s %s\n",!strcmp(argv[1],"get")?"GET":"FIND",argv[2]);int r=!strcmp(argv[1],"get")?get_value(req):request(req,0);if(r==1)fprintf(stderr,"rediskactl: property not found: %s\n",argv[2]);return r;}
    if(!strcmp(argv[1],"set")){if(argc!=4){usage();return 2;}snprintf(req,sizeof req,"SET %s %s\n",argv[2],argv[3]);return request(req,0);}
    if(!strcmp(argv[1],"dump")){if(argc!=2){usage();return 2;}return request("DUMP\n",0);}
    if(!strcmp(argv[1],"stats")){if(argc!=2){usage();return 2;}return request("STATS\n",0);}
    if(!strcmp(argv[1],"watch")){int n=snprintf(req,sizeof req,"WATCH");for(int i=2;i<argc;i++){if(!strcmp(argv[i],"--pid")&&i+1<argc) n+=snprintf(req+n,sizeof req-(size_t)n," pid=%s",argv[++i]);else if(!strcmp(argv[i],"--prefix")&&i+1<argc)n+=snprintf(req+n,sizeof req-(size_t)n," prefix=%s",argv[++i]);else{usage();return 2;}}if(n+2>(int)sizeof req)return 2;strcat(req,"\n");return request(req,1);}
    usage();return 2;}
