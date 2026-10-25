#include "bench_common.h"
#include "whisper.h"

typedef struct {double start,first,segment;} token_clock;
static void on_logits(struct whisper_context *ctx,struct whisper_state *state,
    const whisper_token_data *tokens,int n,float *logits,void *user) {
    (void)state;(void)logits;token_clock *clock=user;
    if(clock->first<0) for(int i=0;i<n;i++) {
        const char *text=whisper_token_to_str(ctx,tokens[i].id);
        if(tokens[i].id<whisper_token_eot(ctx) && text && strspn(text," \t\r\n")<strlen(text)) {
            clock->first=bench_now()-clock->start;break;
        }
    }
}
static void on_segment(struct whisper_context *ctx,struct whisper_state *state,int n,void *user) {
    (void)ctx;(void)state;(void)n;token_clock *clock=user;
    if(clock->segment<0)clock->segment=bench_now()-clock->start;
}
int main(int argc,char **argv) {
    if(argc!=5){fprintf(stderr,"usage: %s MODEL ID MANIFEST REPEATS\n",argv[0]);return 2;}
    bench_clip clips[64];int n=bench_clips(argv[3],clips,64);if(n<=0)return 3;
    double start=bench_now();struct whisper_context_params cp=whisper_context_default_params();
    const char *backend_path=getenv("GGML_BACKEND_PATH");
    if(backend_path)ggml_backend_load_all_from_path(backend_path);
    else ggml_backend_load_all();
    cp.use_gpu=true;cp.flash_attn=true;
    struct whisper_context *ctx=whisper_init_from_file_with_params(argv[1],cp);
    double load=bench_now()-start;if(!ctx)return 4;
    struct whisper_full_params p=whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.n_threads=8;p.language="it";p.no_context=true;p.print_realtime=false;
    p.print_progress=false;p.print_timestamps=false;p.print_special=false;
    p.greedy.best_of=1;p.temperature=0;p.temperature_inc=0;
    p.logits_filter_callback=on_logits;p.new_segment_callback=on_segment;
    FILE *extra=NULL; const char *extra_path=getenv("BENCH_WHISPER_EVENTS");if(extra_path)extra=fopen(extra_path,"a");
    for(int rep=-1;rep<atoi(argv[4]);rep++)for(int i=0;i<(rep<0?1:n);i++) {
        token_clock clock={.start=bench_now(),.first=-1,.segment=-1};
        p.logits_filter_callback_user_data=&clock;p.new_segment_callback_user_data=&clock;
        double cpu=bench_cpu();int rc=whisper_full(ctx,p,clips[i].pcm,clips[i].n);
        double total=bench_now()-clock.start,cpu_used=bench_cpu()-cpu;
        char text[65536]={0};int tokens=0;
        for(int s=0;s<whisper_full_n_segments(ctx);s++) {
            strncat(text,whisper_full_get_segment_text(ctx,s),sizeof(text)-strlen(text)-1);
            tokens+=whisper_full_n_tokens(ctx,s);
        }
        bench_emit(argv[2],"whisper.cpp Metal",&clips[i],rep,load,total,clock.first,cpu_used,
                   "provisional_decoder_token",text,tokens,rc);
        if(extra){fprintf(extra,"{\"model\":\"%s\",\"clip\":\"%s\",\"repeat\":%d,\"first_segment_ms\":%.3f}\n",argv[2],clips[i].id,rep,clock.segment);fflush(extra);}
    }
    whisper_free(ctx);if(extra)fclose(extra);return 0;
}
