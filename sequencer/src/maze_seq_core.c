/* =============================================================================
 * MAZE  -  Overtake (tool) DSP for Ableton Move (Schwung)     [id: maze_seq]
 * -----------------------------------------------------------------------------
 * Dual 8-step generative sequencer (Moog Labyrinth style), clock-synced to the
 * Move transport, MIDI out per sequencer, with disk persistence.
 * Mirrors the stock tb3po.c tool DSP (plugin_api_v2, move_plugin_init_v2).
 *
 * FORCE PORT: this file is schwung-maze/src/maze_seq/dsp/maze_seq.c, verbatim,
 * except for (1) the header include, swapped for the trimmed
 * maze_seq_host.h (see that file's own comment - the core only ever calls
 * host->midi_send_internal, nothing else needs to be provided), and (2) the
 * state-file path, which is now runtime-set from module_dir instead of a
 * hardcoded Move path - both changes are marked FORCE-ONLY below. Everything
 * else, including every set_param/get_param key and the sequencing logic
 * itself, is unchanged. In particular s1_channel/s2_channel (independent
 * per-sequencer MIDI output channel) were ALREADY here in the upstream
 * module - unlike schwung-acid, this "tool" component talks to
 * host->midi_send_internal directly and was never subject to Move's chain-
 * host one-channel-per-slot restriction, so there was nothing to patch to
 * get independent Seq A / Seq B output channels on the Force. See
 * ../DESIGN.md.
 *
 * REALTIME SAFETY (important):
 *   set_param / get_param / create_instance / on_midi / render_block all run on
 *   the SPI AUDIO CALLBACK. File I/O there causes device-wide audio dropouts.
 *   So state SAVES happen on a background SCHED_OTHER worker thread: the audio
 *   thread only sets a dirty flag (set_param "save"), and the worker does the
 *   fopen/fwrite. Load happens once in create_instance (one-time, like tb3po).
 *   (On the Force there is no SPI callback at all - host_shim.cpp calls these
 *   from an RtMidi callback and a control-socket thread instead - but the
 *   worker-thread save design is harmless and unchanged here, and keeping it
 *   verbatim means nothing about the save/load logic itself needed touching.)
 *
 * TRANSPOSE MODEL:
 *   - "key" (0..11) = pitch class, owned by the UI (Key knob).
 *   - "pad_semis"   = semitone transpose from the pad keyboard.
 *   - root = 60 + key + transpose(oct buttons) + pad_semis.
 *   Incoming notes do NOT set key (that fought the knob); the UI owns it.
 *
 * RANGES: corrupt and cv_range are 0..100.
 * Persistence: "<module_dir>/maze_seq.bin" (FORCE-ONLY path; version 3).
 *
 * You (a non-coder) only ever need to touch bits marked  ==>> EDIT ME.
 * ===========================================================================*/

#include "maze_seq_host.h"   /* FORCE-ONLY: was "plugin_api_v1.h" */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>

#define NUM_STEPS   8

/* FORCE-ONLY: Move's hardcoded /data/UserData/schwung/tool_state path
 * doesn't exist on the Force. maze_create() below points g_state_path at
 * "<module_dir>/maze_seq.bin" instead (module_dir is the shim's own already-
 * -existing addon directory), falling back to this default only if
 * module_dir is NULL. Nothing else about persistence changes. */
#define MAZE_STATE_PATH_DEFAULT "/data/UserData/schwung/tool_state/maze_seq.bin"
#define MAZE_STATE_MAGIC 0x455A414Du   /* "MAZE" */
#define MAZE_STATE_VERSION 3u
static char g_state_path[512] = MAZE_STATE_PATH_DEFAULT;

static uint32_t rng_state = 0x1234abcdu;
static inline uint32_t rng_next(void){ uint32_t x=rng_state; x^=x<<13; x^=x>>17; x^=x<<5; rng_state=x; return x; }
static inline float rng_f(void){ return (rng_next()>>8)*(1.0f/16777216.0f); }
static inline float rng_bip(void){ return rng_f()*2.0f-1.0f; }

/* ---- Scales  (==>> EDIT ME: keep in sync with SCALES list in ui.js) ---- */
typedef struct { const int8_t *pc; int n; } scale_t;
static const int8_t SC_CHROM[]={0,1,2,3,4,5,6,7,8,9,10,11};
static const int8_t SC_MAJ[]  ={0,2,4,5,7,9,11};
static const int8_t SC_MIN[]  ={0,2,3,5,7,8,10};
static const int8_t SC_PMAJ[] ={0,2,4,7,9};
static const int8_t SC_PMIN[] ={0,3,5,7,10};
static const int8_t SC_MEL[]  ={0,2,3,5,7,9,11};
static const int8_t SC_HARM[] ={0,2,3,5,7,8,11};
static const int8_t SC_WHOLE[]={0,2,4,6,8,10};
static const int8_t SC_HIRA[] ={0,2,3,7,8};
static const int8_t SC_MAJ7[] ={0,4,7,11};
static const int8_t SC_MIN7[] ={0,3,7,10};
static const int8_t SC_UNQ[]  ={0,1,2,3,4,5,6,7,8,9,10,11};
static const scale_t SCALES[]={
    {SC_CHROM,12},{SC_MAJ,7},{SC_MIN,7},{SC_PMAJ,5},{SC_PMIN,5},
    {SC_MEL,7},{SC_HARM,7},{SC_WHOLE,6},{SC_HIRA,5},{SC_MAJ7,4},{SC_MIN7,4},{SC_UNQ,12}
};
#define NUM_SCALES ((int)(sizeof(SCALES)/sizeof(SCALES[0])))

