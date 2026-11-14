/* The chunk a Maze Voice 1.0.5 project holds: the engine's "state" text plus its NUL (mpc-vst-plugins wrapper/vst2_wrap.c
 * effGetChunk at b6e2197 returns eng_get("state") with strlen + 1). Built against the TAG's src/maze_voice.c by
 * tests/make_legacy_chunks.sh, with a few values moved off their defaults so a load is visible. Writes the chunk to stdout. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef struct {
    uint32_t api_version;
    void *(*create_instance)(const char *, const char *);
    void (*destroy_instance)(void *);
    void (*on_midi)(void *, const uint8_t *, int, int);
    void (*set_param)(void *, const char *, const char *);
    int (*get_param)(void *, const char *, char *, int);
    int (*get_error)(void *, char *, int);
    void (*render_block)(void *, int16_t *, int);
} api_t;
extern api_t *move_plugin_init_v2(const void *host);
int main(void) {
    api_t *a = move_plugin_init_v2(NULL);
    void *v = a->create_instance(".", NULL);
    const char *kv[][2] = {{"vco_tune", "7"}, {"cutoff", "31"}, {"reso", "66"}, {"route", "2"}, {"out_mode", "1"},
                           {"fold_drive", "42"}, {"level", "77"}, {"rnd_filter", "1"}, {"lfo1_rate", "3.5"}};
    for (size_t i = 0; i < sizeof kv / sizeof kv[0]; i++) a->set_param(v, kv[i][0], kv[i][1]);
    char buf[1200];
    int n = a->get_param(v, "state", buf, sizeof buf);
    if (n <= 0) return 1;
    fwrite(buf, 1, strlen(buf) + 1, stdout);
    return 0;
}
