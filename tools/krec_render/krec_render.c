/* krec_render - headless .krec -> raw video/audio renderer (prototype).
 *
 * Drives pcsx_rearmed_libretro.dll the same way tools/krec_harness does
 * (and retroarch-k3's Kaillera path): one .krec input record per
 * retro_run(), 12 bytes per player, empty record = neutral input, every
 * port RETRO_DEVICE_JOYPAD, kaillera_sync.c's forced core options, MultiTap
 * slot order. It also checks the anti-desync RAM digest against the
 * players' in-band "[SYNC] ... d=<window>:<hash>" lines, so a render proves
 * it is the match the players saw.
 *
 * Video: every Nth core frame, nearest-neighbour scaled to a fixed WxH
 * canvas, written to stdout as raw rgb565le (pipe it into ffmpeg).
 * Audio: the core's s16le stereo stream (44100 Hz for PCSX) to a file.
 *
 * Meant to run under Wine on the broadcaster VM with no X display, or
 * natively on Windows. Build: cl /O2 /MT krec_render.c
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <io.h>
#include <fcntl.h>

/* ---- minimal libretro ABI ------------------------------------------------ */
struct retro_variable { const char *key; const char *value; };
struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };
struct retro_game_geometry { unsigned base_width, base_height, max_width, max_height; float aspect_ratio; };
struct retro_system_timing { double fps, sample_rate; };
struct retro_system_av_info { struct retro_game_geometry geometry; struct retro_system_timing timing; };
typedef void (*retro_log_printf_t)(int level, const char *fmt, ...);
struct retro_log_callback { retro_log_printf_t log; };

typedef int (*env_t)(unsigned, void *);
typedef void (*video_t)(const void *, unsigned, unsigned, size_t);
typedef void (*audio_t)(int16_t, int16_t);
typedef size_t (*audiob_t)(const int16_t *, size_t);
typedef void (*poll_t)(void);
typedef int16_t (*state_t)(unsigned, unsigned, unsigned, unsigned);

static void (*p_set_environment)(env_t);
static void (*p_set_video_refresh)(video_t);
static void (*p_set_audio_sample)(audio_t);
static void (*p_set_audio_sample_batch)(audiob_t);
static void (*p_set_input_poll)(poll_t);
static void (*p_set_input_state)(state_t);
static void (*p_init)(void);
static int (*p_load_game)(const struct retro_game_info *);
static void (*p_get_system_av_info)(struct retro_system_av_info *);
static void (*p_set_controller_port_device)(unsigned, unsigned);
static void (*p_run)(void);
static void *(*p_get_memory_data)(unsigned);
static size_t (*p_get_memory_size)(unsigned);

/* ---- core options -------------------------------------------------------- */
#define MAX_OPTS 256
static char *opt_key[MAX_OPTS], *opt_val[MAX_OPTS];
static int n_opts;

static void opt_set(const char *k, const char *v, int overwrite)
{
    int i;
    for (i = 0; i < n_opts; i++)
        if (!strcmp(opt_key[i], k)) {
            if (overwrite) { free(opt_val[i]); opt_val[i] = _strdup(v); }
            return;
        }
    if (n_opts < MAX_OPTS) { opt_key[n_opts] = _strdup(k); opt_val[n_opts] = _strdup(v); n_opts++; }
}

static const char *opt_get(const char *k)
{
    int i;
    for (i = 0; i < n_opts; i++)
        if (!strcmp(opt_key[i], k)) return opt_val[i];
    return NULL;
}

/* Keep in sync with FORCED in tools/krec_harness/krec_harness.py
 * (= ksync_forced_options[] in retroarch-k3 kaillera/kaillera_sync.c). */
