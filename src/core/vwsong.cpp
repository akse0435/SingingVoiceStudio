#include "core/vwsong.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>

#include "core/paths.h"
#include "core/phonology.h"
#include "voices/family.h"

namespace svs {

namespace {

// The file's layout: a 758-byte header, then the SeqInfo (32 TrackInfo
// records of 88 bytes) at the offset the header gives, every offset inside it
// relative to the SeqInfo. All big-endian, as the PowerPC wrote it. See
// engine/docs/PORT.md and engine/src/song.c.
const size_t kHeaderSize = 758;
const size_t kTrackInfoSize = 88;
const size_t kTracks = 32;
const int kVocal = 0x1;
const int kDrums = 0x2;              // an instrument track on MIDI's channel 10
const int kKaraoke = 0x10;

// a track's events: 12 bytes each, a 24-bit time first, 0xffffff ending them
const int kPlayed = 1;               // an instrument's note, with its length
const int kNote = 6;
const int kRestEvent = 8;
const int kProgram = 3;
const int kTempo = 4;
const int kPitchBend = 32;
const int kVolume = 33;
const int kSustain = 34;
const int kBendRange = 38;
const int kDetune = 39;
const int kEnd = 9;

uint32_t be32(const unsigned char *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
int be16s(const unsigned char *p) { return int16_t(uint16_t((p[0] << 8) | p[1])); }
uint32_t be24(const unsigned char *p) { return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]; }

struct Event {
    uint32_t time;
    int status, key, vol;
    uint32_t dur;
    int lyric;
};

//: The resource fork, wherever this system keeps it beside the file: the
//: Mac's named fork, an NTFS stream or a copy's ".rsrc", or an AppleDouble
//: "._" file. Empty if there is none.
std::vector<unsigned char> resource_fork(const std::string &path) {
    std::vector<unsigned char> fork;
    for (const std::string &p : {path + "/..namedfork/rsrc", path + ":rsrc", path + ".rsrc"})
        if (read_file(p, &fork) && !fork.empty()) return fork;
    std::string dir = path, file = base_name(path);
    dir.resize(path.size() - file.size());
    std::vector<unsigned char> dbl;
    if (read_file(dir + "._" + file, &dbl) && dbl.size() > 26 && be32(dbl.data()) == 0x00051607) {
        int n = int(uint16_t((dbl[24] << 8) | dbl[25]));
        for (int i = 0; i < n && 26 + size_t(i) * 12 + 12 <= dbl.size(); ++i) {
            const unsigned char *e = dbl.data() + 26 + i * 12;
            if (be32(e) != 2) continue;               // the resource fork's entry
            size_t off = be32(e + 4), len = be32(e + 8);
            if (off + len <= dbl.size()) return std::vector<unsigned char>(dbl.begin() + off, dbl.begin() + off + len);
        }
    }
    return {};
}

//: One resource out of a resource fork, or empty.
std::vector<unsigned char> resource(const std::vector<unsigned char> &fork, const char *type, int id) {
    if (fork.size() < 16) return {};
    size_t data = be32(&fork[0]), map = be32(&fork[4]);
    if (map + 28 > fork.size()) return {};
    size_t types = map + size_t(uint16_t((fork[map + 24] << 8) | fork[map + 25]));
    if (types + 2 > fork.size()) return {};
    int ntypes = be16s(&fork[types]) + 1;
    for (int t = 0; t < ntypes; ++t) {
        size_t e = types + 2 + size_t(t) * 8;
        if (e + 8 > fork.size()) return {};
        if (std::string(reinterpret_cast<const char *>(&fork[e]), 4) != type) continue;
        int count = be16s(&fork[e + 4]) + 1;
        size_t refs = types + size_t(uint16_t((fork[e + 6] << 8) | fork[e + 7]));
        for (int r = 0; r < count; ++r) {
            size_t ref = refs + size_t(r) * 12;
            if (ref + 12 > fork.size()) return {};
            if (be16s(&fork[ref]) != id) continue;
            size_t at = data + be24(&fork[ref + 5]);
            if (at + 4 > fork.size()) return {};
            size_t len = be32(&fork[at]);
            if (at + 4 + len > fork.size()) return {};
            return std::vector<unsigned char>(fork.begin() + at + 4, fork.begin() + at + 4 + len);
        }
    }
    return {};
}

//: what each control's status is called in a note's controls (project.h)
const std::map<int, const char *> &control_names() {
    static const std::map<int, const char *> names = {
        {33, "volume"},  {64, "color"},       {71, "noise"},      {70, "breath"},
        {67, "vibrato"}, {68, "vibrato_rate"}, {69, "portamento"}, {66, "chorus"},
        {39, "detune"},
    };
    return names;
}

//: A part as it is being put together: the notes VocalWriter sings, at their
//: beats, and every control at its beat.
struct SungAt {
    double start, end;
    int key, velocity;
    int lead = -1;           // the key of the rest before it, from the file
    std::string word;
    std::vector<std::string> phonemes;
};
struct Control {
    double beat;
    std::string name;        // a note control, or "" for a bend
    double value;            // semitones for a bend
    bool before = false;     // written before a note that starts at its moment
};

//: The part's notes in a row, rests between them, each control on the note
//: or rest it falls in. The bend becomes each note's own curve, held across
//: the notes as VocalWriter holds it, and drawn in steps.
std::vector<Note> lay_out(const std::vector<SungAt> &sung, const std::vector<Control> &controls) {
    std::vector<Note> out;
    std::vector<double> starts;
    double at = 0.0;
    const double eps = 1e-9;
    for (size_t i = 0; i < sung.size(); ++i) {
        const SungAt &s = sung[i];
        double end = s.end;
        if (i + 1 < sung.size()) end = std::min(end, sung[i + 1].start);
        if (s.start > at + eps) {
            starts.push_back(at);
            Note rest({kRest}, s.key, s.start - at);
            // a rest keyed to another pitch than the note after it (Daisy)
            if (s.lead >= 0 && s.lead != s.key) rest.controls["lead_pitch"] = {{0.0, double(s.lead)}};
            out.push_back(rest);
            at = s.start;
        }
        double from = std::max(at, s.start);
        if (end <= from + eps) continue;              // swallowed by the note before
        starts.push_back(from);
        Note n(s.phonemes, s.key, end - from, s.word);
        n.velocity = s.velocity;
        out.push_back(n);
        at = end;
    }
    if (out.empty()) return out;
    // the rest VocalWriter ends a track with, a beat long, or as far as the
    // controls still to come, which go on changing the voice as it dies away
    double last = at + 1.0;
    for (const Control &c : controls) last = std::max(last, c.beat);
    starts.push_back(at);
    out.push_back(Note({kRest}, out.back().pitch, last - at));
    auto element = [&](double beat) -> long {
        auto it = std::upper_bound(starts.begin(), starts.end(), beat + eps);
        return it == starts.begin() ? -1 : long(it - starts.begin()) - 1;
    };
    // a change of voice inside a rest divides it, so that the change can
    // stand between the two halves; VocalWriter's sequencer sings them as one
    for (const Control &c : controls) {
        if (c.name != "voice") continue;
        long k = element(c.beat);
        if (k < 0 || !out[size_t(k)].is_rest()) continue;
        double a = starts[size_t(k)], b = a + out[size_t(k)].beats;
        if (c.beat <= a + eps || c.beat >= b - eps) continue;
        Note second({kRest}, out[size_t(k)].pitch, b - c.beat);
        out[size_t(k)].beats = c.beat - a;
        out.insert(out.begin() + k + 1, second);
        starts.insert(starts.begin() + k + 1, c.beat);
    }
    auto position = [&](size_t k, double beat) {
        return std::max(0.0, std::min(1.0, (beat - starts[k]) / out[k].beats));
    };
    // the controls, onto the note or rest they fall in; after the last note
    // they would change nothing, and before the first they belong to it
    for (const Control &c : controls) {
        if (c.name.empty() || c.name == "voice") continue;
        long k = element(c.beat);
        if (k < 0) k = 0;
        if (size_t(k) >= out.size()) continue;
        if (c.beat > starts[size_t(k)] + out[size_t(k)].beats + eps) continue;
        // one at the moment a note starts, written before the note in the
        // file, ends the note or rest before it, as it is sung there
        if (k > 0 && c.before && c.beat <= starts[size_t(k)] + eps) --k;
        Note &n = out[size_t(k)];
        // every one, even the same value twice at once, as VocalWriter sends it
        n.controls[c.name].push_back({position(size_t(k), c.beat), c.value});
    }
    // the bend: held across notes and rests, so each one starts from the
    // value in force, and it moves in steps, as VocalWriter's does
    std::vector<std::pair<double, double>> bends;
    for (const Control &c : controls)
        if (c.name.empty()) bends.push_back({c.beat, c.value});
    double held = 0.0;
    size_t bi = 0;
    for (size_t k = 0; k < out.size(); ++k) {
        double a = starts[k], b = a + out[k].beats;
        while (bi < bends.size() && bends[bi].first <= a + eps) held = bends[bi++].second;
        Bend inside;
        while (bi < bends.size() && bends[bi].first < b - eps) {
            inside.push_back({position(k, bends[bi].first), bends[bi].second});
            ++bi;
        }
        if (inside.empty() && std::fabs(held) < kBendEpsilon) continue;
        // the value in force, unless the note's own first bend is at its start
        Bend steps;
        if (inside.empty() || inside[0].first > 0.0) steps.push_back({0.0, held});
        double last = held;
        for (const auto &p : inside) {
            // a change is a step, the old value held up to it; the same value
            // again is sent again, as VocalWriter sends it
            if (p.second != last && !steps.empty()) steps.push_back({p.first, last});
            steps.push_back(p);
            last = p.second;
        }
        if (steps.size() == 1 && std::fabs(held) < kBendEpsilon) continue;
        out[k].bend = steps;
        held = last;
    }
    // the changes of voice, each a controller where it falls: before the
    // note or rest that starts there, or after a note it falls inside
    std::vector<std::vector<Note>> before(out.size() + 1);
    for (const Control &c : controls) {
        if (c.name != "voice") continue;
        size_t k = 0;
        while (k < out.size() && starts[k] + out[k].beats <= c.beat + eps) ++k;
        if (k < out.size() && starts[k] < c.beat - eps) ++k;
        Note change = Note::tempo_change(0);
        change.voice_id = kVocalWriterBase + int(c.value);
        before[k].push_back(change);
    }
    std::vector<Note> all;
    for (size_t k = 0; k <= out.size(); ++k) {
        all.insert(all.end(), before[k].begin(), before[k].end());
        if (k < out.size()) all.push_back(out[k]);
    }
    return all;
}

//: An instrument track: its notes, volume, pedal, bend, bend range and
//: detune, and program changes as changes of instrument. The drum track has
//: the kit; VocalWriter ignores its program changes.
template <class Beat>
Track instrument_track(const std::vector<Event> &events, bool drums, Beat beat) {
    std::vector<PlayedNote> notes;
    std::vector<PlayedChange> changes;
    int program = -1, range = 2;
    for (const Event &e : events) {
        double b = beat(e.time);
        if (e.status == kPlayed || e.status == kNote) {
            notes.push_back({b, beat(e.time + std::max<uint32_t>(e.dur, 1)) - b, e.key, std::max(1, e.vol)});
        } else if (e.status == kProgram && !drums) {
            // the first program is the track's, any later a change
            bool changed = changes.end() != std::find_if(changes.begin(), changes.end(),
                                                         [](const PlayedChange &c) { return c.name == "voice"; });
            if (!changed && (notes.empty() || b <= notes.front().start))
                program = e.vol;
            else
                changes.push_back({b, "voice", double(kGmBase + e.vol)});
        } else if (e.status == kVolume) {
            changes.push_back({b, "volume", double(e.vol)});
        } else if (e.status == kSustain) {
            changes.push_back({b, "sustain", double(e.vol)});
        } else if (e.status == kBendRange) {
            range = std::max(1, std::min(12, e.vol));
            changes.push_back({b, "bend_range", double(range)});
        } else if (e.status == kPitchBend) {
            changes.push_back({b, "", ((e.vol << 7) + e.key - 8192) / 8192.0 * range});
        } else if (e.status == kDetune) {
            changes.push_back({b, "detune", double((e.vol << 7) + e.key - 8192)});
        }
    }
    Track t;
    t.notes = lay_out_played(notes, changes);
    t.velocity = common_velocity(t.notes);
    // with no program VocalWriter plays the piano
    t.voice_id = drums ? kGmDrums : kGmBase + std::max(0, program);
    return t;
}

}  // namespace

bool is_vocalwriter_song(const std::string &path) { return extension_lower(path) == ".trk"; }

VwSongImport import_vocalwriter_song(const std::string &path, const std::vector<int> &program_voices) {
    std::vector<unsigned char> d;
    if (!read_file(path, &d)) throw std::runtime_error("cannot read the file");
    if (d.size() < kHeaderSize + kTracks * kTrackInfoSize)
        throw std::runtime_error("not a VocalWriter song");
    size_t seq = be32(&d[8]);
    if (seq < kHeaderSize || seq + kTracks * kTrackInfoSize > d.size())
        throw std::runtime_error("not a VocalWriter song");
    VwSongImport res;
    Song &song = res.song;
    int tempo = be16s(&d[0xc]);
    int meter = be16s(&d[0xe]), beat_val = be16s(&d[0x10]);
    if (tempo < 10 || tempo > 300 || beat_val < 0 || beat_val > 4)
        throw std::runtime_error("not a VocalWriter song");
    double ticks_per_beat = double(960 >> beat_val);
    auto beat = [&](uint32_t tick) { return tick / ticks_per_beat; };
    song.sig = {meter > 0 ? meter : 4, 1 << beat_val};
    // the portamento VocalWriter's songs start with, which they never write
    song.voice.portamento = kVocalWriterPortamento;
    auto voice_of_program = [&](int program) {
        if (program >= 0 && size_t(program) < program_voices.size() && program_voices[size_t(program)] >= 0)
            return program_voices[size_t(program)];
        return 0;
    };

    // the tracks and their events
    struct TrackRec {
        int flags;
        std::string name;
        std::vector<Event> events;
        std::vector<std::pair<std::string, std::vector<std::string>>> lyrics;   // (text, phonemes)
        int level;
        bool muted;
    };
    std::vector<TrackRec> tracks;
    std::vector<std::pair<uint32_t, int>> tempos;       // (tick, bpm)
    const unsigned char *info = &d[seq];
    for (size_t i = 0; i < kTracks; ++i) {
        const unsigned char *t = info + i * kTrackInfoSize;
        TrackRec r;
        r.flags = int(be32(t + 4));
        size_t data = be32(t + 8), len = be32(t + 0xc);
        int nlen = std::min<int>(t[0x38], 31);
        r.name.assign(reinterpret_cast<const char *>(t + 0x39), size_t(nlen));
        r.level = be16s(&d[0x16 + 2 * i]);
        r.muted = be16s(&d[0x56 + 2 * i]) == 0;
        if (seq + data > d.size()) continue;
        for (size_t p = seq + data; p + 12 <= d.size() && p < seq + data + std::max<size_t>(len, 4); p += 12) {
            const unsigned char *e = &d[p];
            if (be24(e) == 0xffffff) break;
            r.events.push_back({be24(e), e[4], e[5], e[6], be24(e + 7), (e[10] << 8) | e[11]});
        }
        for (const Event &e : r.events)
            if (e.status == kTempo) tempos.push_back({e.time, (e.key << 8) | e.vol});
        if (r.flags & kVocal) {
            size_t lyr = be32(t + 0x18), count = be32(t + 0x1c), width = be32(t + 0x20);
            if (width == 0) width = 13;
            for (size_t k = 0; k < count; ++k) {
                size_t at = seq + lyr + k * 2 * width;
                if (at + 2 * width > d.size()) break;
                std::string text(reinterpret_cast<const char *>(&d[at + 1]), std::min<size_t>(d[at], width - 1));
                std::vector<std::string> ph;
                const auto &table = phonemes();
                for (size_t j = 0; j < std::min<size_t>(d[at + width], width - 1); ++j) {
                    int code = d[at + width + 1 + j];
                    if (code < int(table.size())) ph.push_back(table[size_t(code)].symbol);
                }
                r.lyrics.push_back({text, ph});
            }
        }
        tracks.push_back(r);
    }
    // the tempo and its changes, as the song's own
    std::stable_sort(tempos.begin(), tempos.end(),
                     [](const std::pair<uint32_t, int> &a, const std::pair<uint32_t, int> &b) { return a.first < b.first; });
    song.bpm = tempo;
    std::vector<TempoChange> changes;
    for (const auto &c : tempos) {
        if (c.first == 0) song.bpm = c.second;
        else changes.push_back({beat(c.first), double(c.second)});
    }
    changes = clean_tempo_changes(changes);

    // the reverb the song was saved with (SongRez, in its resource fork)
    std::vector<unsigned char> sdat = resource(resource_fork(path), "sDat", 1);
    if (sdat.size() >= 0x220) {
        int room = be16s(&sdat[0x214]), wet = be16s(&sdat[0x218]), off = be16s(&sdat[0x21e]);
        song.reverb = off ? Reverb{0, 0} : Reverb{std::max(0, std::min(100, room)), std::max(0, std::min(100, wet))};
    } else {
        song.reverb = {40, 24};          // the application's own preset
        res.notes.push_back("the song's reverb setting was not with it, so it has VocalWriter's usual one");
    }

    // a song with every track switched off (CrazyThing among the demos) is
    // not one that plays nothing: the switches mean nothing then
    auto plays_notes = [](const TrackRec &r) {
        if (r.flags & kKaraoke) return false;
        for (const Event &e : r.events)
            if (e.status == kPlayed || e.status == kNote) return true;
        return false;
    };
    bool any_on = false;
    for (const TrackRec &r : tracks)
        if (plays_notes(r)) any_on = any_on || !r.muted;

    int instruments = 0, instruments_muted = 0;
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        const TrackRec &r = tracks[ti];
        if (!(r.flags & kVocal) && plays_notes(r) && ti > 0) {
            // an instrument track, on the GM Soundbank
            Track t = instrument_track(r.events, r.flags & kDrums, beat);
            t.name = r.name.empty() ? "Instrument" : r.name;
            t.volume = std::max(0, std::min(kMaxVolume, r.level));
            t.mute = r.muted && any_on;
            ++instruments;
            instruments_muted += t.mute;
            song.tracks.push_back(t);
            continue;
        }
        if (!(r.flags & kVocal) || (r.flags & kKaraoke)) continue;
        // the notes, the voice the track starts with, and the controls
        std::vector<SungAt> sung;
        std::vector<Control> controls;
        int voice = voice_of_program(0), first_voice = -1, range = 2, rest_key = -1;
        std::set<uint32_t> seen;     // the moments a note or rest has begun at
        auto sung_at = [&](uint32_t tick) { return seen.count(tick) > 0; };
        for (const Event &e : r.events) {
            double b = beat(e.time);
            // whether a note at this moment is still to come in the file
            bool before = e.status != kNote && e.status != kRestEvent && !sung_at(e.time);
            if (e.status == kProgram) {
                voice = voice_of_program(e.vol);
                // every choice of voice where it is made, the first one too:
                // what is sung before it is the default's
                controls.push_back({b, "voice", double(voice), before});
            } else if (e.status == kNote) {
                if (first_voice < 0) first_voice = voice;
                SungAt s;
                s.start = b;
                s.end = beat(e.time + e.dur);
                s.key = e.key;
                s.velocity = std::max(1, e.vol);
                if (e.lyric >= 0 && size_t(e.lyric) < r.lyrics.size()) {
                    s.word = r.lyrics[size_t(e.lyric)].first;
                    s.phonemes = r.lyrics[size_t(e.lyric)].second;
                }
                if (s.phonemes.empty()) s.phonemes = {kDefaultPhoneme};
                s.lead = rest_key;
                rest_key = -1;
                sung.push_back(s);
            } else if (e.status == kRestEvent) {
                rest_key = e.key;
            } else if (e.status == kBendRange) {
                range = std::min(12, e.vol);
                controls.push_back({b, "bend_range", double(range), before});
            } else if (e.status == kPitchBend) {
                controls.push_back({b, "", ((e.vol << 7) + e.key - 8192) / 8192.0 * range, before});
            } else if (control_names().count(e.status)) {
                std::string name = control_names().at(e.status);
                double v = (name == "chorus" || name == "detune") ? double((e.vol << 7) + e.key - 8192)
                                                                  : double(e.vol);
                controls.push_back({b, name, v, before});
            }
            if (e.status == kNote || e.status == kRestEvent) seen.insert(e.time);
        }
        if (sung.empty()) continue;
        std::stable_sort(controls.begin(), controls.end(),
                         [](const Control &a, const Control &b) { return a.beat < b.beat; });
        Track t;
        t.name = r.name.empty() ? "Vocals" : r.name;
        t.voice_id = kVocalWriterBase + first_voice;
        t.volume = std::max(0, std::min(kMaxVolume, r.level));
        t.mute = r.muted && any_on;
        t.notes = lay_out(sung, controls);
        // the velocity most of the notes have is the track's, and a note
        // only says so when it differs
        std::map<int, int> counts;
        for (const SungAt &s : sung) ++counts[s.velocity];
        int most = sung[0].velocity;
        for (const auto &c : counts)
            if (c.second > counts[most]) most = c.first;
        t.velocity = most;
        for (Note &n : t.notes)
            if (n.velocity == most || n.is_rest()) n.velocity = 0;
        song.tracks.push_back(t);
    }
    if (song.tracks.empty()) throw std::runtime_error("this song has no tracks with notes");
    // the tempo changes on a track of their own at the top, each where it falls
    if (!changes.empty()) song.tracks.insert(song.tracks.begin(), tempo_track(changes));
    // where the song ends, which VocalWriter keeps on the tempo track
    for (const TrackRec &r : tracks)
        for (const Event &e : r.events)
            if (e.status == kEnd) song.end = beat(e.time);
    if (instruments) {
        std::string n = std::to_string(instruments) + " instrument track" + (instruments == 1 ? "" : "s");
        std::string said = n + " play" + (instruments == 1 ? "s" : "") +
                           " on the GM Soundbank, VocalWriter's own instruments";
        if (instruments_muted)
            said += "; " + std::to_string(instruments_muted) + " of them " + (instruments_muted == 1 ? "is" : "are") +
                    " muted, as the song was saved. M on a track unmutes it";
        res.notes.push_back(said);
    }
    return res;
}

}  // namespace svs
