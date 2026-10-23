/*
 * interval_trainer.c - ear-training helper for pitch intervals.
 *
 * Requires miniaudio (single header): https://github.com/mackron/miniaudio
 * Put miniaudio.h next to this file, then build, e.g. on Linux:
 *
 *     gcc -O2 -o interval_trainer interval_trainer.c -lm -lpthread -ldl
 *
 * Run:   ./interval_trainer [logfile]        (default log: interval_log.json)
 *
 * Modes
 *   1) Interval:        you are shown an interval name and direction, hear the
 *                       starting pitch, try to produce/imagine the second pitch,
 *                       press Enter to hear it, then say whether you were right.
 *   2) Three-note seq.: you are shown two interval names. Interval 1 is the
 *                       distance from note 1 to note 2; interval 2 is the
 *                       distance from note 1 to note 3.
 *
 * Pitches use scientific pitch notation (C4 = middle C = 261.63 Hz), so the
 * allowed range C3..C5 is MIDI notes 48..72. Any semitone in that range can be
 * chosen (C#3, F#4, ...), as long as every pitch of the trial stays in range.
 *
 * Log format: a JSON array, one object per trial, e.g.
 *   {
 *     "timestamp": "2026-10-22T19:03:11Z",      (UTC, when the trial began)
 *     "mode": "interval",                        ("interval" or "sequence")
 *     "initial_pitch": "C#3",
 *     "initial_midi": 49,
 *     "intervals": [ {"name": "m3", "direction": "ascending"} ],
 *     "correct": true
 *   }
 * In sequence mode "intervals" has two entries, both measured from the
 * initial pitch.
 */

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define LOW_MIDI      48        /* C3 */
#define HIGH_MIDI     72        /* C5 */
#define NOTE_SECONDS  1.0       /* length of every note */
#define SAMPLE_RATE   48000
#define DEFAULT_LOG   "interval_log.json"
#define TWO_PI        6.283185307179586

typedef struct {
    const char *name;
    int         semitones;
} Interval;

/* NOTE: you wrote "M4" in your list; there is no major fourth in standard
 * interval naming, so this assumes you meant M3 (4 semitones). Edit here
 * if you meant something else. */
static const Interval INTERVALS[] = {
    { "m2",  1 },
    { "M2",  2 },
    { "m3",  3 },
    { "M3",  4 },
    { "P4",  5 },
    { "P5",  7 },
    { "P8", 12 },
};
#define NUM_INTERVALS ((int)(sizeof INTERVALS / sizeof INTERVALS[0]))

/* ------------------------------------------------------------------ */
/* Audio: a tiny synth driven from miniaudio's playback callback       */
/* ------------------------------------------------------------------ */

typedef struct {
    ma_mutex  lock;
    ma_event  done;        /* signalled by the callback when a note ends */
    ma_uint32 sampleRate;
    double    freq;
    double    phase;       /* in cycles, 0..1 */
    ma_uint32 pos;         /* frames of the current note already rendered */
    ma_uint32 total;       /* frames in the current note */
    ma_bool32 active;
} Synth;

static void data_callback(ma_device *pDevice, void *pOutput,
                          const void *pInput, ma_uint32 frameCount)
{
    Synth    *s        = (Synth *)pDevice->pUserData;
    float    *out      = (float *)pOutput;
    ma_uint32 channels = pDevice->playback.channels;
    ma_uint32 attack   = s->sampleRate / 100;       /* 10 ms fade in  */
    ma_uint32 release  = s->sampleRate * 3 / 100;   /* 30 ms fade out */
    ma_bool32 finished = MA_FALSE;
    ma_uint32 i, c;

    (void)pInput;

    ma_mutex_lock(&s->lock);
    for (i = 0; i < frameCount; i++) {
        float sample = 0.0f;

        if (s->active) {
            /* Fundamental plus two weaker harmonics: easier to hear on small
             * speakers than a bare sine, and the pitch is still unambiguous. */
            double p   = s->phase;
            double v   = sin(TWO_PI * p)
                       + 0.50 * sin(2.0 * TWO_PI * p)
                       + 0.25 * sin(3.0 * TWO_PI * p);
            double env = 1.0;

            if (s->pos < attack) {
                env = (double)s->pos / attack;
            } else if (s->total - s->pos < release) {
                env = (double)(s->total - s->pos) / release;
            }
            sample = (float)(0.2 * env * v);

            s->phase += s->freq / s->sampleRate;
            if (s->phase >= 1.0) s->phase -= 1.0;

            if (++s->pos >= s->total) {
                s->active = MA_FALSE;
                finished  = MA_TRUE;
            }
        }
        for (c = 0; c < channels; c++) {
            out[i * channels + c] = sample;
        }
    }
    ma_mutex_unlock(&s->lock);

    if (finished) ma_event_signal(&s->done);
}