static const int RATE_PULSES[]={3,6,12,24,48,96};
#define NUM_RATES ((int)(sizeof(RATE_PULSES)/sizeof(RATE_PULSES[0])))
static const float GATE_STEPS[]={0.25f,0.5f,0.75f,1.0f,1.25f,1.5f,1.75f,2.0f};
#define NUM_GATES ((int)(sizeof(GATE_STEPS)/sizeof(GATE_STEPS[0])))

/* Sequence Reset (Reset Both knob): snap both play heads to step 1 every N BARS.
   UI sends index 0..4 -> 1,2,4,8 bars, 0 = off. One bar = 96 clock pulses;
   every note rate divides it evenly, so N bars = N*(96/RATE_PULSES) steps.
   Keep in sync with RESET_LABELS in ui.js. */
#define PULSES_PER_BAR 96
static const int RESET_BARS[]={1,2,4,8,0};
#define NUM_RESETS ((int)(sizeof(RESET_BARS)/sizeof(RESET_BARS[0])))
static inline int reset_bars_from_idx(int idx){
    if (idx<0) idx=0;
    if (idx>=NUM_RESETS) idx=NUM_RESETS-1;
    return RESET_BARS[idx];
}
static inline int reset_idx_from_bars(int bars){
    for (int i=0;i<NUM_RESETS;i++) if (RESET_BARS[i]==bars) return i;
    return NUM_RESETS-1;   /* unknown -> off */
}

#define MAX_SPREAD 64   /* ==>> EDIT ME: semitone spread each side of root */

#ifdef MAZE_LFO
/* ---- FORCE-ONLY: 2 LFOs x 8 fixed destinations, mirroring maze-voice's own
 * MAZE_LFO block (src/maze_voice.c). Compiled only when the build defines
 * MAZE_LFO (force-maze/scripts/build.sh), so a byte-for-byte diff against
 * schwung-maze's maze_seq.c stays possible. There's no per-sample audio path
 * here (this is a MIDI-only sequencer - see maze_render_block below), so the
 * LFOs are advanced once per incoming MIDI clock pulse (0xF8, 24 ppqn)
 * instead of once per audio block; tempo for "Sync" mode is estimated from
 * the wall-clock gap between clock pulses (see lfo_advance()). ---- */
enum { LFO_D_CORRUPT1, LFO_D_RANGE1, LFO_D_LENGTH1, LFO_D_TRIGMIX,
       LFO_D_CORRUPT2, LFO_D_RANGE2, LFO_D_LENGTH2, LFO_D_NOTELEN, LFO_NDEST };
static const char* const LFO_DEST_KEY[LFO_NDEST] = {
    "corrupt1","range1","length1","trigmix","corrupt2","range2","length2","notelen" };
/* sync divisions, in beats per cycle: 1/16 1/8 1/4 1/2 1bar 2bars 4bars 8bars */
static const float LFO_DIV_BEATS[8] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f };
typedef struct {
    int   shape;    /* 0 saw, 1 tri, 2 sine, 3 square, 4 S&H */
    float rate;     /* 0..1 knob -> 0.02..30 Hz (log), free-run */
    int   sync, div, retrig;
    float depth[LFO_NDEST];   /* -1..1 */
    double phase;
    float  sh;      /* held S&H value */
    float  val;     /* current output, -1..1 */
} lfo_t;
static inline float clampf(float v, float lo, float hi){ return v<lo?lo:(v>hi?hi:v); }
static inline float lfo_wave(const lfo_t* l){
    float p = (float)l->phase;
    switch (l->shape){
    case 0:  return 2.0f * p - 1.0f;
    case 1:  return 4.0f * fabsf(p - 0.5f) - 1.0f;
    case 2:  return sinf(2.0f * (float)M_PI * p);
    case 3:  return p < 0.5f ? 1.0f : -1.0f;
    default: return l->sh;
    }
}
#endif

typedef struct {
    int   bit[NUM_STEPS];
    float cv [NUM_STEPS];
    int   length, play, corrupt, cv_range;   /* corrupt/cv_range: 0..100 */
    int   channel;
    int   reset_bars, reset_ctr;             /* reset_bars: bars per reset, 0 = off; reset_ctr counts steps */
    int   last_note, last_ch; long off_pulse; int note_active;
} seq_t;

typedef struct {
    const host_api_v1_t *host;
    seq_t s[2];
    int   scale, key, rate, gate, trig_mix;
    int   root, transpose;
    int   pad_semis;
    int   running, suspended;
    long  pulse;

#ifdef MAZE_LFO
    lfo_t  lfo[2];
    float  lfoBpm;         /* estimated from the incoming MIDI clock gap */
    double lfoLastPulseT;  /* wall-clock time of the previous 0xF8, 0 = none yet */
#endif

    /* background state saver */
    pthread_t       state_thread;
    int             state_thread_started;
    volatile int    state_thread_stop;
    volatile int    state_dirty;
    pthread_mutex_t state_mutex;
} maze_t;

static const host_api_v1_t *g_host = NULL;

