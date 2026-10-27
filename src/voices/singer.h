// singer.h -- what every voice engine is asked to do: sing one phrase.
//
// A phrase is a run of notes with no rest inside it. The renderer above this
// cuts a song at its rests, places each phrase at the time the score puts it,
// mixes the parts and adds the reverb; an engine only ever has to turn notes
// into samples. Everything here is in the studio's own terms -- VocalWriter's
// phoneme symbols, MIDI pitches, lengths in beats -- and each engine
// translates into its own.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace svs {

//: Every engine delivers 44.1 kHz mono floats, full scale at +-1. An engine
//: that runs at another rate resamples before handing its samples back.
constexpr int kSampleRate = 44100;

//: The phoneme that is silence. A note holding nothing else is a rest.
inline const char *const kRest = "%";

//: VocalWriter's own controls on a note, by name ("volume", "color", "noise",
//: "breath", "vibrato", "vibrato_rate", "portamento", "chorus", "detune",
//: "bend_range", "lead_pitch"), each [(where in the note 0..1, value)]. A
//: value holds, as a control does in VocalWriter, until the next one. Only
//: VocalWriter's voices sing them; see kNoteControls in core/project.h for
//: the ranges.
using NoteControls = std::map<std::string, std::vector<std::pair<double, double>>>;

//: One of those controls set where a controller stands, between two notes:
//: from `beat` on, `name` is `value`.
struct ControlAt {
    double beat;
    std::string name;
    double value;
};

//: One sung note. `durations_ms` has one entry per phoneme and they add up
//: to the note's length: the consonants at the length the singer gives them
//: and the vowels holding the rest (see phonology.h, syllable_lengths).
struct SungNote {
    int midi = 60;
    double beats = 0.5;
    int velocity = 64;
    std::vector<std::string> phonemes;
    std::vector<double> durations_ms;
    //: the lyric as written; "ov-" joins it to the next note's syllable
    std::string word;
    NoteControls controls;
    //: How long an instrument sounds it, if not `beats` (see Note::hold);
    //: only an engine that plays (Family::plays) hears it.
    double hold = 0;
};

//: A bend in force from `seconds` (from the start of the phrase) onwards, in
//: semitones. Already filled in between the written points at 5 ms steps, so
//: an engine can simply hold each value until the next one.
struct BendEvent {
    double seconds = 0.0;
    double semitones = 0.0;
};

//: VocalWriter's own voice controls, 0-127 except detune (-8192..8191). They
//: are the song's, or the part's own. Engines that have nothing matching a
//: control approximate it from the pitch curve (vibrato, detune,
//: portamento) or leave it alone.
struct VoiceStyle {
    int color = 95;
    int vibrato = 31;
    int vibrato_rate = 47;
    int chorus = 0;
    int breath = 0;
    int detune = 0;
    int portamento = 0;
    bool operator==(const VoiceStyle &o) const {
        return color == o.color && vibrato == o.vibrato &&
               vibrato_rate == o.vibrato_rate && chorus == o.chorus &&
               breath == o.breath && detune == o.detune &&
               portamento == o.portamento;
    }
    bool operator!=(const VoiceStyle &o) const { return !(*this == o); }
};

struct Phrase {
    double bpm = 120.0;
    //: The notes in order. The last one is always a marked rest of 0.4
    //: beats, velocity 1: something for the final syllable to be scaled
    //: against and room for it to decay rather than being cut off.
    std::vector<SungNote> notes;
    std::vector<BendEvent> bends;
    //: Whether the part has any bend written anywhere. VocalWriter's engine
    //: is put into its bend range only then, so a part without bends renders
    //: exactly as it always did. Other engines can ignore it.
    bool any_bends = false;
    //: The mod wheel, 0 to 127, from `seconds` onwards, filled in at 5 ms
    //: steps like the bends. As General MIDI has it, the wheel is vibrato: at
    //: 0 the part sings with its own vibrato setting, and turning it up takes
    //: the depth towards the control's full 127 (see effective_vibrato). Empty
    //: when the part has no mod wheel anywhere, which leaves it as it was.
    std::vector<BendEvent> mods;
    VoiceStyle style;
    //: The part's volume as a factor, already multiplied by whatever the
    //: voice needs to stay inside full scale.
    double level = 1.0;
    //: For an engine that keeps its own time (sings_parts): the consonant
    //: length, the beats of rest it starts the phrase with (its bends count
    //: from there), and how many of the song's parts on the same engine come
    //: before this one.
    double consonants = 1.0;
    double room = 0.0;
    int channel = 0;
    //: For such an engine too: the tempo changes inside the phrase, as (beats
    //: from its start, bpm), `bpm` being the tempo it starts at. Any other
    //: engine is given the notes already at `bpm`.
    std::vector<std::pair<double, double>> tempos;
    //: and the controls the notes before the phrase left standing
    std::map<std::string, double> held;
    //: and the controllers' controls inside it, in beats from its first note
    std::vector<ControlAt> controls;
};

//: A whole part, rests and all, from the song's first beat: for an engine
//: that sings a track as one stream, as VocalWriter's sequencer does.
struct Part {
    double bpm = 120.0;
    std::vector<SungNote> notes;
    std::vector<BendEvent> bends;     // in seconds from the song's start
    bool any_bends = false;
    std::vector<BendEvent> mods;
    VoiceStyle style;
    double level = 1.0;
    double consonants = 1.0;
    int channel = 0;
    std::vector<std::pair<double, double>> tempos;   // (beats from the start, bpm)
    //: (beats from the start, voice): the voices of the same engine the part
    //: goes on to, each from its beat on
    std::vector<std::pair<double, int>> voices;
    std::vector<ControlAt> controls;                 // beats from the start
    double end = 0;                                  // beats; 0 for a beat after the last note
};

//: The rendered phrase. `stopped_short` says the engine ran out of room or
//: frames before the singing finished, which the window reports.
struct Sung {
    std::vector<float> samples;
    bool stopped_short = false;
    //: Set when the engine could not sing the phrase at all; the samples are
    //: then nothing to keep.
    std::string error;
    //: how many seconds before the phrase's first beat the samples begin
    double starts_before = 0.0;
    //: 2 when `samples` are stereo, as an instrument's are
    int channels = 1;
};

//: How far into a phrase or part `beats` falls, in seconds, its tempo
//: changes (beats from its start, bpm) and all.
inline double seconds_into(double bpm, const std::vector<std::pair<double, double>> &tempos, double beats) {
    double s = 0, at = 0;
    for (const auto &c : tempos) {
        if (c.first >= beats) break;
        s += (c.first - at) * 60.0 / bpm;
        at = c.first;
        bpm = c.second;
    }
    return s + (beats - at) * 60.0 / bpm;
}

//: The vibrato depth control (0-127) a part sings with when its own setting
//: is `base` and the mod wheel stands at `mod`.
inline double effective_vibrato(int base, double mod) {
    double m = mod < 0 ? 0 : mod > 127 ? 127 : mod;
    return base + (127.0 - base) * m / 127.0;
}

class Singer {
public:
    virtual ~Singer() = default;
    virtual Sung sing(const Phrase &phrase) = 0;
    //: A whole part, for an engine that can sing one (Family::sings_parts).
    virtual Sung sing_part(const Part &) {
        Sung s;
        s.error = "this engine sings phrases only";
        return s;
    }
};

}  // namespace svs
