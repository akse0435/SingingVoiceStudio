/* vw_sequencer.h -- a part sung as VocalWriter 2.0 sings a vocal track.
 *
 * The part is written as a VocalWriter song in memory, its syllables parsed
 * by MakeSpeechData and played into a buffer as File > Play to Disk does,
 * all with the original's own code (engine/), so it comes out as VocalWriter
 * sang it. Plain C, to use the engine's records.
 */
#ifndef SVS_VW_SEQUENCER_H
#define SVS_VW_SEQUENCER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One note of the part, or a rest. Times in ticks, 240 to the beat. */
typedef struct vwseq_note {
    int32_t start;
    int32_t duration;
    int key;                        /* a rest carries the next note's */
    int velocity;
    int rest;
    char text[13];                  /* the lyric; a trailing "-" joins the next syllable */
    unsigned char phonemes[13];     /* a Pascal string of phoneme codes, at most 12 */
} vwseq_note;

/* A control as the sequencer reads it from a track (Update, engine/src/music.c). */
typedef struct vwseq_event {
    int32_t time;
    int status;
    int key;
    int vol;
} vwseq_event;

enum {
    VWSEQ_PITCH_BEND = 32,          /* 14 bits: key the low 7, vol the high 7 */
    VWSEQ_PB_RANGE = 38,
    VWSEQ_DETUNE = 39,              /* 14 bits */
    VWSEQ_COLOR = 64,
    VWSEQ_CHORUS = 66,              /* 14 bits */
    VWSEQ_VIB_DEPTH = 67,
    VWSEQ_VIB_FREQ = 68,
    VWSEQ_PORTAMENTO = 69,
    VWSEQ_BREATH = 70
};

typedef struct vwseq_part {
    int bpm;                        /* 10-250, as a tempo event holds it */
    int voice;                      /* the voice bank's own index */
    const vwseq_note *notes;        /* in time order */
    int note_count;
    const vwseq_event *events;      /* in time order */
    int event_count;
    int track_level;                /* 0-100 */
    /* the speech channel the song would give the track: a song's first
       vocal track starts its vibrato half a cycle from the others' */
    int channel;
    /* the consonants' share, 1 being VocalWriter's own: below it the
       opening ones are shortened by the factor, the closing ones by half */
    double consonants;
    double max_seconds;             /* a stop, should the song never end */
} vwseq_part;

/* Mono samples as they come, 16-bit, 44100 Hz. Nonzero stops. */
typedef int (*vwseq_sink)(void *user, const int16_t *samples, size_t count);

typedef struct vwseq_engine vwseq_engine;

/* The engine as the application starts it (vw_engine_open); NULL, with
   `*error` set, if it will not. `gmbank_path` may be NULL. */
vwseq_engine *vwseq_open(const char *rsrc_path, const char *gmspeech_path,
                         const char *gmbank_path, int *error);
void vwseq_close(vwseq_engine *engine);

/* Sing the part; one caller at a time. 0, or a negative error: -1 refused,
   -2 out of memory, -3 no such voice; or the engine's own error code. */
int vwseq_sing(vwseq_engine *engine, const vwseq_part *part, vwseq_sink sink, void *user);

#ifdef __cplusplus
}
#endif

#endif