static void seq_randomize(seq_t *q){
    q->length=NUM_STEPS; q->play=-1;
    for (int i=0;i<NUM_STEPS;i++){ q->bit[i]=(rng_f()<0.5f)?1:0; q->cv[i]=rng_bip(); }
}
static void seq_corrupt(seq_t *q, int idx, int corrupt){
    float c=corrupt/100.0f;
    float p_cv =(c<=0.5f)?(c*0.5f):(0.25f+(c-0.5f)*0.5f);
    float p_bit=(c<=0.5f)?0.0f:((c-0.5f));
    if (rng_f()<p_cv) q->cv[idx]=rng_bip();
    if (p_bit>0.0f && rng_f()<p_bit){ q->bit[idx]=!q->bit[idx]; if(q->bit[idx]) q->cv[idx]=rng_bip(); }
}
static int quantize_note(const maze_t *L, float cv, int cv_range){
    float scaled=cv*(cv_range/100.0f);
    int semi=(int)lrintf(scaled*MAX_SPREAD);
    int note=L->root+semi;
    if (note<0) note=0; if (note>127) note=127;
    const scale_t *sc=&SCALES[L->scale];
    int root_pc=((L->root%12)+12)%12;
    int pc=((note%12)+12)%12, best_pc=root_pc, bd=128;
    for (int i=0;i<sc->n;i++){
        int cand=(root_pc+sc->pc[i])%12;
        int d=abs(pc-cand); if(d>6)d=12-d;
        if(d<bd){ bd=d; best_pc=cand; }
    }
    int diff=best_pc-pc; if(diff>6)diff-=12; if(diff<-6)diff+=12;
    note+=diff;
    if (note<0) note+=12; if (note>127) note-=12;
    return note;
}
static void trig_velocities(int trig_mix, int *v1, int *v2){
    if (trig_mix<=0){
        float f=(trig_mix+63)/63.0f;
        *v1=(int)lrintf(127.0f-27.0f*f);
        *v2=(trig_mix==-63)?0:(int)lrintf(1.0f+99.0f*f);
    } else {
        float f=trig_mix/64.0f;
        *v1=(int)lrintf(100.0f*(1.0f-f));
        *v2=(int)lrintf(100.0f+27.0f*f);
    }
    if(*v1<0)*v1=0; if(*v1>127)*v1=127; if(*v2<0)*v2=0; if(*v2>127)*v2=127;
}

static void send_midi(maze_t *L, uint8_t status, uint8_t d1, uint8_t d2){
    if (!L->host || !L->host->midi_send_internal) return;
    uint8_t pkt[4]={ (uint8_t)((status>>4)&0x0F), status, d1, d2 };
    L->host->midi_send_internal(pkt,4);
}
static void seq_note_off(maze_t *L, seq_t *q){
    if (q->note_active){ send_midi(L,0x80|(q->last_ch&0x0F),q->last_note&0x7F,0); q->note_active=0; }
}
static void all_notes_off(maze_t *L){ seq_note_off(L,&L->s[0]); seq_note_off(L,&L->s[1]); }

static void step_seq(maze_t *L, int which, int vel, int corrupt, int cv_range, int length, int gate){
    seq_t *q=&L->s[which];
    int n=length<1?1:length;
    /* Sequence Reset: every reset_bars bars, snap the play head back to step 1.
       steps/bar = 96/RATE_PULSES at the current note rate. */
    if (q->reset_bars>0){
        int thresh = q->reset_bars * (PULSES_PER_BAR / RATE_PULSES[L->rate]);
        if (thresh>0 && q->reset_ctr>=thresh){ q->play=-1; q->reset_ctr=0; }
    }
    q->play=(q->play+1)%n;
    q->reset_ctr++;
    seq_corrupt(q,q->play,corrupt);
    if (q->bit[q->play] && vel>0){
        seq_note_off(L,q);
        int note=quantize_note(L,q->cv[q->play],cv_range);
        send_midi(L,0x90|(q->channel&0x0F),note&0x7F,vel&0x7F);
        q->last_note=note; q->last_ch=q->channel; q->note_active=1;
        q->off_pulse=L->pulse+(long)lrintf(GATE_STEPS[gate]*RATE_PULSES[L->rate]);
        if (q->off_pulse<=L->pulse) q->off_pulse=L->pulse+1;
    }
}
static void flush_offs(maze_t *L){
    for (int i=0;i<2;i++){ seq_t *q=&L->s[i]; if(q->note_active && L->pulse>=q->off_pulse) seq_note_off(L,q); }
}
static void transport_reset(maze_t *L){
    L->pulse=0;
    L->s[0].play=-1; L->s[1].play=-1;
    L->s[0].reset_ctr=0; L->s[1].reset_ctr=0;
}
static void set_root_from_key(maze_t *L){
    int r=60+(L->key%12)+L->transpose+L->pad_semis;
    if(r<0)r=0; if(r>127)r=127; L->root=r;
}

#ifdef MAZE_LFO
/* Advance both LFOs by one MIDI-clock pulse and accumulate each destination's
 * signed offset (sum over both LFOs of val * depth, still -1..1-ish) into
 * off[]. Called once per 0xF8, whether or not this pulse lands on a step. */
