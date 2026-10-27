/* vw_sequencer.c -- parts as a VocalWriter song (see vw_sequencer.h):
 * written as a .trk, read back with the engine's loader and played as
 * engine/src/song.c's vw_render does.
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
#define MAX_PARTS 31                /* the tracks after the tempo track */
#define REST_PHONEME 23

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

/* A key folded into C1 to C7, as VocalWriter's MIDI import folds it. */
static int instrument_key(int key)
{
    while (key > 96)
        key -= 12;
    while (key <= 23)
        key += 12;
    return key;
}

/* One part as a track: its events, the controls at and before a note in
   front of it, and a vocal track's lyrics. Returns the last tick it sings. */
static int32_t write_part(const vwseq_part *part, int track, int program, buf *vocal, buf *lyrics,
                          int *lyric_count)
{
    int32_t last_sung = 0;
    int i, ei = 0, lyric = 0;

    /* the drum kit has no program: its keys are its drums */
    if (part->kind != VWSEQ_DRUMS && !part->voice_later)
        event(vocal, track, 0, 3, 0, program, 0, 0);
    for (i = 0; i < part->note_count; i++) {
        const vwseq_note *n = &part->notes[i];
        while (ei < part->event_count && (part->events[ei].time < n->start ||
                                          (part->events[ei].time == n->start && !part->events[ei].after_note))) {
            const vwseq_event *e = &part->events[ei++];
            event(vocal, track, e->time, e->status, e->key, e->vol, 0, 0);
        }
        if (part->kind != VWSEQ_VOCAL) {
            /* a note with its length; the song's end is the singing's */
            if (n->rest)
                continue;
            event(vocal, track, n->start, 1, instrument_key(n->key), clamp(n->velocity, 1, 127),
                  n->duration < 1 ? 1 : n->duration, 0);
            continue;
        }
        if (n->rest) {
            event(vocal, track, n->start, 8, n->key, 0, n->duration, 0);
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
            put(lyrics, rec, 26);
        }
        event(vocal, track, n->start, 6, n->key, clamp(n->velocity, 1, 127), n->duration, lyric++);
        if (n->start + n->duration > last_sung)
            last_sung = n->start + n->duration;
    }
    while (ei < part->event_count) {
        const vwseq_event *e = &part->events[ei++];
        event(vocal, track, e->time, e->status, e->key, e->vol, 0, 0);
    }
    put(vocal, kEnd, 4);
    *lyric_count = lyric;
    return last_sung;
}

/* The parts as a .trk file: the header, the 32 track records, the tempo
   track, a vocal track and its lyrics for each part, from track 1, and the
   empty tracks. */
