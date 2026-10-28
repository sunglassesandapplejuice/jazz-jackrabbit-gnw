/*
 * Headless Linux harness for the Jazz Jackrabbit (OpenJazz) homebrew.
 *
 * Runs the game through the same core, mini SDL, heap and asset pack code
 * as the Game & Watch, with a virtual clock (the game runs as fast as the
 * host can), scripted buttons and frame/audio dumps. The pack is mapped
 * read-only, like the external flash of the console.
 *
 *   ./jj_host <pack.jj> [options]
 *     -t <ms>          stop after <ms> of game time (default 30000)
 *     -s <script>      button script: "ms:BTN:1,ms:BTN:0,..." (BTN = LEFT,
 *                      RIGHT, UP, DOWN, A, B, GAME, TIME)
 *     -T <timeline>    replay a gwemu timeline/recording (down/release lines,
 *                      guest seconds); see tools/timeline_trim.py
 *     -O <seconds>     with -T: subtract this guest time base (default 0)
 *     -S <ms,ms,...>   write frame_<ms>.ppm at these times
 *     -e <ms>          also write a frame every <ms>
 *     -w <file.wav>    write the audio output
 *     -o <dir>         output directory (default .)
 *     -H <kb>          heap size in KB (default 600)
 *     -l <world:level> start at this level (difficulty screen + intro remain)
 *     -m               no music, -n: no sound effects
 *     -F <ms>          slow device: every presented frame costs <ms> more
 *     -R <dir>         render the music modules to <dir>/<NAME>.MUS (for
 *                      tools/jj_pack.py) and quit
 *     -M <rate>        with -R: 22050 or 11025
 *     -A               with -R: IMA ADPCM (default: 16 bit PCM)
 *     -q <file> -Q <ms> savestate test: save to <file> at that time
 *     -j <file> -J <ms> load <file> at that time (pause menu load)
 *     -L <file>        load this savestate at the start (like a resume)
 *     -B <file>        write every picture of the game pre-decoded to <file>
 *                      (SURFACES.BAK, for tools/jj_pack.py) and quit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#ifndef _WIN32
#include <sys/mman.h>
#else
#include <direct.h>
#endif
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "jj_platform.h"
#include "jj_pack.h"

#define RATE 22050

static uint64_t now_us;
static uint64_t end_us = 30000000ull;
static char out_dir[256] = ".";
static uint8_t last_frame[JJ_WIDTH * JJ_HEIGHT * 3];
static int last_w = JJ_WIDTH, last_h = JJ_HEIGHT;
static bool have_frame;
static uint64_t frames_presented;
static uint64_t frame_cost_us;

typedef struct { uint64_t t; uint32_t mask; int down; int seq; } btn_ev_t;
static btn_ev_t *evs;
static int n_evs, next_ev, cap_evs, ev_seq;
static uint64_t last_ev_us;
static uint32_t buttons;

static uint64_t shots[256];
static int n_shots, next_shot;
static uint64_t shot_every, next_every;

static FILE *wav;
static uint64_t audio_frames_done;
static uint32_t wav_bytes;
static int32_t peak;

static const char *save_path, *load_path, *boot_load_path;
static uint64_t save_at, load_at;
static bool saved, loaded;

static const uint8_t *pack;
static size_t pack_size;

uint32_t jj_plat_time_ms(void) { return (uint32_t)(now_us / 1000); }
int jj_plat_audio_rate(void) { return RATE; }
static char save_dir[300];
const char *jj_plat_save_dir(void) { return save_dir; }

/* the G&W puts the canvas outside its heap (LCD bonus pool) */
void *jj_plat_canvas_memory(size_t size)
{
    return calloc(1, size);
}

bool jj_plat_find_file(const char *name, const uint8_t **data, uint32_t *size)
{
    return jj_pack_find(name, data, size);
}

static void write_ppm(const char *name)
{
    if (!have_frame)
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", out_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", last_w, last_h);
    fwrite(last_frame, 3, (size_t)last_w * last_h, f);
    fclose(f);
}