static const char *FORCED[][2] = {
    {"pcsx_rearmed_memcard1", "none"}, {"pcsx_rearmed_memcard2", "none"},
    {"pcsx_rearmed_bios", "auto"}, {"pcsx_rearmed_show_bios_bootlogo", "disabled"},
    {"pcsx_rearmed_region", "auto"}, {"pcsx_rearmed_drc", "enabled"},
    {"pcsx_rearmed_psxclock", "57"}, {"pcsx_rearmed_nostalls", "disabled"},
    {"pcsx_rearmed_icache_emulation", "enabled"}, {"pcsx_rearmed_exception_emulation", "disabled"},
    {"pcsx_rearmed_nosmccheck", "disabled"}, {"pcsx_rearmed_gteregsunneeded", "disabled"},
    {"pcsx_rearmed_nogteflags", "disabled"}, {"pcsx_rearmed_gpu_slow_llists", "auto"},
    {"pcsx_rearmed_gpu_thread_rendering", "auto"}, {"pcsx_rearmed_cd_turbo", "disabled"},
    {"pcsx_rearmed_noxadecoding", "enabled"}, {"pcsx_rearmed_nocdaudio", "enabled"},
    {"pcsx_rearmed_spu_reverb", "enabled"}, {"pcsx_rearmed_frameskip_type", "disabled"},
    {"pcsx_rearmed_multitap", "disabled"}, {"pcsx_rearmed_multitap1", "disabled"},
    {"pcsx_rearmed_multitap2", "disabled"}, {"pcsx_rearmed_analog_axis_modifier", "square"},
    {"pcsx_rearmed_input_sensitivity", "1.00"},
};

static void load_opt_file(const char *path)
{
    char line[1024];
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "aviso: sem %s\n", path); return; }
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *k = line, *v, *e;
        if (!eq) continue;
        *eq = 0; v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        for (e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*v == ' ' || *v == '\t' || *v == '"') v++;
        for (e = v + strlen(v); e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '"'); ) *--e = 0;
        opt_set(k, v, 1);
    }
    fclose(f);
}

/* ---- .krec --------------------------------------------------------------- */
typedef struct { const uint8_t *p; int len; } rec_t;
static rec_t *frames; static int n_frames;
static int numplayers, multitap;
#define DIGEST_FRAMES 300
typedef struct { uint64_t h[4]; int n; } want_t;
static want_t *want; static int n_want;

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void parse_sync(const char *msg)
{
    const char *s = strstr(msg, "[SYNC] "), *d, *e;
    int epoch = 0, w, i; uint64_t h = 0;
    if (!s) return;
    e = strstr(s, " e=");
    d = strstr(s, " d=");
    if (!d) return;
    if (e && e < d) epoch = atoi(e + 3);
    if (epoch != 0) return;
    w = atoi(d + 3);
    d = strchr(d, ':');
    if (!d) return;
    for (i = 1; i <= 16; i++) { int x = hexval(d[i]); if (x < 0) return; h = (h << 4) | (uint64_t)x; }
    if (w < 0 || w >= n_want) return;
    if (want[w].n < 4) want[w].h[want[w].n++] = h;
}

static uint8_t *load_krec(const char *path)
{
    FILE *f = fopen(path, "rb"); long sz; uint8_t *d; int p, hdr, chats = 0, cap;
    if (!f) { fprintf(stderr, "nao abriu %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    d = malloc(sz + 1); fread(d, 1, sz, f); fclose(f); d[sz] = 0;
    if (memcmp(d, "KRC1", 4) && memcmp(d, "KRC0", 4)) { fprintf(stderr, "nao e .krec\n"); exit(2); }
    hdr = !memcmp(d, "KRC1", 4) ? 400 : 272;
    numplayers = *(int32_t *)(d + 268);
    cap = (int)(sz / 3) + 1;
    frames = malloc(sizeof(rec_t) * cap);
    n_want = cap / DIGEST_FRAMES + 2;
    want = calloc(n_want, sizeof(want_t));
    p = hdr;
    while (p < sz) {
        int t = d[p++];
        if (t == 0x12) {
            int16_t l = *(int16_t *)(d + p); p += 2;
            if (l < 0 || p + l > sz) break;
            frames[n_frames].p = d + p; frames[n_frames].len = l; n_frames++; p += l;
        } else if (t == 0x08) {
            const char *nick = (const char *)d + p, *msg;
            p += (int)strlen(nick) + 1; msg = (const char *)d + p; p += (int)strlen(msg) + 1;
            if (chats++ < 4 && !strcmp(nick, "n02") && !strncmp(msg, "[MultiTap]", 10) && !strstr(msg, "desativado"))
                multitap = 1;
            parse_sync(msg);
        } else if (t == 0x14) {
            p += (int)strlen((const char *)d + p) + 1 + 4;
        } else break;
    }
    if (numplayers < 3) multitap = 0;
    return d;
}

/* ---- input --------------------------------------------------------------- */
static int16_t joy[8][6];
static const int MT_SLOT_OF_PORT[8] = {0, 2, 3, 4, 1, 5, 6, 7};

static void set_input(const rec_t *r)
{
    int16_t slots[8][6]; int p;
    memset(slots, 0, sizeof slots);
    if (r->len) for (p = 0; p < 8 && p < r->len / 12; p++) memcpy(slots[p], r->p + p * 12, 12);
    for (p = 0; p < 8; p++) memcpy(joy[p], slots[multitap ? MT_SLOT_OF_PORT[p] : p], 12);
}

static int16_t cb_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port >= 8) return 0;
    device &= 0xFF;
    if (device == 1) return id == 256 ? joy[port][1] : (joy[port][1] >> (id & 0xF)) & 1;
    if (device == 5) return index == 0 ? joy[port][2 + (id & 1)] : joy[port][4 + (id & 1)];
    return 0;
}
static void cb_poll(void) {}