static void lfo_advance(maze_t *L, float off[LFO_NDEST]){
    for (int d=0; d<LFO_NDEST; ++d) off[d]=0.0f;
#ifdef MAZE_VST   /* MPC-VST-ONLY: pulses arrive in bursts per audio block; tempo comes from the host (set_param "host_bpm") */
    double dt = 60.0/((double)L->lfoBpm*24.0);
    double now = 0.0;
    if (0){
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + (double)ts.tv_nsec*1e-9;
    double dt = 60.0/(120.0*24.0); /* nominal fallback: 120bpm, 24ppqn */
    if (L->lfoLastPulseT > 0.0){
#endif
        double gap = now - L->lfoLastPulseT;
        if (gap > 0.0005 && gap < 2.0){
            dt = gap;
            L->lfoBpm = clampf((float)(60.0/(gap*24.0)), 20.0f, 300.0f);
        }
    }
    L->lfoLastPulseT = now;
    for (int n=0; n<2; ++n){
        lfo_t *l=&L->lfo[n];
        float hz = l->sync ? (L->lfoBpm/60.0f)/LFO_DIV_BEATS[l->div] : 0.02f*powf(1500.0f,l->rate);
        l->phase += (double)hz*dt;
        if (l->phase >= 1.0){ l->phase -= floorf((float)l->phase); if(l->phase>=1.0) l->phase=0.0; l->sh=rng_bip(); }
        l->val = lfo_wave(l);
        for (int d=0; d<LFO_NDEST; ++d) off[d] += l->val*l->depth[d];
    }
}
#endif

/* ---- persistence (runs on the WORKER thread, never the audio callback) ---- */
/* FORCE-ONLY: derive the directory from g_state_path instead of the old
 * fixed MAZE_STATE_DIR constant, since g_state_path is now set at runtime
 * (see maze_create() below). */
static int ensure_state_dir(void){
    char dir[512];
    strncpy(dir, g_state_path, sizeof(dir)-1); dir[sizeof(dir)-1]='\0';
    char *slash = strrchr(dir, '/');
    if (!slash) return 1;         /* no directory component - nothing to create */
    if (slash == dir) return 1;   /* path is "/xxx" - root always exists */
    *slash = '\0';
    struct stat st;
    if (stat(dir,&st)==0 && S_ISDIR(st.st_mode)) return 1;
    if (mkdir(dir,0755)==0) return 1;
    return (stat(dir,&st)==0 && S_ISDIR(st.st_mode));
}
static void write_seq(FILE *f, const seq_t *q){
    int32_t i;
    for (int k=0;k<NUM_STEPS;k++){ i=(int32_t)q->bit[k]; fwrite(&i,4,1,f); }
    for (int k=0;k<NUM_STEPS;k++){ float v=q->cv[k]; fwrite(&v,4,1,f); }
    i=q->length;  fwrite(&i,4,1,f);
    i=q->play;    fwrite(&i,4,1,f);
    i=q->corrupt; fwrite(&i,4,1,f);
    i=q->cv_range;fwrite(&i,4,1,f);
    i=q->channel; fwrite(&i,4,1,f);
}
static int read_seq(FILE *f, seq_t *q){
    int32_t i; float v;
    for (int k=0;k<NUM_STEPS;k++){ if(fread(&i,4,1,f)!=1) return 0; q->bit[k]=i?1:0; }
    for (int k=0;k<NUM_STEPS;k++){ if(fread(&v,4,1,f)!=1) return 0; q->cv[k]=v; }
    if(fread(&i,4,1,f)!=1) return 0; q->length  =(i<1?1:(i>8?8:i));
    if(fread(&i,4,1,f)!=1) return 0; q->play    =i;
    if(fread(&i,4,1,f)!=1) return 0; q->corrupt =(i<0?0:(i>100?100:i));
    if(fread(&i,4,1,f)!=1) return 0; q->cv_range=(i<0?0:(i>100?100:i));
    if(fread(&i,4,1,f)!=1) return 0; q->channel =(i<0?0:(i>15?15:i));
    return 1;
}
/* Do the actual file write. Guarded by state_mutex so the worker and the
   final destroy-time save can't race on the same FILE*. */
static void maze_save_state_locked(maze_t *L){
    if (!L || !ensure_state_dir()) return;
    FILE *f=fopen(g_state_path,"wb");
    if(!f) return;
    uint32_t u; int32_t i;
    u=MAZE_STATE_MAGIC;   fwrite(&u,4,1,f);
    u=MAZE_STATE_VERSION; fwrite(&u,4,1,f);
    i=L->scale;    fwrite(&i,4,1,f);
    i=L->key;      fwrite(&i,4,1,f);
    i=L->rate;     fwrite(&i,4,1,f);
    i=L->gate;     fwrite(&i,4,1,f);
    i=L->trig_mix; fwrite(&i,4,1,f);
    i=L->transpose;fwrite(&i,4,1,f);
    i=L->pad_semis;fwrite(&i,4,1,f);
    i=L->root;     fwrite(&i,4,1,f);
    write_seq(f,&L->s[0]);
    write_seq(f,&L->s[1]);
    fclose(f);
}
static void maze_save_state(maze_t *L){
    if(!L) return;
    pthread_mutex_lock(&L->state_mutex);
    maze_save_state_locked(L);
    pthread_mutex_unlock(&L->state_mutex);
}
static int maze_load_state(maze_t *L){
    FILE *f=fopen(g_state_path,"rb");
    if(!f) return 0;
    uint32_t magic=0,ver=0; int32_t i;
    if(fread(&magic,4,1,f)!=1||fread(&ver,4,1,f)!=1||magic!=MAZE_STATE_MAGIC||ver!=MAZE_STATE_VERSION){ fclose(f); return 0; }
    if(fread(&i,4,1,f)!=1){fclose(f);return 0;} L->scale=(i<0?0:(i>=NUM_SCALES?NUM_SCALES-1:i));
    if(fread(&i,4,1,f)!=1){fclose(f);return 0;} L->key=((i%12)+12)%12;
    if(fread(&i,4,1,f)!=1){fclose(f);return 0;} L->rate=(i<0?0:(i>=NUM_RATES?NUM_RATES-1:i));
    if(fread(&i,4,1,f)!=1){fclose(f);return 0;} L->gate=(i<0?0:(i>=NUM_GATES?NUM_GATES-1:i));
    if(fread(&i,4,1,f)!=1){fclose(f);return 0;} L->trig_mix=(i<-63?-63:(i>64?64:i));
    if(fread(&i,4,1,f)!=1){fclose(f);return 0;} L->transpose=(i<-48?-48:(i>48?48:i));
    if(fread(&i,4,1,f)!=1){fclose(f);return 0;} L->pad_semis=(i<-60?-60:(i>60?60:i));
    if(fread(&i,4,1,f)!=1){fclose(f);return 0;} L->root=(i<0?0:(i>127?127:i));
    if(!read_seq(f,&L->s[0])){fclose(f);return 0;}
    if(!read_seq(f,&L->s[1])){fclose(f);return 0;}
    fclose(f);
    return 1;
}

/* Background worker: wake every ~2s, flush if dirty. Demotes itself off the
 * audio priority it inherits, and keeps core 3 free for SPI. */
static void *maze_state_worker(void *arg){
    maze_t *L=(maze_t*)arg;
    struct sched_param sp; sp.sched_priority=0;
    sched_setscheduler(0, SCHED_OTHER, &sp);     /* MUST be first */
#ifdef CPU_ZERO
    cpu_set_t set; CPU_ZERO(&set);
    CPU_SET(0,&set); CPU_SET(1,&set); CPU_SET(2,&set);   /* not core 3 */
    sched_setaffinity(0, sizeof(set), &set);
#endif
    while(!L->state_thread_stop){
        for(int i=0;i<10 && !L->state_thread_stop;i++) usleep(200*1000); /* ~2s */
        if(L->state_thread_stop) break;
        if(L->state_dirty){ maze_save_state(L); L->state_dirty=0; }
    }
    return NULL;
}

/* =============================================================================
 *  Plugin ABI (plugin_api_v2)
 * ===========================================================================*/
static void *maze_create(const char *module_dir, const char *json_defaults){
    (void)json_defaults;
    /* FORCE-ONLY: point persistence at "<module_dir>/maze_seq.bin" - the
     * shim's own addon directory - instead of Move's hardcoded
     * /data/UserData path (see MAZE_STATE_PATH_DEFAULT above). host_shim.cpp
     * always passes a real module_dir, so the DEFAULT above is really only
     * a documentation fallback. */
#ifndef MAZE_VST
    if (module_dir && module_dir[0]) {
        snprintf(g_state_path, sizeof(g_state_path), "%s/maze_seq.bin", module_dir);
    }
#endif
    maze_t *L=(maze_t*)calloc(1,sizeof(maze_t));
    if(!L) return NULL;
    L->host=g_host;
    rng_state^=(uint32_t)(uintptr_t)L|0x9e3779b9u;
    L->scale=1; L->key=0; L->rate=1; L->gate=3; L->trig_mix=0;
    L->transpose=0; L->root=60;
    seq_randomize(&L->s[0]); seq_randomize(&L->s[1]);
    L->s[0].cv_range=20; L->s[0].channel=0;   /* ==>> EDIT ME: default Seq1 ch */
    L->s[1].cv_range=20; L->s[1].channel=0;   /* ==>> EDIT ME: default Seq2 ch */
    L->s[0].reset_bars=0; L->s[1].reset_bars=0; /* default: off (never reset) */

#ifdef MAZE_LFO
    for (int n=0;n<2;++n){ memset(&L->lfo[n],0,sizeof(lfo_t)); L->lfo[n].shape=2; L->lfo[n].rate=0.4f; L->lfo[n].div=2; }
    L->lfoBpm=120.0f; L->lfoLastPulseT=0.0;
#endif

    pthread_mutex_init(&L->state_mutex, NULL);
#ifndef MAZE_VST   /* MPC-VST-ONLY: no shared state file; the project chunk carries the pattern */
    maze_load_state(L);                        /* one-time load (like tb3po) */
#endif

    L->running=0; L->suspended=0; L->pulse=0;

    L->state_thread_stop=0; L->state_dirty=0;
#ifndef MAZE_VST
    /* start the background saver */
    if (pthread_create(&L->state_thread, NULL, maze_state_worker, L)==0)
        L->state_thread_started=1;
#endif
    return L;
}
static void maze_destroy(void *inst){
    maze_t *L=(maze_t*)inst;
    if(!L) return;
    all_notes_off(L);
#ifndef MAZE_VST
    /* stop the worker, then one final synchronous save */
    L->state_thread_stop=1;
    if(L->state_thread_started) pthread_join(L->state_thread, NULL);
    maze_save_state(L);
#endif
    pthread_mutex_destroy(&L->state_mutex);
    free(L);
}

static void maze_on_midi(void *inst, const uint8_t *msg, int len, int source){
    (void)source;
    maze_t *L=(maze_t*)inst;
    if(!L||!msg||len<1) return;
    uint8_t st=msg[0];
    if (st==0xFA){ transport_reset(L); L->running=1; return; }
    if (st==0xFB){ L->running=1; return; }
    if (st==0xFC){ L->running=0; all_notes_off(L); return; }
    if (st==0xF8){
        if(!L->running) return;
        L->pulse++;
        flush_offs(L);
#ifdef MAZE_LFO
        float lfoOff[LFO_NDEST];
        lfo_advance(L,lfoOff);
#endif
        if (L->pulse % RATE_PULSES[L->rate] == 0){
            int trigMix=L->trig_mix;
            int corrupt1=L->s[0].corrupt, range1=L->s[0].cv_range, length1=L->s[0].length;
            int corrupt2=L->s[1].corrupt, range2=L->s[1].cv_range, length2=L->s[1].length;
            int gate=L->gate;
#ifdef MAZE_LFO
            trigMix  = (int)lrintf(clampf((float)trigMix  + lfoOff[LFO_D_TRIGMIX]*63.0f, -63.0f, 64.0f));
            corrupt1 = (int)lrintf(clampf((float)corrupt1 + lfoOff[LFO_D_CORRUPT1]*50.0f, 0.0f, 100.0f));
            range1   = (int)lrintf(clampf((float)range1   + lfoOff[LFO_D_RANGE1]*50.0f, 0.0f, 100.0f));
            length1  = (int)lrintf(clampf((float)length1  + lfoOff[LFO_D_LENGTH1]*3.5f, 1.0f, 8.0f));
            corrupt2 = (int)lrintf(clampf((float)corrupt2 + lfoOff[LFO_D_CORRUPT2]*50.0f, 0.0f, 100.0f));
            range2   = (int)lrintf(clampf((float)range2   + lfoOff[LFO_D_RANGE2]*50.0f, 0.0f, 100.0f));
            length2  = (int)lrintf(clampf((float)length2  + lfoOff[LFO_D_LENGTH2]*3.5f, 1.0f, 8.0f));
            gate     = (int)lrintf(clampf((float)gate      + lfoOff[LFO_D_NOTELEN]*3.5f, 0.0f, 7.0f));
#endif
            int v1,v2; trig_velocities(trigMix,&v1,&v2);
            step_seq(L,0,v1,corrupt1,range1,length1,gate);
            step_seq(L,1,v2,corrupt2,range2,length2,gate);
        }
        return;
    }
    /* incoming notes intentionally do NOT set key (UI owns it). */
}

#ifdef MAZE_VST   /* MPC-VST-ONLY: Advance rotates the pattern (gates + CV of the active steps) one step; works while stopped */
static void seq_rotate(seq_t *q, int dir){
    int n=q->length<1?1:(q->length>NUM_STEPS?NUM_STEPS:q->length);
    if (n<2) return;
    int b0=q->bit[dir>0?n-1:0]; float c0=q->cv[dir>0?n-1:0];
    if (dir>0){ for (int i=n-1;i>0;i--){ q->bit[i]=q->bit[i-1]; q->cv[i]=q->cv[i-1]; } q->bit[0]=b0; q->cv[0]=c0; }
    else      { for (int i=0;i<n-1;i++){ q->bit[i]=q->bit[i+1]; q->cv[i]=q->cv[i+1]; } q->bit[n-1]=b0; q->cv[n-1]=c0; }
}
#endif
static void maze_set_param(void *inst, const char *key, const char *val){
    maze_t *L=(maze_t*)inst;
    if(!L||!key||!val) return;
    int v=atoi(val);
    if      (!strcmp(key,"s1_corrupt"))  L->s[0].corrupt=(v<0?0:(v>100?100:v));
    else if (!strcmp(key,"s1_cv_range")) L->s[0].cv_range=(v<0?0:(v>100?100:v));
    else if (!strcmp(key,"s1_length"))   L->s[0].length=(v<1?1:(v>8?8:v));
    else if (!strcmp(key,"s1_channel"))  L->s[0].channel=(v<0?0:(v>15?15:v));
    else if (!strcmp(key,"s1_flip")){ int p=(v<0||v>7)?0:v; L->s[0].bit[p]=!L->s[0].bit[p]; if(L->s[0].bit[p])L->s[0].cv[p]=rng_bip(); }
#ifdef MAZE_VST
    else if (!strcmp(key,"s1_adv")){ seq_rotate(&L->s[0], v<0?-1:1); }
#else
    else if (!strcmp(key,"s1_adv")){ int n=L->s[0].length<1?1:L->s[0].length; L->s[0].play=((L->s[0].play+(v<0?-1:1))%n+n)%n; }
#endif
    else if (!strcmp(key,"s1_len_dec")){ L->s[0].length=(L->s[0].length<=1)?8:L->s[0].length-1; }
    else if (!strcmp(key,"s2_corrupt"))  L->s[1].corrupt=(v<0?0:(v>100?100:v));
    else if (!strcmp(key,"s2_cv_range")) L->s[1].cv_range=(v<0?0:(v>100?100:v));
    else if (!strcmp(key,"s2_length"))   L->s[1].length=(v<1?1:(v>8?8:v));
    else if (!strcmp(key,"s2_channel"))  L->s[1].channel=(v<0?0:(v>15?15:v));
    else if (!strcmp(key,"s2_flip")){ int p=(v<0||v>7)?0:v; L->s[1].bit[p]=!L->s[1].bit[p]; if(L->s[1].bit[p])L->s[1].cv[p]=rng_bip(); }
#ifdef MAZE_VST
    else if (!strcmp(key,"s2_adv")){ seq_rotate(&L->s[1], v<0?-1:1); }
#else
    else if (!strcmp(key,"s2_adv")){ int n=L->s[1].length<1?1:L->s[1].length; L->s[1].play=((L->s[1].play+(v<0?-1:1))%n+n)%n; }
#endif
    else if (!strcmp(key,"s2_len_dec")){ L->s[1].length=(L->s[1].length<=1)?8:L->s[1].length-1; }
    else if (!strcmp(key,"trig_mix"))    L->trig_mix=(v<-63?-63:(v>64?64:v));
    /* Reset Both: one global reset length (in bars) for both sequencers. */
    else if (!strcmp(key,"g_reset")){ int rb=reset_bars_from_idx(v); L->s[0].reset_bars=rb; L->s[1].reset_bars=rb; }
    else if (!strcmp(key,"scale"))       L->scale=(v<0?0:(v>=NUM_SCALES?NUM_SCALES-1:v));
    else if (!strcmp(key,"key")){ L->key=((v%12)+12)%12; set_root_from_key(L); }
    else if (!strcmp(key,"note_rate"))   L->rate=(v<0?0:(v>=NUM_RATES?NUM_RATES-1:v));
    else if (!strcmp(key,"note_length")) L->gate=(v<0?0:(v>=NUM_GATES?NUM_GATES-1:v));
    else if (!strcmp(key,"transpose")){ L->transpose=(v<-48?-48:(v>48?48:v)); set_root_from_key(L); }
    else if (!strcmp(key,"pad_semis")){ L->pad_semis=(v<-60?-60:(v>60?60:v)); set_root_from_key(L); }
#ifdef MAZE_VST   /* MPC-VST-ONLY */
    else if (!strcmp(key,"host_bpm")){ float b=(float)atof(val); if (b>=20.0f && b<=300.0f) L->lfoBpm=b; }
    else if (!strcmp(key,"song_pulse")){
        /* MPC-VST-ONLY: anchor the pulse counter to the song position. val = index of the 0xF8 about to arrive (ppq*24), so
           `pulse % RATE_PULSES` -- and the reset-every-N-bars counter -- are functions of the song position, not of how many
           pulses happened to arrive since Start. A playing note keeps its length (off_pulse is rebased). */
        long m=atol(val); if (m<0) m=0;
        long np=m-1;
        for (int i=0;i<2;i++){
            seq_t *q=&L->s[i];
            if (q->note_active){ long rem=q->off_pulse-L->pulse; if (rem<1) rem=1; q->off_pulse=np+rem; }
            if (q->reset_bars>0){
                int thresh=q->reset_bars*(PULSES_PER_BAR/RATE_PULSES[L->rate]);
                if (thresh>0) q->reset_ctr=(int)((m/RATE_PULSES[L->rate])%thresh);
            }
        }
        L->pulse=np;
    }
    else if (!strcmp(key,"s1_regen")||!strcmp(key,"s2_regen")){
        seq_t *q=&L->s[key[1]=='2'?1:0]; int len=q->length, ch=q->channel, cr=q->corrupt, rg=q->cv_range, rb=q->reset_bars;
        seq_randomize(q); q->length=len; q->channel=ch; q->corrupt=cr; q->cv_range=rg; q->reset_bars=rb;
    }
    else if (!strcmp(key,"pattern")){   /* "<bits>:<cv>:<cv>...|<bits>:..." as written by get_param */
        const char *p=val;
        for (int n=0;n<2;n++){
            seq_t *q=&L->s[n]; int i=0;
            for (;i<NUM_STEPS && (*p=='0'||*p=='1');i++,p++) q->bit[i]=(*p=='1');
            for (int k=0;k<NUM_STEPS;k++){ if (*p!=':') break; p++; q->cv[k]=clampf((float)atof(p),-1.0f,1.0f); while(*p && *p!=':' && *p!='|') p++; }
            if (*p=='|') p++;
        }
    }
#endif
    else if (!strcmp(key,"suspend")){ L->suspended=(v!=0); }
    else if (!strcmp(key,"panic")){ all_notes_off(L); }
    /* RT-safe save: just mark dirty; the worker thread does the file I/O. */
    else if (!strcmp(key,"save")){ L->state_dirty=1; }
#ifdef MAZE_LFO
    else if (!strncmp(key,"lfo",3) && (key[3]=='1'||key[3]=='2') && key[4]=='_'){
        lfo_t *l=&L->lfo[key[3]-'1']; const char *k=key+5;
        float lf=(float)v;
        if      (!strcmp(k,"shape"))  l->shape=(int)clampf(lf,0,4);
        else if (!strcmp(k,"rate"))   l->rate=clampf(lf/100.0f,0,1);
        else if (!strcmp(k,"sync"))   l->sync=(v!=0);
        else if (!strcmp(k,"div"))    l->div=(int)clampf(lf,0,7);
        else if (!strcmp(k,"retrig")) l->retrig=(v!=0);
        else for (int d=0; d<LFO_NDEST; ++d) if (!strcmp(k,LFO_DEST_KEY[d])) l->depth[d]=clampf(lf/100.0f,-1,1);
    }
#endif
}

/* Remote UI (Tool tab) contract, see docs/MODULES.md "Remote UI for overtake
 * tools": answer get_param("module_id") so the manager can discover us, and
 * get_param("state") with a flat JSON object of string values so the manager
 * seeds AND periodically refreshes the browser (toolTickLoop polls "state"
 * on an adaptive ~100ms-while-active cadence for tools that don't implement
 * the optional rui_poll fast path — this module doesn't, and doesn't need
 * to: an 8-step sequencer has no business updating faster than that).
 * s1_bits/s2_bits/s1_play/s2_play/running mirror ui.js's own s1_state/
 * s2_state polling (same underlying data, JSON instead of pipe-delimited,
 * for the remote-ui side specifically). Every other field here mirrors a
 * knob ui.js already owns (see U{} in ui.js) so the remote panel and the
 * on-device screen never disagree about what a knob is currently set to.
 *
 * FORCE PORT: this is exactly the endpoint the web/ control panel polls
 * (GET /state -> ctrl socket "GET state" -> this) to draw the step grids and
 * play-head - no on-device screen exists here, but the same JSON contract
 * turned out to be exactly what a browser step-grid poller wants too. */
static int build_state_json(const maze_t *L, char *buf, int buf_len){
    char b1[NUM_STEPS+1], b2[NUM_STEPS+1];
    for (int i=0;i<NUM_STEPS;i++){ b1[i]=L->s[0].bit[i]?'1':'0'; b2[i]=L->s[1].bit[i]?'1':'0'; }
    b1[NUM_STEPS]='\0'; b2[NUM_STEPS]='\0';
    int g_reset_idx = reset_idx_from_bars(L->s[0].reset_bars);
    int n = snprintf(buf, (size_t)buf_len,
        "{\"running\":\"%d\","
        "\"s1_bits\":\"%s\",\"s1_play\":\"%d\",\"s1_length\":\"%d\",\"s1_corrupt\":\"%d\",\"s1_cv_range\":\"%d\",\"s1_channel\":\"%d\","
        "\"s2_bits\":\"%s\",\"s2_play\":\"%d\",\"s2_length\":\"%d\",\"s2_corrupt\":\"%d\",\"s2_cv_range\":\"%d\",\"s2_channel\":\"%d\","
        "\"trig_mix\":\"%d\",\"scale\":\"%d\",\"key\":\"%d\",\"note_rate\":\"%d\",\"note_length\":\"%d\",\"g_reset\":\"%d\","
        "\"pad_semis\":\"%d\"}",
        L->running?1:0,
        b1, L->s[0].play, L->s[0].length, L->s[0].corrupt, L->s[0].cv_range, L->s[0].channel,
        b2, L->s[1].play, L->s[1].length, L->s[1].corrupt, L->s[1].cv_range, L->s[1].channel,
        L->trig_mix, L->scale, L->key, L->rate, L->gate, g_reset_idx,
        L->pad_semis);
    if (n<0) return -1; if (n>=buf_len) n=buf_len-1;
    return n;
}
static int maze_get_param(void *inst, const char *key, char *buf, int buf_len){
    maze_t *L=(maze_t*)inst;
    if(!L||!key||!buf||buf_len<2) return -1;
    int n=0;
    if (!strcmp(key,"running")) n=snprintf(buf,buf_len,"%d",L->running?1:0);
    else if (!strcmp(key,"module_id")) n=snprintf(buf,buf_len,"maze_seq");
    else if (!strcmp(key,"state")) return build_state_json(L,buf,buf_len);
#ifdef MAZE_LFO
    else if (!strncmp(key,"lfo",3) && (key[3]=='1'||key[3]=='2') && key[4]=='_'){
        const lfo_t *l=&L->lfo[key[3]-'1']; const char *k=key+5;
        if (!strcmp(k,"shape"))  n=snprintf(buf,buf_len,"%d",l->shape);
        else if (!strcmp(k,"rate"))   n=snprintf(buf,buf_len,"%.4g",l->rate*100.0f);
        else if (!strcmp(k,"sync"))   n=snprintf(buf,buf_len,"%d",l->sync);
        else if (!strcmp(k,"div"))    n=snprintf(buf,buf_len,"%d",l->div);
        else if (!strcmp(k,"retrig")) n=snprintf(buf,buf_len,"%d",l->retrig);
        else {
            int found=0;
            for (int d=0; d<LFO_NDEST; ++d) if (!strcmp(k,LFO_DEST_KEY[d])){ n=snprintf(buf,buf_len,"%.4g",l->depth[d]*100.0f); found=1; break; }
            if (!found) return -1;
        }
    }
#endif
#ifdef MAZE_VST   /* MPC-VST-ONLY */
    else if (!strcmp(key,"pattern")){
        int off=0;
        for (int n=0;n<2;n++){
            const seq_t *q=&L->s[n];
            for (int i=0;i<NUM_STEPS && off<buf_len-2;i++) buf[off++]=q->bit[i]?'1':'0';
            for (int k=0;k<NUM_STEPS && off<buf_len-16;k++) off+=snprintf(buf+off,buf_len-off,":%.5f",q->cv[k]);
            if (n==0 && off<buf_len-2) buf[off++]='|';
        }
        buf[off]='\0'; return off;
    }
#endif
    else if (!strcmp(key,"s1_state")||!strcmp(key,"s2_state")){
        seq_t *q=&L->s[key[1]=='2'?1:0];
        int off=snprintf(buf,buf_len,"%d|",q->length);
        for (int i=0;i<NUM_STEPS && off<buf_len-2;i++)
            off+=snprintf(buf+off,buf_len-off,"%s%d", i?",":"", q->bit[i]);
        if (off<buf_len-2) off+=snprintf(buf+off,buf_len-off,"|%d", q->play);
        n=off;
    }
    else return -1;
    if (n<0) return -1; if (n>=buf_len) n=buf_len-1;
    return n;
}
static int maze_get_error(void *inst, char *buf, int buf_len){ (void)inst;(void)buf;(void)buf_len; return 0; }
static void maze_render_block(void *inst, int16_t *out, int frames){
    (void)inst;
    if (out && frames>0) memset(out,0,sizeof(int16_t)*frames*2);
}

static plugin_api_v2_t g_api = {
    .api_version      = MOVE_PLUGIN_API_VERSION_2,
    .create_instance  = maze_create,
    .destroy_instance = maze_destroy,
    .on_midi          = maze_on_midi,
    .set_param        = maze_set_param,
    .get_param        = maze_get_param,
    .get_error        = maze_get_error,
    .render_block     = maze_render_block,
};
plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host){
    g_host = host;
    return &g_api;
}