static unsigned char *write_song(const vwseq_part *parts, int count, const int *programs,
                                 size_t *out_len)
{
    buf tempo = {0}, file = {0}, vocal[MAX_PARTS], lyrics[MAX_PARTS];
    unsigned char header[SEQHEADER_SIZE], info[SEQINFO_SIZE];
    int32_t last_sung = 0, offset;
    int i, lyric_count[MAX_PARTS], bpm = clamp(parts[0].bpm, 10, 250), failed = 0;

    memset(vocal, 0, sizeof vocal);
    memset(lyrics, 0, sizeof lyrics);
    for (i = 0; i < count; i++) {
        int32_t last = write_part(&parts[i], 1 + i, programs[i], &vocal[i], &lyrics[i], &lyric_count[i]);
        if (last > last_sung)
            last_sung = last;
    }
    /* the tempo and its changes, and the end mark a beat after the last
       sung note */
    event(&tempo, 0, 0, 4, bpm >> 8, bpm & 0xff, 0, 0);
    for (i = 0; i < parts[0].tempo_count; i++) {
        int t = clamp(parts[0].tempos[i].bpm, 10, 250);
        event(&tempo, 0, parts[0].tempos[i].time, 4, t >> 8, t & 0xff, 0, 0);
    }
    event(&tempo, 0, parts[0].end >= last_sung && parts[0].end > 0 ? parts[0].end : last_sung + TICKS_PER_BEAT, 9,
          0, 0, 0, 0);
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
        be16(header + 0x16 + 2 * i, i >= 1 && i <= count ? clamp(parts[i - 1].track_level, 0, 200) : 100);
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
        } else if (i <= count && parts[i - 1].kind != VWSEQ_VOCAL) {
            /* an instrument track, flagged 2 for the drum kit */
            be32(t + 0x4, parts[i - 1].kind == VWSEQ_DRUMS ? 2 : 0);
            be32(t + 0x8, offset);
            be32(t + 0xc, (int32_t)vocal[i - 1].len);
            offset += (int32_t)vocal[i - 1].len;
        } else if (i <= count) {
            be32(t + 0x4, 1);                    /* a vocal track */
            be32(t + 0x8, offset);
            be32(t + 0xc, (int32_t)vocal[i - 1].len);
            offset += (int32_t)vocal[i - 1].len;
            be32(t + 0x18, offset);
            be32(t + 0x1c, lyric_count[i - 1]);
            be32(t + 0x20, 13);
            offset += (int32_t)lyrics[i - 1].len;
        } else {
            be32(t + 0x8, offset);
            be32(t + 0xc, 4);
            offset += 4;
        }
    }
    put(&file, header, sizeof header);
    put(&file, info, sizeof info);
    put(&file, tempo.p, tempo.len);
    for (i = 0; i < count; i++) {
        put(&file, vocal[i].p, vocal[i].len);
        if (lyrics[i].len)
            put(&file, lyrics[i].p, lyrics[i].len);
        failed |= vocal[i].failed | lyrics[i].failed;
        free(vocal[i].p);
        free(lyrics[i].p);
    }
    for (i = count + 1; i < 32; i++)
        put(&file, kEnd, 4);
    free(tempo.p);
    if (file.failed || tempo.failed || failed) {
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

int vwseq_has_bank(vwseq_engine *s)
{
    return s != NULL && s->e.hasBank;
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

/* MakeSpeechData for a vocal track, as StartCurSeq calls it, hung off the
   song, its consonants shortened if asked. */
static int make_speech(vw_engine *e, vw_song *s, int track, double consonants)
{
    TrackInfoPtr t = (TrackInfoPtr)(s->image + track * TRACKINFO_SIZE);
    Handle h = NewHandle(0);
    int32_t len = 0, off;
    int16_t serr;
    if (h == NULL)
        return -2;
    serr = Synth_MakeSpeechData(e->svv, s->image + t->lyricData, s->image + t->trackData, h, &len,
                                SPEECH_RATE);
    if (serr != 0 || len < 4) {
        DisposeHandle(h);
        return serr ? serr : -1;
    }
    nucleus_every_note((int16_t *)*h);
    if (consonants > 0 && consonants < 1.0)
        scale_consonants((int16_t *)*h, consonants);
    if (s->image_len + (size_t)len > s->image_cap) {
        unsigned char *p = (unsigned char *)realloc(s->image, s->image_len + (size_t)len);
        if (p == NULL) {
            DisposeHandle(h);
            return -2;
        }
        s->image = p;
        s->image_cap = s->image_len + (size_t)len;
    }
    off = (int32_t)s->image_len;
    memcpy(s->image + off, *h, (size_t)len);
    s->image_len += (size_t)len;
    DisposeHandle(h);
    t = (TrackInfoPtr)(s->image + track * TRACKINFO_SIZE);
    t->speechData = off;
    t->speechDataLen = len;
    return 0;
}

/* The programs a song's voices are reached by: the bank's own program for a
   voice one reaches, and for any other one borrowed from the top, put back
   when the song is over. */
typedef struct {
    int voice[128], program[128], count;
    int patched[128], saved[128], npatched;
} reach;

static int program_of(const reach *r, int voice)
{
    int i;
    for (i = 0; i < r->count; i++)
        if (r->voice[i] == voice)
            return r->program[i];
    return -1;
}

static void need(reach *r, int voice)
{
    int i;
    for (i = 0; i < r->count; i++)
        if (r->voice[i] == voice)
            return;
    if (r->count < 128) {
        r->voice[r->count] = voice;
        r->program[r->count++] = -1;
    }
}

static int settle(int16_t *map, reach *r)
{
    int i, k, p = 127;
    for (i = 0; i < r->count; i++)
        for (k = 0; k < 128 && r->program[i] < 0; k++)
            if (map[k] == r->voice[i])
                r->program[i] = k;
    for (i = 0; i < r->count; i++) {
        if (r->program[i] >= 0)
            continue;
        for (;; --p) {
            int taken = 0;
            if (p < 0)
                return -1;
            for (k = 0; k < r->count; k++)
                if (r->program[k] == p)
                    taken = 1;
            if (!taken)
                break;
        }
        r->program[i] = p;
        r->patched[r->npatched] = p;
        r->saved[r->npatched++] = map[p];
        map[p] = (int16_t)r->voice[i];
    }
    return 0;
}

static void unsettle(int16_t *map, const reach *r)
{
    int i;
    for (i = r->npatched - 1; i >= 0; i--)
        map[r->patched[i]] = (int16_t)r->saved[i];
}

/* The parts as one song, played as the application plays it: `channels` 1
   gives the left channel (a voice is in the middle), 2 both. */
static int sing(vwseq_engine *engine, const vwseq_part *parts, int count, int room, int wet,
                int channels, vwseq_sink sink, void *user)
{
    vw_engine *e = &engine->e;
    shellVarPtr svv = e->svv;
    synthVarsPtr xx = svv->ChannelGlobals;
    int16_t *map = (int16_t *)e->voices, serr;
    vw_song s;
    PlayRec pr;
    unsigned char *trk;
    size_t trk_len = 0;
    int i, k, rc = 0, programs[MAX_PARTS];
    double seconds = 0.0, max_seconds = 0.0;
    reach r;
    vwseq_part sung[MAX_PARTS];
    vwseq_event *events[MAX_PARTS];

    if (count < 1 || count > MAX_PARTS)
        return -1;
    memset(&r, 0, sizeof r);
    memset(events, 0, sizeof events);
    /* A voice is chosen as a song chooses it, by a program change: at the
       first tick, and wherever a part changes voice. */
    for (i = 0; i < count; i++) {
        if (parts[i].max_seconds > max_seconds)
            max_seconds = parts[i].max_seconds;
        if (parts[i].kind != VWSEQ_VOCAL) {
            /* an instrument is the bank's, by GM program */
            if (!e->hasBank)
                return -4;
            if (parts[i].kind == VWSEQ_INSTRUMENT && (parts[i].voice < 0 || parts[i].voice > 127))
                return -3;
            continue;
        }
        if (parts[i].voice < 0 || parts[i].voice >= e->voiceCount)
            return -3;
        need(&r, parts[i].voice);
        for (k = 0; k < parts[i].event_count; k++)
            if (parts[i].events[k].status == VWSEQ_SET_VOICE) {
                if (parts[i].events[k].vol < 0 || parts[i].events[k].vol >= e->voiceCount)
                    return -3;
                need(&r, parts[i].events[k].vol);
            }
    }
    if (settle(map, &r) != 0) {
        rc = -1;
        goto restore;
    }
    for (i = 0; i < count; i++) {
        sung[i] = parts[i];
        programs[i] = parts[i].kind == VWSEQ_VOCAL ? program_of(&r, parts[i].voice) : parts[i].voice;
        if (parts[i].event_count == 0)
            continue;
        events[i] = (vwseq_event *)malloc(sizeof(vwseq_event) * (size_t)parts[i].event_count);
        if (events[i] == NULL) {
            rc = -2;
            goto restore;
        }
        for (k = 0; k < parts[i].event_count; k++) {
            events[i][k] = parts[i].events[k];
            if (events[i][k].status != VWSEQ_SET_VOICE)
                continue;
            events[i][k].status = 3;
            events[i][k].key = 0;
            if (parts[i].kind == VWSEQ_VOCAL) {
                events[i][k].vol = program_of(&r, parts[i].events[k].vol);
            } else if (parts[i].kind == VWSEQ_DRUMS || parts[i].events[k].vol < 0 ||
                       parts[i].events[k].vol > 127) {
                /* a whole track is drums or not, so the change is dropped:
                   status 0 is nothing */
                events[i][k].status = 0;
            }
        }
        sung[i].events = events[i];
    }
    trk = write_song(sung, count, programs, &trk_len);
    rc = trk == NULL ? -2 : vw_song_load(&s, trk, trk_len, NULL, 0) != VW_OK ? -1 : 0;
    free(trk);
    if (rc != 0)
        goto restore;

    /* the speech channels before the levels: a level reaches only a voice */
    Synth_MakeAllTrackskNotSpeech(svv);
    for (i = 0; i < count; i++) {
        if (parts[i].kind != VWSEQ_VOCAL)
            continue;
        serr = Synth_MakeTrackSpeech(svv, (int16_t)(1 + i));
        if (serr != 0) {
            rc = serr;
            goto done;
        }
    }
    Synth_SetKaraokeTrack(svv, -1);
    for (i = 0; i < 32; i++) {
        Synth_SetPlayTrack(svv, (int16_t)i, (int16_t)(i >= 1 && i <= count));
        Synth_TrackToChan(svv, (int16_t)i, -1);
        Synth_SetTrackLevel(svv, (int16_t)i, s.trackLevels[i]);
    }
    for (i = 0; i < count; i++)
        if (parts[i].kind == VWSEQ_VOCAL && (rc = make_speech(e, &s, 1 + i, parts[i].consonants)) != 0)
            goto done;
    Synth_SetKbdFlags(svv, 0);
    /* the reverb, as SendReverbParams sends it */
    if (wet > 0)
        Synth_SetReverb(svv, (float)room / 100.0f, (float)wet / 100.0f, 1.0f - (float)wet / 100.0f);
    else
        Synth_SetReverb(svv, 0.0f, 0.0f, 0.0f);
    Synth_StartMusic(svv);
    /* the song's start tempo, which Play to Disk sets before it starts */
    Synth_SetTempo(svv, (int16_t)clamp(parts[0].bpm, 10, 250));

    memset(&pr, 0, sizeof pr);
    pr.PbufStart = (Ptr)s.image;
    pr.theFlags = 1;
    pr.ticksPerBeat = TICKS_PER_BEAT;
    pr.polyphony = POLYPHONY;
    g_done = 0;
    Synth_SetSeqDoneCB(svv, on_done, 0);
    rc = Synth_SeqPlayer(svv, &pr);
    if (rc == 0) {
        int16_t out[4096];
        /* Start_Speech gives the first speech channel its vibrato from the
           top of the cycle and every other one from half way. A part sung
           here without the parts before it in its song is set as its own
           channel would be. */
        for (i = k = 0; i < count; i++) {
            if (parts[i].kind != VWSEQ_VOCAL)
                continue;
            if ((parts[i].channel > 0) != (k > 0)) {
                formantVarPtr zz = (formantVarPtr)xx->speechVars[xx->SpeechMap[1 + i]];
                zz->vibrato_Phase1 = parts[i].channel > 0 ? 128 << 16 : 0;
                zz->vibrato_Phase2 = zz->vibrato_Phase1;
            }
            k++;
        }
        int stop = 0;
        while (!g_done && !stop) {
            Ptr buf;
            int32_t blen, frames, n = 0;
            const int16_t *stereo;
            Synth_GetNextBuffer(svv, &buf, &blen);
            stereo = (const int16_t *)buf;
            frames = blen / 4;
            /* the whole buffer, the one the song ends in too, as Play to
               Disk writes it */
            while (n < frames && !stop) {
                int32_t m = frames - n > 2048 ? 2048 : frames - n, j;
                for (j = 0; j < m * channels; j++)
                    out[j] = channels == 2 ? stereo[2 * n + j] : stereo[2 * (n + j)];
                stop = sink(user, out, (size_t)(m * channels));
                n += m;
            }
            seconds += (double)frames / 44100.0;
            if (max_seconds > 0 && seconds >= max_seconds)
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
    unsettle(map, &r);
    for (i = 0; i < count; i++)
        free(events[i]);
    return rc;
}

int vwseq_sing(vwseq_engine *engine, const vwseq_part *part, vwseq_sink sink, void *user)
{
    return sing(engine, part, 1, 0, 0, part->kind == VWSEQ_VOCAL ? 1 : 2, sink, user);
}

int vwseq_sing_song(vwseq_engine *engine, const vwseq_song *song, vwseq_sink sink, void *user)
{
    return sing(engine, song->parts, song->part_count, song->reverb_room, song->reverb_wet, 2, sink, user);
}
