// mixer.h -- a song in, a WAV out.
//
// Each part is cut into phrases at its rests, each phrase sung by the part's
// voice and placed at the time the score puts it; the parts are panned, the
// ones sharing a reverb setting go through one reverberator together, and the
// whole is turned down if it adds up past full scale. A backing track's file
// is laid under the voices after the reverb, which is the voices' room alone. Finished renders are
// kept under a key made from the song, so playing the same thing twice
// renders once.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/project.h"
#include "voices/singer.h"

namespace svs {

struct PartRequest {
    int voice_id = 0;
    double volume = 1.0;     // fraction
    double pan = 0.0;        // -1 .. 1
    VoiceStyle voice;
    std::optional<Reverb> reverb;
    std::optional<double> consonants;
    int velocity = 64;
    struct N {
        int pitch;
        double beats;
        std::vector<std::string> phonemes;
    };
    std::vector<N> notes;
    std::vector<BendPoint> bends;   // (beat, semitones, slides)
    std::vector<BendPoint> mods;    // (beat, mod wheel 0-127, slides)
};

//: A backing track: a sound file played under the voices.
struct BackingRequest {
    std::string path;
    double volume = 1.0;     // fraction
    double pan = 0.0;        // -1 .. 1
    double start = 0.0;      // seconds into the song the file begins
    std::string name;        // the track's, for a message
};

struct RenderRequest {
    double bpm = 120;
    double consonants = 1.0;
    double start = 0.0;          // beats
    Reverb reverb{0, 0};
    bool anticipate = true;
    std::vector<PartRequest> tracks;
    std::vector<BackingRequest> backings;
    //: beats to the bar, when the metronome is wanted (never in an export)
    std::optional<double> metronome_bar;
};

//: A song mixed in memory, ready to play.
struct Mixed {
    bool ok = false;
    std::string error;
    std::shared_ptr<const std::vector<float>> samples;   //: interleaved when stereo
    int channels = 1;
    double seconds = 0;
    double peak = 0;
    //: nothing was sung: the same song had just been mixed
    bool cached = false;
    bool stopped_short = false;
};

//: The same, written to a WAV file: an export.
struct RenderResult {
    bool ok = false;
    std::string error;
    double seconds = 0;
    double peak = 0;
    std::string path;
    bool cached = false;
    bool stopped_short = false;
};

//: The song as the renderer wants it: `parts` are the tracks to sing and
//: nothing else, so mute and solo are settled by whoever calls this.
RenderRequest make_request(const Song &song, const std::vector<const Track *> &parts,
                           double start, const std::map<int, int> &program_map);

class Mixer {
public:
    Mixer();
    //: The song in memory, the metronome on it if the request asks for it.
    //: Only phrases not sung before are sung: the rest come from memory.
    Mixed mix(const RenderRequest &req);
    //: The song written to `out`, as 16-bit PCM: what Export does.
    RenderResult render(const RenderRequest &req, const std::string &out);

private:
    Mixed mix_locked(const RenderRequest &req);
    std::vector<float> track(const PartRequest &t, double bpm, double consonants, double start,
                             bool early, bool *stopped_short, std::string *error);
    //: a backing track's file, read the first time it is played and kept
    //: until it changes on disk
    std::shared_ptr<const std::vector<float>> backing(const std::string &path, std::string *error);
    std::mutex mu_;

    struct Read {
        long long size, time;
        std::shared_ptr<const std::vector<float>> samples;
        unsigned long long used;
    };
    std::map<std::string, Read> files_;

    //: Every phrase sung, kept in memory under a key made of everything that
    //: decides how it sounds -- the voice, its controls, its level, the notes,
    //: their phonemes and lengths, the bends and the wheel -- so that changing
    //: one note sings one phrase again, not the song. The phrases sung longest
    //: ago go first when the store passes kPhraseBytes.
    struct Kept {
        std::shared_ptr<const std::vector<float>> samples;
        bool stopped_short;
        unsigned long long used;
    };
    std::map<std::string, Kept> phrases_;
    size_t phrase_bytes_ = 0;
    unsigned long long clock_ = 0;
    //: and the last song mixed, so playing it again does nothing at all
    std::string last_key_;
    Mixed last_;
};

//: One tick of the metronome mixed in, accented at each bar line; `start` is
//: the beat the audio begins on so the ticks stay on the song's beats.
void with_metronome(std::vector<float> &y, int channels, double bpm, double bar_beats,
                    double start);

}  // namespace svs