/* ---- video / audio ------------------------------------------------------- */
static int pixfmt = 0;              /* 0=0RGB1555 1=XRGB8888 2=RGB565 */
static int out_w = 640, out_h = 480;
static const void *last_data; static unsigned last_w, last_h; static size_t last_pitch;

/* Video goes out through a writer thread with a small queue. ffmpeg reads
 * the video pipe and the audio FIFO separately; if the emulation thread
 * blocked on a video write it would stop producing audio, and ffmpeg (which
 * may be waiting for a full audio packet first) would never read the video:
 * a deadlock. With the queue the emulation keeps feeding audio while video
 * drains; it only waits when QN frames are already pending. */
#define QN 12
static uint16_t *slot[QN];
static int q_head, q_tail, q_count, q_done, q_failed;
static CRITICAL_SECTION q_cs;
static CONDITION_VARIABLE q_notempty, q_notfull;
static HANDLE vout;

static DWORD WINAPI video_writer(void *arg)
{
    const DWORD bytes = (DWORD)((size_t)out_w * out_h * 2);
    (void)arg;
    for (;;) {
        const uint8_t *buf; DWORD done = 0, n;
        EnterCriticalSection(&q_cs);
        while (!q_count && !q_done) SleepConditionVariableCS(&q_notempty, &q_cs, INFINITE);
        if (!q_count) { LeaveCriticalSection(&q_cs); break; }
        buf = (const uint8_t *)slot[q_tail];
        LeaveCriticalSection(&q_cs);
        while (done < bytes) {
            if (!WriteFile(vout, buf + done, bytes - done, &n, NULL) || !n) {
                EnterCriticalSection(&q_cs); q_failed = 1; WakeAllConditionVariable(&q_notfull);
                LeaveCriticalSection(&q_cs);
                return 1;
            }
            done += n;
        }
        EnterCriticalSection(&q_cs);
        q_tail = (q_tail + 1) % QN; q_count--;
        WakeConditionVariable(&q_notfull);
        LeaveCriticalSection(&q_cs);
    }
    return 0;
}
static FILE *audio_out;
static long long audio_frames;
static int audio_on;                /* audio is written from --video-from on, like the video */

static void cb_video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data) return;   /* dupe: keep the previous frame */
    last_data = data; last_w = w; last_h = h; last_pitch = pitch;
}