static void wav_header(uint32_t data_bytes)
{
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data_bytes; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t s = 1; memcpy(h + 20, &s, 2);
    s = 1; memcpy(h + 22, &s, 2);
    v = RATE; memcpy(h + 24, &v, 4);
    v = RATE * 2; memcpy(h + 28, &v, 4);
    s = 2; memcpy(h + 32, &s, 2);
    s = 16; memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
    fseek(wav, 0, SEEK_END);
}

#ifdef JJ_LIVE_TRACE
void jj_live_dump(const char *path);
#endif

static void __attribute__((noreturn)) finish(int code)
{
#ifdef JJ_LIVE_TRACE
    {
        char p[512];
        snprintf(p, sizeof(p), "%s/live.txt", out_dir);
        jj_live_dump(p);
    }
#endif
    write_ppm("frame_end.ppm");
    if (wav) {
        wav_header(wav_bytes);
        fclose(wav);
    }
    printf("jj_host: %llu ms, %llu frames presented, audio peak %d, heap used %zu / peak %zu / size %zu\n",
           (unsigned long long)(now_us / 1000), (unsigned long long)frames_presented,
           (int)peak, jj_heap_used(), jj_heap_peak(), jj_heap_size());
    exit(code);
}

void jj_plat_fatal(const char *msg)
{
    printf("jj_host: FATAL at %llu ms: %s\n", (unsigned long long)(now_us / 1000), msg);
    finish(1);
}

void jj_plat_quit(void)
{
    printf("jj_host: game quit\n");
    finish(0);
}

static void feed_audio(void)
{
    uint64_t due = now_us * RATE / 1000000ull;
    while (audio_frames_done + 256 <= due) {
        int16_t buf[256];
        jj_core_audio_render(buf, 256);
        for (int i = 0; i < 256; i++) {
            int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > peak)
                peak = a;
        }
        if (wav) {
            fwrite(buf, 2, 256, wav);
            wav_bytes += 512;
        }
        audio_frames_done += 256;
    }
}

static int in_service;

void jj_plat_service(int in_delay)
{
    if (in_service)
        return;
    in_service = 1;
    now_us += in_delay ? 1000 : 20;

    while (next_ev < n_evs && evs[next_ev].t <= now_us) {
        if (evs[next_ev].down)
            buttons |= evs[next_ev].mask;
        else
            buttons &= ~evs[next_ev].mask;
        next_ev++;
    }
    jj_core_set_buttons(buttons);
    while (next_shot < n_shots && shots[next_shot] <= now_us) {
        char name[64];
        snprintf(name, sizeof(name), "frame_%06llu.ppm", (unsigned long long)(shots[next_shot] / 1000));
        write_ppm(name);
        next_shot++;
    }
    if (shot_every && now_us >= next_every) {
        char name[64];
        snprintf(name, sizeof(name), "frame_%06llu.ppm", (unsigned long long)(next_every / 1000));
        write_ppm(name);
        next_every += shot_every;
    }
    feed_audio();
    if (save_path && !saved && now_us >= save_at) {
        saved = true;
        bool ok = jj_core_save_state(save_path);
        printf("jj_host: saving %s at %llu ms: %s\n", save_path, (unsigned long long)(now_us / 1000), ok ? "ok" : "FAILED");
    }
    if (load_path && !loaded && now_us >= load_at) {
        loaded = true;
        bool ok = jj_core_load_state(load_path);
        printf("jj_host: loading %s at %llu ms: %s\n", load_path, (unsigned long long)(now_us / 1000), ok ? "ok" : "FAILED");
    }
    in_service = 0;
    if (now_us >= end_us)
        finish(0);
}