static int audio_start(Synth *s, ma_device *dev)
{
    ma_device_config cfg;

    memset(s, 0, sizeof *s);
    if (ma_mutex_init(&s->lock) != MA_SUCCESS) return 0;
    if (ma_event_init(&s->done) != MA_SUCCESS) {
        ma_mutex_uninit(&s->lock);
        return 0;
    }

    cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = SAMPLE_RATE;
    cfg.dataCallback      = data_callback;
    cfg.pUserData         = s;

    if (ma_device_init(NULL, &cfg, dev) != MA_SUCCESS) {
        ma_event_uninit(&s->done);
        ma_mutex_uninit(&s->lock);
        return 0;
    }
    s->sampleRate = dev->sampleRate;

    if (ma_device_start(dev) != MA_SUCCESS) {
        ma_device_uninit(dev);
        ma_event_uninit(&s->done);
        ma_mutex_uninit(&s->lock);
        return 0;
    }
    return 1;
}

static void audio_stop(Synth *s, ma_device *dev)
{
    ma_device_uninit(dev);
    ma_event_uninit(&s->done);
    ma_mutex_uninit(&s->lock);
}

static double midi_to_freq(int midi)
{
    return 440.0 * pow(2.0, (midi - 69) / 12.0);
}

/* Plays one note and blocks until it has been fully rendered. */
static void play_note(Synth *s, int midi, double seconds)
{
    ma_mutex_lock(&s->lock);
    s->freq   = midi_to_freq(midi);
    s->phase  = 0.0;
    s->pos    = 0;
    s->total  = (ma_uint32)(seconds * s->sampleRate);
    if (s->total == 0) s->total = 1;
    s->active = MA_TRUE;
    ma_mutex_unlock(&s->lock);

    ma_event_wait(&s->done);
}

/* ------------------------------------------------------------------ */
/* Helpers: names, input, random numbers, time                         */
/* ------------------------------------------------------------------ */

static void note_name(int midi, char *buf, size_t cap)
{
    static const char *const NAMES[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    snprintf(buf, cap, "%s%d", NAMES[midi % 12], midi / 12 - 1);
}

/* Reads one line (without the newline). Returns 0 on EOF. */
static int read_line(char *buf, size_t cap)
{
    size_t len;

    if (!fgets(buf, (int)cap, stdin)) return 0;
    len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    } else {
        int ch;                         /* line was longer than buf: discard rest */
        while ((ch = getchar()) != '\n' && ch != EOF) { }
    }
    return 1;
}

static int rand_range(int lo, int hi)   /* inclusive */
{
    return lo + rand() % (hi - lo + 1);
}

static void utc_timestamp(char *buf, size_t cap)
{
    time_t     now = time(NULL);
    struct tm *tm  = gmtime(&now);

    if (tm) strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", tm);
    else    snprintf(buf, cap, "unknown");
}

/* ------------------------------------------------------------------ */
/* Trials                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int count;       /* number of intervals: 1 (interval mode) or 2 (sequence) */
    int idx[2];      /* index into INTERVALS */
    int dir[2];      /* +1 ascending, -1 descending */
    int midi[3];     /* pitches in play order; midi[0] is the initial pitch */
} Trial;

/* Chooses intervals and directions first, then a random starting pitch
 * such that every pitch of the trial lies inside C3..C5. */