static void scale_to(uint16_t *canvas)
{
    unsigned x, y;
    if (!last_data || !last_w || !last_h) { memset(canvas, 0, (size_t)out_w * out_h * 2); return; }
    for (y = 0; y < (unsigned)out_h; y++) {
        const uint8_t *row = (const uint8_t *)last_data + (size_t)(y * last_h / out_h) * last_pitch;
        uint16_t *o = canvas + (size_t)y * out_w;
        for (x = 0; x < (unsigned)out_w; x++) {
            unsigned sx = x * last_w / out_w;
            if (pixfmt == 1) {
                uint32_t c = ((const uint32_t *)row)[sx];
                o[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
            } else if (pixfmt == 2) {
                o[x] = ((const uint16_t *)row)[sx];
            } else {
                uint16_t c = ((const uint16_t *)row)[sx];
                o[x] = (uint16_t)(((c & 0x7FE0) << 1) | (c & 0x1F));
            }
        }
    }
}

static size_t cb_audio_batch(const int16_t *data, size_t n)
{
    if (!audio_on) return n;
    if (audio_out) fwrite(data, 4, n, audio_out);
    audio_frames += n;
    return n;
}
static void cb_audio(int16_t l, int16_t r) { int16_t s[2] = {l, r}; cb_audio_batch(s, 1); }

/* ---- environment --------------------------------------------------------- */
static char sysdir[MAX_PATH], savedir[MAX_PATH];
static int verbose;

static void cb_log(int level, const char *fmt, ...)
{
    va_list ap;
    if (level < 2 && !verbose) return;
    va_start(ap, fmt); fputs("[core] ", stderr); vfprintf(stderr, fmt, ap); va_end(ap);
}

static int cb_env(unsigned cmd, void *data)
{
    cmd &= 0xFFFF;
    switch (cmd) {
    case 9: *(const char **)data = sysdir; return 1;
    case 31: *(const char **)data = savedir; return 1;
    case 15: {
        struct retro_variable *v = data; const char *x = opt_get(v->key);
        if (!x) return 0;
        v->value = x; return 1;
    }
    case 16: {
        struct retro_variable *v = data;
        for (; v->key; v++) {
            const char *semi = strchr(v->value, ';');
            char def[256]; size_t n;
            if (!semi || opt_get(v->key)) continue;
            semi++; while (*semi == ' ') semi++;
            n = strcspn(semi, "|"); if (n >= sizeof def) n = sizeof def - 1;
            memcpy(def, semi, n); def[n] = 0;
            opt_set(v->key, def, 0);
        }
        return 1;
    }
    case 52: *(unsigned *)data = 0; return 1;
    case 17: *(unsigned char *)data = 0; return 1;
    case 10: pixfmt = *(int *)data; return 1;
    case 27: ((struct retro_log_callback *)data)->log = cb_log; return 1;
    case 3: *(unsigned char *)data = 1; return 1;
    case 47: if (data) *(int *)data = 3; return 1;
    case 59: *(unsigned *)data = 8; return 1;
    case 11: case 13: case 32: case 35: case 36: case 37: case 51: case 70: return 1;
    }
    return 0;
}

/* ---- main ---------------------------------------------------------------- */
#define SYM(var, name) do { *(FARPROC *)&var = GetProcAddress(lib, name); \
    if (!var) { fprintf(stderr, "sem %s no core\n", name); return 2; } } while (0)

static double now(void)
{
    static LARGE_INTEGER fq; LARGE_INTEGER c;
    if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / fq.QuadPart;
}