void jj_plat_present(const uint8_t *pixels, int w, int h, int pitch,
                     const uint8_t *palette, uint32_t pal_version)
{
    (void)pal_version;
    last_w = w > JJ_WIDTH ? JJ_WIDTH : w;
    last_h = h > JJ_HEIGHT ? JJ_HEIGHT : h;
    for (int y = 0; y < last_h; y++)
        for (int x = 0; x < last_w; x++) {
            const uint8_t *c = palette + pixels[y * pitch + x] * 4;
            uint8_t *d = last_frame + (y * last_w + x) * 3;
            d[0] = c[0];
            d[1] = c[1];
            d[2] = c[2];
        }
    have_frame = true;
    frames_presented++;
    now_us += frame_cost_us;
}

static uint32_t parse_btn(const char *s)
{
    if (!strcmp(s, "LEFT")) return JJ_BTN_LEFT;
    if (!strcmp(s, "RIGHT")) return JJ_BTN_RIGHT;
    if (!strcmp(s, "UP")) return JJ_BTN_UP;
    if (!strcmp(s, "DOWN")) return JJ_BTN_DOWN;
    if (!strcmp(s, "A")) return JJ_BTN_A;
    if (!strcmp(s, "B")) return JJ_BTN_B;
    if (!strcmp(s, "GAME")) return JJ_BTN_GAME;
    if (!strcmp(s, "TIME")) return JJ_BTN_TIME;
    fprintf(stderr, "unknown button %s\n", s);
    exit(2);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int cmp_ev(const void *a, const void *b)
{
    const btn_ev_t *x = a, *y = b;
    if (x->t != y->t)
        return x->t < y->t ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static int eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* gwemu logical button names (hw/misc/gnw_h7b0_gpio.c), case-insensitive.
 * start/select are what the firmware maps to GAME/TIME on the device;
 * pause/pwr are emulator-side and do not reach the game. */
static uint32_t parse_btn_timeline(const char *s)
{
    if (eq_ci(s, "left")) return JJ_BTN_LEFT;
    if (eq_ci(s, "right")) return JJ_BTN_RIGHT;
    if (eq_ci(s, "up")) return JJ_BTN_UP;
    if (eq_ci(s, "down")) return JJ_BTN_DOWN;
    if (eq_ci(s, "a")) return JJ_BTN_A;
    if (eq_ci(s, "b")) return JJ_BTN_B;
    if (eq_ci(s, "game") || eq_ci(s, "start")) return JJ_BTN_GAME;
    if (eq_ci(s, "time") || eq_ci(s, "select")) return JJ_BTN_TIME;
    if (eq_ci(s, "pause") || eq_ci(s, "pwr")) return 0;
    fprintf(stderr, "jj_host: timeline: unknown button %s\n", s);
    return 0;
}

static void add_ev(uint64_t t_us, uint32_t mask, int down)
{
    if (n_evs == cap_evs) {
        cap_evs = cap_evs ? cap_evs * 2 : 256;
        evs = realloc(evs, (size_t)cap_evs * sizeof(*evs));
        if (!evs) {
            perror("realloc");
            exit(1);
        }
    }
    evs[n_evs++] = (btn_ev_t){ t_us, mask, down, ev_seq++ };
    if (t_us > last_ev_us)
        last_ev_us = t_us;
}

static void add_btn_list(const char *list, uint64_t t_us, int down)
{
    char *s = strdup(list);
    if (!s) {
        perror("strdup");
        exit(1);
    }
    for (char *tok = strtok(s, "+"); tok; tok = strtok(NULL, "+")) {
        uint32_t mask = parse_btn_timeline(tok);
        if (mask)
            add_ev(t_us, mask, down);
    }
    free(s);
}

/* "[MM:]SS[.fff]" */
static int parse_time(const char *s, double *out)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s)
        return 0;
    if (*end == ':') {
        char *end2;
        double sec = strtod(end + 1, &end2);
        if (end2 == end + 1 || *end2)
            return 0;
        v = v * 60.0 + sec;
    } else if (*end) {
        return 0;
    }
    *out = v;
    return 1;
}