static void make_trial(Trial *t, int count)
{
    int d[2] = { 0, 0 };
    int lo, hi, k;

    t->count = count;

    for (;;) {
        for (k = 0; k < count; k++) {
            t->idx[k] = rand_range(0, NUM_INTERVALS - 1);
            t->dir[k] = rand_range(0, 1) ? 1 : -1;
            d[k]      = t->dir[k] * INTERVALS[t->idx[k]].semitones;
        }
        /* In sequence mode avoid notes 2 and 3 being the same pitch. */
        if (count < 2 || d[0] != d[1]) break;
    }

    lo = LOW_MIDI;
    hi = HIGH_MIDI;
    for (k = 0; k < count; k++) {
        if (LOW_MIDI  - d[k] > lo) lo = LOW_MIDI  - d[k];
        if (HIGH_MIDI - d[k] < hi) hi = HIGH_MIDI - d[k];
    }

    t->midi[0] = rand_range(lo, hi);
    for (k = 0; k < count; k++) {
        t->midi[k + 1] = t->midi[0] + d[k];
    }
}

static const char *dir_name(int dir)
{
    return dir > 0 ? "ascending" : "descending";
}

/* ------------------------------------------------------------------ */
/* JSON log                                                            */
/* ------------------------------------------------------------------ */

static void format_record(char *buf, size_t cap, const Trial *t,
                          int correct, const char *stamp)
{
    char   name[16];
    size_t n = 0;
    int    k;

    note_name(t->midi[0], name, sizeof name);

    n += (size_t)snprintf(buf + n, cap - n,
        "  {\n"
        "    \"timestamp\": \"%s\",\n"
        "    \"mode\": \"%s\",\n"
        "    \"initial_pitch\": \"%s\",\n"
        "    \"initial_midi\": %d,\n"
        "    \"intervals\": [\n",
        stamp, t->count == 1 ? "interval" : "sequence", name, t->midi[0]);

    for (k = 0; k < t->count; k++) {
        n += (size_t)snprintf(buf + n, cap - n,
            "      {\"name\": \"%s\", \"direction\": \"%s\"}%s\n",
            INTERVALS[t->idx[k]].name, dir_name(t->dir[k]),
            k + 1 < t->count ? "," : "");
    }

    snprintf(buf + n, cap - n,
        "    ],\n"
        "    \"correct\": %s\n"
        "  }", correct ? "true" : "false");
}

/* Appends one object to the JSON array stored in `path`, creating the file if
 * needed. The file is rewritten via a temporary file so a crash can't leave a
 * half-written log. Returns 1 on success. */
