#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "ascolto/local_stt.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static bool stop(void *unused) { (void)unused; return true; }
int main(int argc, char **argv) {
    if(argc!=2)return 2;
    void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    typedef int (*prepare_t)(int,int,bool(*)(void*),void*,char*,size_t);
    prepare_t prepare=library?(prepare_t)dlsym(library,"ascolto_stt_prepare_package_v1"):NULL;
    if(!prepare)return 1;
    char directory[]="/tmp/ascolto-prepare-abi-XXXXXX";if(!mkdtemp(directory))return 1;
    int fd=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(fd<0)return 1;
    int input=openat(fd,"model.safetensors",O_CREAT|O_EXCL|O_RDWR,0600);if(input<0)return 1;
    char error[256]={0};
    int failed=prepare(fd,input,NULL,NULL,error,sizeof(error));
    /* This exact size error proves the real dylib found and checked its bundled
     * recipe/resources before rejecting the deliberately empty source. */
    int result=!failed||!strstr(error,"file type or size mismatch");
    error[0]=0;
    if(!prepare(fd,input,stop,NULL,error,sizeof(error))||!strstr(error,"cancelled"))result=1;
    close(input);unlinkat(fd,"model.safetensors",0);close(fd);rmdir(directory);dlclose(library);
    printf("Native BF16 preparation ABI/resources: %s (no weights or inference)\n",result?"FAIL":"PASS");
    return result;
}