static void load_timeline(const char *path, double base)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "jj_host: cannot open timeline %s: %s\n",
                path, strerror(errno));
        exit(1);
    }
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        char *ts = strtok(line, " \t\r\n");
        if (!ts)
            continue;
        char *act = strtok(NULL, " \t\r\n");
        if (!act)
            continue;
        char *arg = strtok(NULL, " \t\r\n");
        double t;
        if (!parse_time(ts, &t)) {
            fprintf(stderr, "jj_host: %s:%d: bad time %s\n", path, lineno, ts);
            exit(2);
        }
        if (!strcmp(act, "quit"))
            break;
        if (!strcmp(act, "screenshot") || !strcmp(act, "checkpoint"))
            continue;
        if (!arg) {
            fprintf(stderr, "jj_host: %s:%d: %s needs a button\n",
                    path, lineno, act);
            continue;
        }
        int down;
        double dur = 0.0;
        if (!strcmp(act, "down")) {
            down = 1;
        } else if (!strcmp(act, "release")) {
            down = 0;
        } else if (!strcmp(act, "press")) {
            down = 1;
            dur = 0.1;
        } else if (!strcmp(act, "hold")) {
            down = 1;
            char *d = strtok(NULL, " \t\r\n");
            if (!d || (dur = strtod(d, NULL)) <= 0) {
                fprintf(stderr, "jj_host: %s:%d: hold needs a duration\n",
                        path, lineno);
                continue;
            }
        } else {
            fprintf(stderr, "jj_host: %s:%d: ignoring action %s\n",
                    path, lineno, act);
            continue;
        }
        double rel = t - base;
        if (rel < 0.0)
            continue; /* before the cut */
        uint64_t us = (uint64_t)(rel * 1000000.0 + 0.5);
        add_btn_list(arg, us, down);
        if (down && dur > 0.0)
            add_btn_list(arg, us + (uint64_t)(dur * 1000000.0 + 0.5), 0);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    size_t heap_kb = 600;
    int start_world = -1, start_level = -1;
    bool music = true, sound = true;
    const char *bake_out = NULL;
    const char *render_dir = NULL;
    const char *timeline_path = NULL;
    double timeline_base = 0.0;
    bool end_set = false;
    int render_rate = RATE;
    bool render_adpcm = false;
    if (argc < 2) {
        fprintf(stderr, "usage: %s <pack.jj> [options]\n", argv[0]);
        return 2;
    }
    const char *pack_path = argv[1];
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "-t") && v) { end_us = strtoull(v, NULL, 10) * 1000; end_set = true; i++; }
        else if (!strcmp(a, "-o") && v) { snprintf(out_dir, sizeof(out_dir), "%s", v); i++; }
        else if (!strcmp(a, "-H") && v) { heap_kb = strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "-l") && v) { if (sscanf(v, "%d:%d", &start_world, &start_level) != 2) return 2; i++; }
        else if (!strcmp(a, "-m")) { music = false; }
        else if (!strcmp(a, "-n")) { sound = false; }
        else if (!strcmp(a, "-B") && v) { bake_out = v; i++; }
        else if (!strcmp(a, "-R") && v) { render_dir = v; i++; }
        else if (!strcmp(a, "-M") && v) { render_rate = atoi(v); i++; }
        else if (!strcmp(a, "-A")) { render_adpcm = true; }
        else if (!strcmp(a, "-q") && v) { save_path = v; i++; }
        else if (!strcmp(a, "-Q") && v) { save_at = strtoull(v, NULL, 10) * 1000; i++; }
        else if (!strcmp(a, "-j") && v) { load_path = v; i++; }
        else if (!strcmp(a, "-J") && v) { load_at = strtoull(v, NULL, 10) * 1000; i++; }
        else if (!strcmp(a, "-L") && v) { boot_load_path = v; i++; }
        else if (!strcmp(a, "-w") && v) { wav = fopen(v, "wb"); if (wav) { uint8_t z[44] = {0}; fwrite(z, 1, 44, wav); } i++; }
        else if (!strcmp(a, "-e") && v) { shot_every = strtoull(v, NULL, 10) * 1000; next_every = shot_every; i++; }
        else if (!strcmp(a, "-F") && v) { frame_cost_us = strtoull(v, NULL, 10) * 1000; i++; }
        else if (!strcmp(a, "-S") && v) {
            char *s = strdup(v);
            for (char *tok = strtok(s, ","); tok && n_shots < 256; tok = strtok(NULL, ","))
                shots[n_shots++] = strtoull(tok, NULL, 10) * 1000;
            qsort(shots, n_shots, sizeof(shots[0]), cmp_u64);
            i++;
        } else if (!strcmp(a, "-T") && v) {
            timeline_path = v;
            i++;
        } else if (!strcmp(a, "-O") && v) {
            timeline_base = strtod(v, NULL);
            i++;
        } else if (!strcmp(a, "-s") && v) {
            char *s = strdup(v);
            for (char *tok = strtok(s, ","); tok; tok = strtok(NULL, ",")) {
                char key[32];
                unsigned long long t;
                int down;
                if (sscanf(tok, "%llu:%31[^:]:%d", &t, key, &down) != 3) {
                    fprintf(stderr, "bad script item %s\n", tok);
                    return 2;
                }
                add_ev(t * 1000, parse_btn(key), down);
            }
            free(s);
            i++;
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            return 2;
        }
    }

    if (timeline_path)
        load_timeline(timeline_path, timeline_base);
    if (n_evs > 1)
        qsort(evs, n_evs, sizeof(evs[0]), cmp_ev);
    if (timeline_path)
        printf("jj_host: timeline %s: %d events, last at %llu ms\n",
               timeline_path, n_evs, (unsigned long long)(last_ev_us / 1000));
    /* A timeline is usually longer than the default 30 s run. */
    if (timeline_path && !end_set && end_us < last_ev_us + 5000000ull)
        end_us = last_ev_us + 5000000ull;