int main(int argc, char **argv)
{
    const char *core = "cores\\pcsx_rearmed_libretro.dll", *game = NULL, *krec = NULL;
    const char *opt = "config\\PCSX-ReARMed\\PCSX-ReARMed.opt", *aout = NULL;
    int video = 0, every = 1, last = 0, video_from = 1, report = 600, i, f, ok = 0, bad = 0, first_bad = -1;
    HMODULE lib; struct retro_game_info gi = {0}; struct retro_system_av_info av;
    uint8_t *ram; size_t ram_size, slice; uint64_t acc = 0; int win = -1;
    double t0, tchunk, tout0 = 0; int fchunk;
    HANDLE writer = NULL;

    strcpy(sysdir, "system"); strcpy(savedir, "render_saves");
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--core")) core = v, i++;
        else if (!strcmp(a, "--game")) game = v, i++;
        else if (!strcmp(a, "--krec")) krec = v, i++;
        else if (!strcmp(a, "--opt")) opt = v, i++;
        else if (!strcmp(a, "--system")) strncpy(sysdir, v, MAX_PATH - 1), i++;
        else if (!strcmp(a, "--audio")) aout = v, i++;
        else if (!strcmp(a, "--video")) video = 1;
        else if (!strcmp(a, "--every")) every = atoi(v), i++;
        else if (!strcmp(a, "--frames")) last = atoi(v), i++;
        else if (!strcmp(a, "--video-from")) video_from = atoi(v), i++;
        else if (!strcmp(a, "--size")) sscanf(v, "%dx%d", &out_w, &out_h), i++;
        else if (!strcmp(a, "--report")) report = atoi(v), i++;
        else if (!strcmp(a, "--verbose")) verbose = 1;
        else { fprintf(stderr, "opcao desconhecida: %s\n", a); return 2; }
    }
    if (!game || !krec) {
        fprintf(stderr, "uso: krec_render --game JOGO --krec PARTIDA.krec [--core DLL] [--system DIR] [--opt ARQ]\n"
                        "       [--video] [--every N] [--size WxH] [--video-from F] [--frames N] [--audio ARQ.s16]\n");
        return 2;
    }
    _setmode(_fileno(stdout), _O_BINARY);
    CreateDirectoryA(savedir, NULL);

    load_krec(krec);
    fprintf(stderr, "krec: %d frames (~%d:%02d), %d jogadores, MultiTap %s\n", n_frames,
            n_frames / 3600, n_frames / 60 % 60, numplayers, multitap ? "sim" : "nao");
    if (!last || last > n_frames) last = n_frames;

    load_opt_file(opt);
    for (i = 0; i < (int)(sizeof FORCED / sizeof FORCED[0]); i++) opt_set(FORCED[i][0], FORCED[i][1], 1);
    if (multitap) {
        int both = numplayers > 4;
        opt_set("pcsx_rearmed_multitap", both ? "ports 1 and 2" : "port 1", 1);
        opt_set("pcsx_rearmed_multitap1", "enabled", 1);
        opt_set("pcsx_rearmed_multitap2", both ? "enabled" : "disabled", 1);
    }

    lib = LoadLibraryA(core);
    if (!lib) { fprintf(stderr, "LoadLibrary(%s) falhou: %lu\n", core, GetLastError()); return 2; }
    SYM(p_set_environment, "retro_set_environment"); SYM(p_set_video_refresh, "retro_set_video_refresh");
    SYM(p_set_audio_sample, "retro_set_audio_sample"); SYM(p_set_audio_sample_batch, "retro_set_audio_sample_batch");
    SYM(p_set_input_poll, "retro_set_input_poll"); SYM(p_set_input_state, "retro_set_input_state");
    SYM(p_init, "retro_init"); SYM(p_load_game, "retro_load_game"); SYM(p_run, "retro_run");
    SYM(p_get_system_av_info, "retro_get_system_av_info");
    SYM(p_set_controller_port_device, "retro_set_controller_port_device");
    SYM(p_get_memory_data, "retro_get_memory_data"); SYM(p_get_memory_size, "retro_get_memory_size");

    p_set_environment(cb_env); p_set_video_refresh(cb_video);
    p_set_audio_sample(cb_audio); p_set_audio_sample_batch(cb_audio_batch);
    p_set_input_poll(cb_poll); p_set_input_state(cb_state);
    p_init();
    gi.path = game;
    if (!p_load_game(&gi)) { fprintf(stderr, "retro_load_game falhou: %s\n", game); return 2; }
    for (i = 0; i < 8; i++) p_set_controller_port_device(i, 1);
    p_get_system_av_info(&av);
    fprintf(stderr, "core: %.2f fps, audio %.0f Hz, pixfmt %d; saida %dx%d a cada %d frame(s)\n",
            av.timing.fps, av.timing.sample_rate, pixfmt, out_w, out_h, every);

    ram = p_get_memory_data(2); ram_size = p_get_memory_size(2);
    slice = (ram_size + DIGEST_FRAMES - 1) / DIGEST_FRAMES;
    if (video) {
        vout = GetStdHandle(STD_OUTPUT_HANDLE);
        for (i = 0; i < QN; i++) slot[i] = calloc((size_t)out_w * out_h, 2);
        InitializeCriticalSection(&q_cs);
        InitializeConditionVariable(&q_notempty); InitializeConditionVariable(&q_notfull);
        writer = CreateThread(NULL, 0, video_writer, NULL, 0, NULL);
    }
    if (aout) {
        audio_out = fopen(aout, "wb");
        if (!audio_out) { fprintf(stderr, "nao abriu o audio %s\n", aout); return 2; }
        /* Unbuffered: with ffmpeg reading video from stdout and audio from a
         * FIFO, audio held in a stdio buffer while a video write blocks is a
         * deadlock. */
        setvbuf(audio_out, NULL, _IONBF, 0);
    }

    t0 = tchunk = now(); fchunk = 0;
    for (f = 1; f <= last; f++) {
        int w = f / DIGEST_FRAMES, idx = f % DIGEST_FRAMES;
        set_input(&frames[f - 1]);
        audio_on = f >= video_from;
        p_run();

        if (w != win) { win = w; acc = 0xCBF29CE484222325ULL; }
        if (ram && (size_t)idx * slice < ram_size) {
            const uint8_t *b = ram + (size_t)idx * slice;
            size_t n = ram_size - (size_t)idx * slice, k;
            if (n > slice) n = slice;
            for (k = 0; k + 8 <= n; k += 8) acc = (acc ^ *(const uint64_t *)(b + k)) * 0x100000001B3ULL;
            for (; k < n; k++) acc = (acc ^ b[k]) * 0x100000001B3ULL;
        }
        if (idx == DIGEST_FRAMES - 1 && w < n_want && want[w].n) {
            int j, m = 0;
            for (j = 0; j < want[w].n; j++) m |= want[w].h[j] == acc;
            if (m) ok++;
            else { bad++; if (first_bad < 0) first_bad = w; }
        }

        if (f == video_from) tout0 = now();
        if (video && f >= video_from && f % every == 0) {
            uint16_t *dst;
            EnterCriticalSection(&q_cs);
            while (q_count == QN && !q_failed) SleepConditionVariableCS(&q_notfull, &q_cs, INFINITE);
            dst = q_failed ? NULL : slot[q_head];
            LeaveCriticalSection(&q_cs);
            if (!dst) { fprintf(stderr, "saida de video fechada no frame %d\n", f); break; }
            scale_to(dst);   /* the writer never touches slots outside [tail, tail+count) */
            EnterCriticalSection(&q_cs);
            q_head = (q_head + 1) % QN; q_count++;
            WakeConditionVariable(&q_notempty);
            LeaveCriticalSection(&q_cs);
        }
        if (report && f % report == 0) {
            double t = now();
            fprintf(stderr, "[frame %6d ~%d:%02d] %6.1f fps (trecho)  %6.1f fps (media)  digests ok=%d bad=%d\n",
                    f, f / 3600, f / 60 % 60, (f - fchunk) / (t - tchunk), f / (t - t0), ok, bad);
            tchunk = t; fchunk = f;
        }
    }
    if (audio_out) fclose(audio_out);   /* EOF for the audio side first... */
    if (writer) {                        /* ...then drain the video queue */
        EnterCriticalSection(&q_cs); q_done = 1; WakeAllConditionVariable(&q_notempty); LeaveCriticalSection(&q_cs);
        WaitForSingleObject(writer, INFINITE);
        CloseHandle(vout);
    }
    {
        double t = now();
        fprintf(stderr, "FIM: %d frames em %.1f s = %.1f fps (%.2fx tempo real)", f - 1, t - t0,
                (f - 1) / (t - t0), (f - 1) / (t - t0) / 60.0);
        if (tout0 > 0 && f - 1 >= video_from)
            fprintf(stderr, "; com saida desde o frame %d: %.1f fps (%.2fx)", video_from,
                    (f - video_from) / (t - tout0), (f - video_from) / (t - tout0) / 60.0);
        fprintf(stderr, "\nRAM x jogadores: %d janelas iguais, %d diferentes%s\n", ok, bad,
                first_bad >= 0 ? " (DIVERGE)" : "");
        if (first_bad >= 0) fprintf(stderr, "primeira divergencia na janela %d (~%d:%02d)\n",
                                    first_bad, first_bad * 5 / 60, first_bad * 5 % 60);
        fprintf(stderr, "audio: %lld amostras\n", audio_frames);
    }
    return bad ? 1 : 0;
}
