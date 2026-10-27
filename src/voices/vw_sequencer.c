/* vw_sequencer.c -- one part as a VocalWriter song (see vw_sequencer.h):
 * written as a .trk, read back with the engine's loader and played as
 * engine/src/song.c's vw_render does, the reverb left to the mixer.
 */
#include "voices/vw_sequencer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* in angle brackets: the engine's vocalwriter.h, not this folder's */
#include <vw_engine.h>
#include <vocalwriter.h>

#define SEQHEADER_SIZE 758
#define TRACKINFO_SIZE 88
#define SEQINFO_SIZE (32 * TRACKINFO_SIZE)
#define TICKS_PER_BEAT 240
#define SPEECH_RATE 140            /* what StartCurSeq passes MakeSpeechData */
#define POLYPHONY 48
#define VOCAL_TRACK 1
#define REST_PHONEME 23
#define SPARE_PROGRAM 127

typedef struct {
    unsigned char *p;
    size_t len, cap;
    int failed;
} buf;

static void put(buf *b, const void *data, size_t n)
{
    if (b->failed)
        return;
    if (b->len + n > b->cap) {
        size_t cap = (b->len + n) * 2 + 1024;
        unsigned char *q = (unsigned char *)realloc(b->p, cap);
        if (q == NULL) {
            b->failed = 1;
            return;
        }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

static void be16(unsigned char *p, int v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static void be32(unsigned char *p, int32_t v)
{
    p[0] = (unsigned char)((uint32_t)v >> 24);
    p[1] = (unsigned char)((uint32_t)v >> 16);
    p[2] = (unsigned char)((uint32_t)v >> 8);
    p[3] = (unsigned char)v;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* One 12-byte event: a 24-bit time, the channel byte (0 for the tempo track,
   1 for a voice, as the application writes them), the status, two data bytes
   (a tempo's are a whole 16-bit number), a 24-bit length, the lyric. */
static void event(buf *b, int channel, int32_t time, int status, int key, int vol, int32_t dur,
                  int lyric)
{
    unsigned char r[12];
    time = clamp(time, 0, 0xfffffe);
    dur = clamp(dur, 0, 0xfffffe);
    r[0] = (unsigned char)(time >> 16);
    r[1] = (unsigned char)(time >> 8);
    r[2] = (unsigned char)time;
    r[3] = (unsigned char)channel;
    r[4] = (unsigned char)status;
    r[5] = (unsigned char)(status == 4 ? key & 0xff : clamp(key, 0, 127));
    r[6] = (unsigned char)(status == 4 ? vol & 0xff : clamp(vol, 0, 127));
    r[7] = (unsigned char)(dur >> 16);
    r[8] = (unsigned char)(dur >> 8);
    r[9] = (unsigned char)dur;
    r[10] = (unsigned char)(lyric >> 8);
    r[11] = (unsigned char)lyric;
    put(b, r, 12);
}

static const unsigned char kEnd[4] = {0xff, 0xff, 0xff, 0xff};

/* The part as a .trk file: the header, the 32 track records, the tempo
   track, the vocal track, its lyrics and the empty tracks. */
static unsigned char *write_song(const vwseq_part *part, int program, size_t *out_len)
{
    buf tempo = {0}, vocal = {0}, lyrics = {0}, file = {0};
    unsigned char header[SEQHEADER_SIZE], info[SEQINFO_SIZE];
    int32_t last_sung = 0, offset;
    int i, ei = 0, lyric = 0, bpm = clamp(part->bpm, 10, 250);

    event(&vocal, 1, 0, 3, 0, program, 0, 0);
    for (i = 0; i < part->note_count; i++) {
        const vwseq_note *n = &part->notes[i];
        /* the controls at and before a note go in front of it */
        while (ei < part->event_count && part->events[ei].time <= n->start) {
            const vwseq_event *e = &part->events[ei++];
            event(&vocal, 1, e->time, e->status, e->key, e->vol, 0, 0);
        }
        if (n->rest) {
            event(&vocal, 1, n->start, 8, n->key, 0, n->duration, 0);
            continue;
        }
        {
            unsigned char rec[26];
            size_t tl = strlen(n->text);
            int pl = n->phonemes[0] > 12 ? 12 : n->phonemes[0];
            memset(rec, 0, sizeof rec);
            if (tl > 12)
                tl = 12;
            rec[0] = (unsigned char)tl;
            memcpy(rec + 1, n->text, tl);
            rec[13] = (unsigned char)pl;
            memcpy(rec + 14, n->phonemes + 1, (size_t)pl);
            put(&lyrics, rec, 26);
        }
        event(&vocal, 1, n->start, 6, n->key, clamp(n->velocity, 1, 127), n->duration, lyric++);
        if (n->start + n->duration > last_sung)
            last_sung = n->start + n->duration;
    }
    while (ei < part->event_count) {
        const vwseq_event *e = &part->events[ei++];
        event(&vocal, 1, e->time, e->status, e->key, e->vol, 0, 0);
    }
    put(&vocal, kEnd, 4);
    /* the tempo, and the end mark a beat after the last sung note */
    event(&tempo, 0, 0, 4, bpm >> 8, bpm & 0xff, 0, 0);
    event(&tempo, 0, last_sung + TICKS_PER_BEAT, 9, 0, 0, 0, 0);
    put(&tempo, kEnd, 4);

    memset(header, 0, sizeof header);
    be32(header + 0, 3);
    be32(header + 4, 0x10000);
    be32(header + 8, SEQHEADER_SIZE);
    be16(header + 0xc, bpm);
    be16(header + 0xe, 4);
    be16(header + 0x10, 2);                      /* 960 >> 2: 240 ticks a beat */
    be16(header + 0x12, TICKS_PER_BEAT);
    be16(header + 0x14, 32);
    for (i = 0; i < 32; i++) {
        be16(header + 0x16 + 2 * i, i == VOCAL_TRACK ? clamp(part->track_level, 0, 100) : 100);
        be16(header + 0x56 + 2 * i, 1);
    }
    memset(info, 0, sizeof info);
    offset = SEQINFO_SIZE;
    for (i = 0; i < 32; i++) {
        unsigned char *t = info + i * TRACKINFO_SIZE;
        be32(t, i);
        if (i == 0) {
            be32(t + 0x4, 4);
            be32(t + 0x8, offset);
            be32(t + 0xc, (int32_t)tempo.len);
            offset += (int32_t)tempo.len;
        } else if (i == VOCAL_TRACK) {
            be32(t + 0x4, 1);                    /* a vocal track */
            be32(t + 0x8, offset);
            be32(t + 0xc, (int32_t)vocal.len);
            offset += (int32_t)vocal.len;
            be32(t + 0x18, offset);
            be32(t + 0x1c, lyric);
            be32(t + 0x20, 13);
            offset += (int32_t)lyrics.len;
        } else {
            be32(t + 0x8, offset);
            be32(t + 0xc, 4);
            offset += 4;
        }
    }
    put(&file, header, sizeof header);
    put(&file, info, sizeof info);
    put(&file, tempo.p, tempo.len);
    put(&file, vocal.p, vocal.len);
    if (lyrics.len)
        put(&file, lyrics.p, lyrics.len);
    for (i = 2; i < 32; i++)
        put(&file, kEnd, 4);
    free(tempo.p);
    free(vocal.p);
    free(lyrics.p);
    if (file.failed || tempo.failed || vocal.failed || lyrics.failed) {
        free(file.p);
        return NULL;
    }
    *out_len = file.len;
    return file.p;
}

/* MakeSpeechData's output is [0, count, phonemes, controls, lengths,
   lengths]; bit 0 of a control is the nucleus, bit 1 a consonant that opens
   the next note. */
static void scale_consonants(int16_t *data, double factor)
{
    int n = data[1], i;
    int16_t *phon = data + 2, *ctrl = phon + n, *dur1 = ctrl + n, *dur = dur1 + n;
    double onset = factor < 0.1 ? 0.1 : factor, coda = 1.0 - (1.0 - onset) * 0.5;
    for (i = 0; i < n; i++) {
        double f;
        long a, b;
        if (phon[i] == REST_PHONEME || (ctrl[i] & 1))
            continue;
        f = (ctrl[i] & 2) ? onset : coda;
        a = lrint(dur[i] * f);
        b = lrint(dur1[i] * f);
        dur[i] = (int16_t)(a < 1 ? 1 : a);
        dur1[i] = (int16_t)(b < 1 ? 1 : b);
    }
}

/* The voice moves from nucleus (bit 0) to nucleus, one a note, and a note
   with no vowel has none: every note after it would be sung one early, up
   to the next rest. Bit 5 opens a note; one without a nucleus gets it on
   its first phoneme that is not a stop, and a start (bit 1) if it has none. */
static void nucleus_every_note(int16_t *data)
{
    int n = data[1], i, first = -1, nucleus = 0, started = 0;
    int16_t *phon = data + 2, *ctrl = phon + n;
    for (i = 0; i <= n; i++) {
        if (i == n || (ctrl[i] & 32)) {
            if (first >= 0 && !nucleus) {
                int k = first;
                while (k < i && (phon[k] == REST_PHONEME || (phon[k] >= 44 && phon[k] <= 55)))
                    k++;
                ctrl[k < i ? k : first] |= 1;
            }
            if (first >= 0 && !started)
                ctrl[first] |= 2;
            first = i;
            nucleus = started = 0;
        }
        if (i < n && (ctrl[i] & 1))
            nucleus = 1;
        if (i < n && (ctrl[i] & 2))
            started = 1;
    }
}

struct vwseq_engine {
    vw_engine e;
};

vwseq_engine *vwseq_open(const char *rsrc_path, const char *gmspeech_path,
                         const char *gmbank_path, int *error)
{
    vwseq_engine *s = (vwseq_engine *)calloc(1, sizeof *s);
    int rc;
    if (s == NULL) {
        *error = VW_ERR_MEMORY;
        return NULL;
    }
    rc = vw_engine_open(&s->e, rsrc_path, gmspeech_path, gmbank_path, NULL);
    if (rc != VW_OK) {
        *error = rc;
        vw_engine_close(&s->e);
        free(s);
        return NULL;
    }
    *error = 0;
    return s;
}

void vwseq_close(vwseq_engine *s)
{
    if (s == NULL)
        return;
    vw_engine_close(&s->e);
    free(s);
}

static int g_done;                              /* the sequencer's done callback */

static void on_done(int32_t refCon)
{
    (void)refCon;
    g_done = 1;
}

int vwseq_sing(vwseq_engine *engine, const vwseq_part *part, vwseq_sink sink, void *user)
{
    vw_engine *e = &engine->e;
    shellVarPtr svv = e->svv;
    synthVarsPtr xx = svv->ChannelGlobals;
    int16_t *map = (int16_t *)e->voices, saved = 0, serr;
    vw_song s;
    TrackInfoPtr t;
    Handle h;
    PlayRec pr;
    unsigned char *trk;
    size_t trk_len = 0;
    int32_t len = 0, off;
    int i, rc, program = -1, patched = 0;
    double seconds = 0.0;

    if (part->voice < 0 || part->voice >= e->voiceCount)
        return -3;
    /* The voice is chosen as a song chooses it, by a program change at the
       first tick; one no program reaches borrows the last for the song. */
    for (i = 0; i < 128 && program < 0; i++)
        if (map[i] == part->voice)
            program = i;
    if (program < 0) {
        program = SPARE_PROGRAM;
        patched = 1;
        saved = map[program];
        map[program] = (int16_t)part->voice;
    }
    trk = write_song(part, program, &trk_len);
    rc = trk == NULL ? -2 : vw_song_load(&s, trk, trk_len, NULL, 0) != VW_OK ? -1 : 0;
    free(trk);
    if (rc != 0)
        goto restore;

    /* the speech channel before the levels: a level reaches only a voice */
    Synth_MakeAllTrackskNotSpeech(svv);
    serr = Synth_MakeTrackSpeech(svv, VOCAL_TRACK);
    if (serr != 0) {
        rc = serr;
        goto done;
    }
    Synth_SetKaraokeTrack(svv, -1);
    for (i = 0; i < 32; i++) {
        Synth_SetPlayTrack(svv, (int16_t)i, (int16_t)(i == VOCAL_TRACK));
        Synth_TrackToChan(svv, (int16_t)i, -1);
        Synth_SetTrackLevel(svv, (int16_t)i, s.trackLevels[i]);
    }

    /* MakeSpeechData, as StartCurSeq calls it, hung off the song */
    t = (TrackInfoPtr)(s.image + VOCAL_TRACK * TRACKINFO_SIZE);
    h = NewHandle(0);
    if (h == NULL) {
        rc = -2;
        goto done;
    }
    serr = Synth_MakeSpeechData(svv, s.image + t->lyricData, s.image + t->trackData, h, &len,
                                SPEECH_RATE);
    if (serr != 0 || len < 4) {
        DisposeHandle(h);
        rc = serr ? serr : -1;
        goto done;
    }
    nucleus_every_note((int16_t *)*h);
    if (part->consonants > 0 && part->consonants < 1.0)
        scale_consonants((int16_t *)*h, part->consonants);
    if (s.image_len + (size_t)len > s.image_cap) {
        unsigned char *p = (unsigned char *)realloc(s.image, s.image_len + (size_t)len);
        if (p == NULL) {
            DisposeHandle(h);
            rc = -2;
            goto done;
        }
        s.image = p;
        s.image_cap = s.image_len + (size_t)len;
    }
    off = (int32_t)s.image_len;
    memcpy(s.image + off, *h, (size_t)len);
    s.image_len += (size_t)len;
    DisposeHandle(h);
    t = (TrackInfoPtr)(s.image + VOCAL_TRACK * TRACKINFO_SIZE);
    t->speechData = off;
    t->speechDataLen = len;
    Synth_SetKbdFlags(svv, 0);
    Synth_SetReverb(svv, 0.0f, 0.0f, 0.0f);      /* the mixer's, over all the parts */
    Synth_StartMusic(svv);

    memset(&pr, 0, sizeof pr);
    pr.PbufStart = (Ptr)s.image;
    pr.theFlags = 1;
    pr.ticksPerBeat = TICKS_PER_BEAT;
    pr.polyphony = POLYPHONY;
    g_done = 0;
    Synth_SetSeqDoneCB(svv, on_done, 0);
    rc = Synth_SeqPlayer(svv, &pr);
    if (rc == 0) {
        int16_t mono[4096];
        /* Start_Speech gives channel 0 its vibrato from the top of the cycle
           and every other channel from half way; this part sings alone, on
           channel 0, so a later track's is set as its own channel would. */
        if (part->channel > 0) {
            formantVarPtr zz = (formantVarPtr)xx->speechVars[xx->SpeechMap[VOCAL_TRACK]];
            zz->vibrato_Phase1 = 128 << 16;
            zz->vibrato_Phase2 = 128 << 16;
        }
        while (!g_done) {
            Ptr out;
            int32_t blen, frames, k, n = 0;
            const int16_t *stereo;
            Synth_GetNextBuffer(svv, &out, &blen);
            stereo = (const int16_t *)out;
            frames = blen / 4;
            /* the voice is in the middle: both channels are the same */
            while (n < frames && !g_done) {
                int32_t m = frames - n > 4096 ? 4096 : frames - n;
                for (k = 0; k < m; k++)
                    mono[k] = stereo[2 * (n + k)];
                if (sink(user, mono, (size_t)m))
                    g_done = 1;
                n += m;
            }
            seconds += (double)frames / 44100.0;
            if (part->max_seconds > 0 && seconds >= part->max_seconds)
                break;
        }
        pr.theFlags = 0;
        Synth_SeqPlayer(svv, &pr);
        Synth_KillAllNotes(svv);
    }
    Synth_SetSeqDoneCB(svv, NULL, 0);
done:
    Synth_MakeAllTrackskNotSpeech(svv);
    vw_song_free(&s);
restore:
    if (patched)
        map[program] = saved;
    return rc;
}