#ifdef _WIN32
    FILE *pf = fopen(pack_path, "rb");
    if (!pf) {
        perror(pack_path);
        return 1;
    }
    fseek(pf, 0, SEEK_END);
    pack_size = (size_t)ftell(pf);
    fseek(pf, 0, SEEK_SET);
    uint8_t *pbuf = malloc(pack_size);
    if (!pbuf || fread(pbuf, 1, pack_size, pf) != pack_size) {
        perror(pack_path);
        return 1;
    }
    fclose(pf);
    pack = pbuf;
#else
    /* read-only, like the flash of the console */
    int fd = open(pack_path, O_RDONLY);
    if (fd < 0) {
        perror(pack_path);
        return 1;
    }
    struct stat st;
    fstat(fd, &st);
    pack_size = (size_t)st.st_size;
    pack = mmap(NULL, pack_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (pack == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
#endif
    int r = jj_pack_open(pack, (uint32_t)pack_size);
    if (r != JJ_PACK_OK) {
        fprintf(stderr, "invalid pack (%d)\n", r);
        return 1;
    }

    if (render_dir) {
        extern int jj_render_music(const char *dir, int rate, bool adpcm);
        return jj_render_music(render_dir, render_rate, render_adpcm) > 0 ? 0 : 1;
    }

    static uint8_t *heap;
    heap = malloc(heap_kb * 1024);
    jj_heap_init(heap, heap_kb * 1024);
    jj_heap_ignore_range(pack, pack + pack_size);

    snprintf(save_dir, sizeof(save_dir), "%s/save", out_dir);
#ifdef _WIN32
    _mkdir(save_dir);
#else
    mkdir(save_dir, 0755);
#endif

    jj_core_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.music = music;
    opts.sound = sound;
    opts.start_world = start_world;
    opts.start_level = start_level;
    opts.bake_out = bake_out;
    if (boot_load_path)
        printf("jj_host: boot load %s: %s\n", boot_load_path, jj_core_load_state(boot_load_path) ? "ok" : "FAILED");
    jj_core_run(&opts); /* does not return */
    return 0;
}