static int append_record(const char *path, const char *record)
{
    char   *old = NULL;
    size_t  oldLen = 0, start = 0, end = 0, body = 0;
    int     fresh, empty = 0, ok;
    char    tmpPath[1024];
    FILE   *f, *out;

    f = fopen(path, "rb");
    if (f) {
        long sz = -1;
        if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
        if (sz >= 0) {
            rewind(f);
            old = (char *)malloc((size_t)sz + 1);
            if (old) {
                oldLen = fread(old, 1, (size_t)sz, f);
                old[oldLen] = '\0';
            }
        }
        fclose(f);
        if (!old) return 0;
    }

    /* Trim whitespace at both ends of the existing content. */
    end = oldLen;
    while (end > 0 && isspace((unsigned char)old[end - 1])) end--;
    while (start < end && isspace((unsigned char)old[start])) start++;
    fresh = (end == start);                 /* missing or blank file */

    if (!fresh) {
        if (end - start < 2 || old[start] != '[' || old[end - 1] != ']') {
            free(old);                      /* not a JSON array: leave it alone */
            return 0;
        }
        body = end - 1;                     /* everything before the final ']' */
        while (body > start + 1 && isspace((unsigned char)old[body - 1])) body--;
        empty = (body == start + 1);        /* only '[' remains: empty array */
    }

    if (snprintf(tmpPath, sizeof tmpPath, "%s.tmp", path) >= (int)sizeof tmpPath) {
        free(old);
        return 0;
    }
    out = fopen(tmpPath, "wb");
    if (!out) {
        free(old);
        return 0;
    }

    if (fresh) {
        ok = fputs("[\n", out) >= 0;
    } else {
        ok = fwrite(old, 1, body, out) == body
          && fputs(empty ? "\n" : ",\n", out) >= 0;
    }
    ok = ok && fputs(record, out) >= 0 && fputs("\n]\n", out) >= 0;
    ok = (fclose(out) == 0) && ok;
    free(old);

    if (!ok) {
        remove(tmpPath);
        return 0;
    }
#ifdef _WIN32
    remove(path);                           /* rename() won't overwrite on Windows */
#endif
    if (rename(tmpPath, path) != 0) {
        remove(tmpPath);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Running a trial                                                     */
/* ------------------------------------------------------------------ */

/* Returns 1 (correct), 0 (incorrect) or -1 if input ended before an answer. */
static int run_trial(Synth *synth, const Trial *t, int number, const char *logPath)
{
    char line[64], stamp[32], names[3][16], record[1024];
    int  k, correct;

    utc_timestamp(stamp, sizeof stamp);
    for (k = 0; k <= t->count; k++) note_name(t->midi[k], names[k], sizeof names[k]);

    printf("\n--- Trial %d ---\n", number);
    if (t->count == 1) {
        printf("Interval: %s %s\n", dir_name(t->dir[0]), INTERVALS[t->idx[0]].name);
    } else {
        printf("Interval 1 (note 1 -> note 2): %s %s\n",
               dir_name(t->dir[0]), INTERVALS[t->idx[0]].name);
        printf("Interval 2 (note 1 -> note 3): %s %s\n",
               dir_name(t->dir[1]), INTERVALS[t->idx[1]].name);
    }

    printf("Playing the starting pitch...\n");
    play_note(synth, t->midi[0], NOTE_SECONDS);

    printf("Press Enter when you are ready to hear %s: ",
           t->count == 1 ? "the second pitch" : "the second and third pitches");
    fflush(stdout);
    if (!read_line(line, sizeof line)) return -1;

    for (k = 1; k <= t->count; k++) play_note(synth, t->midi[k], NOTE_SECONDS);

    printf("That was: %s", names[0]);
    for (k = 1; k <= t->count; k++) printf(" -> %s", names[k]);
    printf("\n");

    for (;;) {
        printf("Did you get it right? (y/n): ");
        fflush(stdout);
        if (!read_line(line, sizeof line)) return -1;
        if (tolower((unsigned char)line[0]) == 'y') { correct = 1; break; }
        if (tolower((unsigned char)line[0]) == 'n') { correct = 0; break; }
        printf("Please answer y or n.\n");
    }

    format_record(record, sizeof record, t, correct, stamp);
    if (!append_record(logPath, record)) {
        printf("Warning: could not append to %s (is it a valid JSON array?)\n", logPath);
    }
    return correct;
}

/* Returns 1 or 2 for the chosen mode, 0 to quit. */
static int choose_mode(void)
{
    char line[64];

    for (;;) {
        printf("\nChoose a mode:\n"
               "  1) Intervals\n"
               "  2) Three-note sequences\n"
               "  q) Quit\n"
               "> ");
        fflush(stdout);
        if (!read_line(line, sizeof line)) return 0;
        if (line[0] == '1') return 1;
        if (line[0] == '2') return 2;
        if (tolower((unsigned char)line[0]) == 'q') return 0;
        printf("Please enter 1, 2 or q.\n");
    }
}

/* Returns 'n' (next trial), 'm' (change mode) or 'q' (quit). */
static char ask_next(void)
{
    char line[64];

    for (;;) {
        printf("\nEnter = next trial, m = change mode, q = quit: ");
        fflush(stdout);
        if (!read_line(line, sizeof line)) return 'q';
        if (line[0] == '\0')                       return 'n';
        if (tolower((unsigned char)line[0]) == 'm') return 'm';
        if (tolower((unsigned char)line[0]) == 'q') return 'q';
    }
}

int main(int argc, char **argv)
{
    const char *logPath = argc > 1 ? argv[1] : DEFAULT_LOG;
    Synth       synth;
    ma_device   device;
    int         trials = 0, right = 0, quit = 0;

    srand((unsigned)time(NULL) ^ (unsigned)clock());

    if (!audio_start(&synth, &device)) {
        fprintf(stderr, "Could not open an audio playback device.\n");
        return 1;
    }

    printf("Interval ear trainer. Results are logged to %s\n", logPath);

    while (!quit) {
        int mode = choose_mode();
        if (mode == 0) break;

        for (;;) {
            Trial t;
            int   r;
            char  next;

            make_trial(&t, mode);
            r = run_trial(&synth, &t, trials + 1, logPath);
            if (r < 0) { quit = 1; break; }
            trials++;
            right += r;

            next = ask_next();
            if (next == 'q') { quit = 1; break; }
            if (next == 'm') break;
        }
    }

    printf("\nSession: %d of %d correct. Goodbye.\n", right, trials);
    audio_stop(&synth, &device);
    return 0;
}
