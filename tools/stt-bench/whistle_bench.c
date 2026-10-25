#include "bench_common.h"
#include "needle.h"

int main(int argc,char **argv) {
    if(argc!=4) { fprintf(stderr,"usage: %s MODEL MANIFEST REPEATS\n",argv[0]); return 2; }
    bench_clip clips[64]; int n=bench_clips(argv[2],clips,64); if(n<=0)return 3;
    setenv("NEEDLE_TELEMETRY","0",1);setenv("DO_NOT_TRACK","1",1);
    double start=bench_now();
    FILE *f=fopen(argv[1],"rb"); if(!f)return 4;
    fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    unsigned char *weights=malloc(size);if(!weights)return 5;
    if(fread(weights,1,size,f)!=(size_t)size)return 5;fclose(f);
    int rc=needle_load(weights,size); double load=bench_now()-start;
    if(rc<0){fprintf(stderr,"%s\n",needle_last_error());return 6;}
    for(int rep=-1;rep<atoi(argv[3]);rep++) for(int i=0;i<(rep<0?1:n);i++) {
        char json[65536]={0};bench_clip *c=&clips[i];
        double cpu=bench_cpu();start=bench_now();
        rc=needle_transcribe(c->pcm,c->n,"it",NULL,1,json,sizeof(json));
        double total=bench_now()-start, cpu_used=bench_cpu()-cpu;
        /* Preserve the engine's full JSON in text; the report parser extracts text. */
        double first=-1;const char *p=strstr(json,"\"ttft_ms\":");if(p)first=strtod(p+10,NULL);
        bench_emit("whistle","Needle CPU",c,rep,load,total,first,cpu_used,"engine_reported_ttft",json,rc,rc<0?rc:0);
        cpu=bench_cpu();double embed_start=bench_now();
        int count=needle_embed(NULL,c->pcm,c->n,NULL,0);
        float *frames=count>0?malloc((size_t)count*sizeof(float)):NULL;
        int embed_rc=frames?needle_embed(NULL,c->pcm,c->n,frames,count):-1;
        double embed=bench_now()-embed_start;
        bench_emit("whistle-with-embeddings","Needle CPU",c,rep,load,total+embed,first,cpu_used+bench_cpu()-cpu,"engine_reported_ttft",json,rc,embed_rc<0?embed_rc:0);
        free(frames);
    }
    return 0;
}
