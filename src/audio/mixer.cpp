#include "audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#endif

#include "audio/decode.h"
#include "audio/wav.h"
#include "core/paths.h"
#include "core/phonology.h"
#include "voices/registry.h"
#include "voices/vocalwriter.h"

namespace svs {

namespace {

const double kPi = 3.14159265358979323846;
//: how long after the last note the file runs on, so its decay is not clipped
const double kTailSeconds = 0.4;
//: how often a bend in motion is sent: one VocalWriter frame, about 5 ms
const double kBendStep = 0.005;
//: The most of a note that may be given to the note in front of it so that a
//: note opening with consonants can start early enough for its vowel to land
//: on the beat. Take more than half and what is left is a grace note.
const double kAnticipateMost = 0.5;

long round_half_even(double x) { return long(std::nearbyint(x)); }

struct Pt {
    double t, v;
    bool slides;
};

//: Fill in between the written points so a bend moves rather than jumps --
//: only where the point says it slides into the next one.
std::vector<std::pair<double, double>> glide(const std::vector<Pt> &pts) {
    std::vector<std::pair<double, double>> out;
    if (pts.size() < 2) {
        for (const Pt &p : pts) out.push_back({p.t, p.v});
        return out;
    }
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const Pt &a = pts[i], &b = pts[i + 1];
        out.push_back({a.t, a.v});
        if (!a.slides || b.v == a.v || b.t - a.t <= kBendStep) continue;
        int n = int((b.t - a.t) / kBendStep);
        for (int k = 1; k < n; ++k) {
            double f = k * kBendStep / (b.t - a.t);
            out.push_back({a.t + k * kBendStep, a.v + (b.v - a.v) * f});
        }
    }
    out.push_back({pts.back().t, pts.back().v});
    return out;
}

int wheel_step(double v) { return int(std::lrint(v)); }

int bend_raw(double semitones) {
    long v = round_half_even(semitones / 12.0 * 8192);
    return int(std::max(-8192L, std::min(8191L, v)));
}

//: Bend values for one phrase, in seconds from its own start; whatever was in
//: force when the phrase began is carried in ahead of it. `step` is the
//: smallest change worth sending: the engine's own bend resolution for a bend,
//: a whole step for the mod wheel.
std::vector<BendEvent> bend_events(const std::vector<Pt> &points, double t0, double t1,
                                   int (*step)(double) = bend_raw) {
    std::vector<BendEvent> ev;
    if (points.empty()) return ev;
    bool have_before = false;
    double before = 0;
    for (const Pt &p : points)
        if (p.t < t0) {
            before = p.v;
            have_before = true;
        }
    if (have_before) ev.push_back({-1.0, before});
    bool have_last = false;
    int last = 0;
    for (const auto &tv : glide(points)) {
        if (t0 <= tv.first && tv.first <= t1) {
            int raw = step(tv.second);
            if (!have_last || raw != last) {    // the same value twice is work
                ev.push_back({tv.first - t0, tv.second});
                last = raw;
                have_last = true;
            }
        }
    }
    return ev;
}

std::pair<double, double> pan_gains(double pan) {
    double theta = (std::max(-1.0, std::min(1.0, pan)) + 1.0) * (kPi / 4.0);
    return {std::sqrt(2.0) * std::cos(theta), std::sqrt(2.0) * std::sin(theta)};
}

Reverb clean_reverb(const Reverb &r) {
    return {std::max(0, std::min(100, r.first)), std::max(0, std::min(100, r.second))};
}

bool is_rest(const std::vector<std::string> &ph) {
    for (const std::string &p : ph)
        if (p != kRest) return false;
    return true;
}

//: How long a note's opening consonants last, in beats.
//: Only what comes before the phoneme the note is held on counts. A note with
//: no vowel used to count as all onset -- a hummed m was moved, whole, half a
//: beat ahead of where it was written, its bend left behind at the beat.
double onset_beats(const SungNote &note, double bpm) {
    int held = held_phoneme(note.phonemes);
    double ms = 0;
    for (int i = 0; i < held && size_t(i) < note.durations_ms.size(); ++i) ms += note.durations_ms[size_t(i)];
    return ms * bpm / 60000.0;
}

//: Move every note's consonants in front of its beat, taking the time from
//: the note before. Returns how much earlier the phrase now starts.
double anticipate(std::vector<SungNote> &notes, double bpm, double consonants, double room) {
    auto retime = [&](SungNote &n, double beats) {
        n.beats = beats;
        n.durations_ms = syllable_lengths(n.phonemes, beats * 60000.0 / bpm, consonants);
    };
    double lead = std::max(0.0, std::min(onset_beats(notes[0], bpm), room));
    if (lead > 0) retime(notes[0], notes[0].beats + lead);
    for (size_t i = 1; i < notes.size(); ++i) {
        double give = std::min(onset_beats(notes[i], bpm), kAnticipateMost * notes[i - 1].beats);
        if (give <= 0) continue;
        retime(notes[i - 1], notes[i - 1].beats - give);
        retime(notes[i], notes[i].beats + give);
    }
    return lead;
}

std::string hash_key(const std::string &s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%016llx%08zx", (unsigned long long)h, s.size());
    return buf;
}

std::string request_key(const RenderRequest &r) {
    std::string k;
    char buf[64];
    auto num = [&](double v) {
        std::snprintf(buf, sizeof buf, "%.17g,", v);
        k += buf;
    };
    num(r.bpm);
    num(r.consonants);
    num(r.start);
    num(r.reverb.first);
    num(r.reverb.second);
    num(r.anticipate);
    for (const PartRequest &t : r.tracks) {
        k += "|T";
        num(t.voice_id);
        num(t.volume);
        num(t.pan);
        num(t.velocity);
        for (int VoiceStyle::*f : {&VoiceStyle::color, &VoiceStyle::vibrato, &VoiceStyle::vibrato_rate,
                                   &VoiceStyle::chorus, &VoiceStyle::breath, &VoiceStyle::detune,
                                   &VoiceStyle::portamento})
            num(t.voice.*f);
        if (t.reverb) {
            num(t.reverb->first);
            num(t.reverb->second);
        } else {
            k += "-,";
        }
        if (t.consonants) num(*t.consonants);
        else k += "-,";
        for (const auto &n : t.notes) {
            k += "N";
            num(n.pitch);
            num(n.beats);
            for (const std::string &p : n.phonemes) k += p + " ";
        }
        for (const BendPoint &b : t.bends) {
            k += "B";
            num(b.beat);
            num(b.semitones);
            num(b.slides);
        }
        for (const BendPoint &b : t.mods) {
            k += "M";
            num(b.beat);
            num(b.semitones);
            num(b.slides);
        }
    }
    // a backing track's file by what it is now: changed on disk, it is
    // read again
    for (const BackingRequest &b : r.backings) {
        k += "|A" + b.path + "|";
        num(double(file_size(b.path)));
        num(double(file_time(b.path)));
        num(b.volume);
        num(b.pan);
        num(b.start);
    }
    return hash_key(k);
}

std::vector<float> click(double hz, double level) {
    int n = int(kSampleRate * 0.035);
    std::vector<float> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        double t = i / double(kSampleRate);
        out[static_cast<size_t>(i)] = float(level * std::sin(2 * kPi * hz * t) * std::exp(-t * 45.0));
    }
    return out;
}

}  // namespace

void with_metronome(std::vector<float> &y, int channels, double bpm, double bar_beats,
                    double start) {
    const double kClickHz = 1000.0, kAccentHz = 1600.0, kLevel = 0.22;
    double spb = 60.0 / std::max(bpm, 1e-6);
    std::vector<float> plain = click(kClickHz, kLevel), accent = click(kAccentHz, kLevel * 1.4);
    size_t n = y.size() / size_t(channels);
    std::vector<float> ticks(n, 0.0f);
    long beats = std::max(1L, round_half_even(bar_beats));   // keeps 3/4, 6/8 and 7/8
    long k = long(std::ceil(start - 1e-9));
    while (true) {
        long at = round_half_even((k - start) * spb * kSampleRate);
        if (at >= long(n)) break;
        const std::vector<float> &tick = (k % beats == 0) ? accent : plain;
        for (size_t i = 0; i < tick.size(); ++i) {
            long j = at + long(i);
            if (j >= 0 && j < long(n)) ticks[size_t(j)] += tick[i];
        }
        ++k;
    }
    float peak = 0;
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < channels; ++c) {
            float &s = y[i * size_t(channels) + size_t(c)];
            s += ticks[i];
            peak = std::max(peak, std::fabs(s));
        }
    if (peak > 1.0f)                       // the ticks must not push it into clip
        for (float &s : y) s /= peak;
}

RenderRequest make_request(const Song &song, const std::vector<const Track *> &parts, double start,
                           const std::map<int, int> &program_map) {
    RenderRequest r;
    r.bpm = song.bpm;
    r.consonants = song.consonants;
    r.start = std::nearbyint(start * 1e6) / 1e6;
    r.reverb = song.reverb;
    r.anticipate = song.anticipate;
    for (const Track *t : parts) {
        if (t->is_backing()) {
            r.backings.push_back({t->audio, t->volume / 100.0, t->pan / 100.0,
                                  std::nearbyint(t->audio_start * 1e3) / 1e3, t->name});
            continue;
        }
        PartRequest p;
        p.voice_id = track_voice(*t, program_map);
        p.volume = t->volume / 100.0;
        p.pan = t->pan / 100.0;
        // a part with no voice controls of its own is sung with the song's
        p.voice = t->voice ? *t->voice : song.voice;
        p.reverb = t->reverb;
        p.consonants = t->consonants;
        for (const Note &n : t->notes)
            p.notes.push_back({n.pitch, n.beats,
                               n.phonemes.empty() ? std::vector<std::string>{kRest} : n.phonemes});
        for (BendPoint b : timeline(t->notes)) {
            b.beat = std::nearbyint(b.beat * 1e5) / 1e5;
            b.semitones = std::nearbyint(b.semitones * 1e4) / 1e4;
            p.bends.push_back(b);
        }
        for (BendPoint b : mod_timeline(t->notes)) {
            b.beat = std::nearbyint(b.beat * 1e5) / 1e5;
            b.semitones = std::nearbyint(b.semitones * 1e4) / 1e4;
            p.mods.push_back(b);
        }
        r.tracks.push_back(p);
    }
    return r;
}

namespace {

//: The most the phrase store keeps. A phrase is mono floats, 176 KB a second,
//: so this is three quarters of an hour of distinct singing; past it the
//: phrases sung longest ago go first.
const size_t kPhraseBytes = 512u * 1024 * 1024;

//: Everything that decides how a phrase sounds, as text: the key it is kept
//: under. Doubles are written in full, so two phrases that differ by a hair
//: are two phrases.
std::string phrase_key(int voice_id, const Phrase &p) {
    std::string k;
    char buf[64];
    auto num = [&](double v) {
        std::snprintf(buf, sizeof buf, "%.17g,", v);
        k += buf;
    };
    num(voice_id);
    num(p.bpm);
    num(p.level);
    num(p.any_bends);
    for (int VoiceStyle::*f : {&VoiceStyle::color, &VoiceStyle::vibrato, &VoiceStyle::vibrato_rate,
                               &VoiceStyle::chorus, &VoiceStyle::breath, &VoiceStyle::detune,
                               &VoiceStyle::portamento})
        num(p.style.*f);
    for (const SungNote &n : p.notes) {
        k += "N";
        num(n.midi);
        num(n.beats);
        num(n.velocity);
        for (const std::string &ph : n.phonemes) k += ph + " ";
        for (double d : n.durations_ms) num(d);
    }
    for (const BendEvent &b : p.bends) {
        k += "B";
        num(b.seconds);
        num(b.semitones);
    }
    for (const BendEvent &m : p.mods) {
        k += "M";
        num(m.seconds);
        num(m.semitones);
    }
    return k;
}

//: The most backing tracks' files kept read at once. A file is stereo floats,
//: 21 MB a minute, and a song seldom has more than one or two.
const size_t kFilesKept = 4;

}  // namespace

// The renders used to be WAV files in a cache folder, which nothing emptied;
// they are kept in memory now, so whatever an older copy of the program left
// in the temporary folder is cleared away the first time this one starts.
Mixer::Mixer() { remove_tree(join_path(temp_dir(), "singing-voice-studio-cache")); }

std::shared_ptr<const std::vector<float>> Mixer::backing(const std::string &path, std::string *error) {
    long long size = file_size(path), time = file_time(path);
    auto kept = files_.find(path);
    if (kept != files_.end() && kept->second.size == size && kept->second.time == time) {
        kept->second.used = ++clock_;
        return kept->second.samples;
    }
    auto y = std::make_shared<std::vector<float>>();
    if (!decode_audio(path, y.get(), error)) return nullptr;
    files_[path] = {size, time, y, ++clock_};
    while (files_.size() > kFilesKept) {
        auto oldest = files_.begin();
        for (auto it = files_.begin(); it != files_.end(); ++it)
            if (it->second.used < oldest->second.used) oldest = it;
        files_.erase(oldest);
    }
    return y;
}

std::vector<float> Mixer::track(const PartRequest &t, double bpm, double consonants, double start,
                                bool early, bool *stopped_short, std::string *error) {
    double spb = 60.0 / std::max(bpm, 1e-6);
    // The voice's own level, so that a part that would clip is turned down
    // before an engine clamps it rather than after.
    double level = t.volume * Registry::get().headroom(t.voice_id);
    if (t.consonants) consonants = *t.consonants;
    std::vector<Pt> bends;
    for (const BendPoint &b : t.bends) bends.push_back({b.beat, b.semitones, b.slides});
    // by time alone: two points may share a moment, which is how a step is
    // written, and sorting on the value would turn it into a slide
    std::stable_sort(bends.begin(), bends.end(), [](const Pt &a, const Pt &b) { return a.t < b.t; });
    std::vector<Pt> mods;
    for (const BendPoint &b : t.mods) mods.push_back({b.beat, b.semitones, b.slides});
    std::stable_sort(mods.begin(), mods.end(), [](const Pt &a, const Pt &b) { return a.t < b.t; });

    // the phrases between the rests, and the song's total length
    struct Run {
        double at;
        std::vector<const PartRequest::N *> notes;
    };
    std::vector<Run> runs;
    double at = 0.0;
    bool open = false;
    for (const auto &e : t.notes) {
        if (is_rest(e.phonemes)) {
            open = false;
        } else {
            if (!open) {
                runs.push_back({at, {}});
                open = true;
            }
            runs.back().notes.push_back(&e);
        }
        at += e.beats;
    }
    double total = at;
    double length = std::max(0.0, total - start) * spb + kTailSeconds;
    std::vector<float> out(size_t(round_half_even(length * kSampleRate)), 0.0f);

    // made the first time a phrase is not already in the store, so a song
    // whose phrases are all there sings nothing and starts no engine
    std::unique_ptr<Singer> singer;
    double was_over = 0.0;
    for (Run &run : runs) {
        Phrase phrase;
        phrase.bpm = bpm;
        phrase.style = t.voice;
        phrase.level = level;
        phrase.any_bends = !bends.empty();
        for (const auto *e : run.notes) {
            SungNote n;
            n.midi = e->pitch;
            n.beats = e->beats;
            n.velocity = t.velocity;
            n.phonemes = e->phonemes.empty() ? std::vector<std::string>{kRest} : e->phonemes;
            n.durations_ms = syllable_lengths(n.phonemes, n.beats * 60000.0 / bpm, consonants);
            phrase.notes.push_back(n);
        }
        double lead = early ? anticipate(phrase.notes, bpm, consonants, run.at - was_over) : 0.0;
        double sum = 0;
        for (const SungNote &n : phrase.notes) sum += n.beats;
        was_over = run.at + sum - lead;
        // a marked rest to scale the last syllable against, and to let it
        // decay rather than being cut off
        SungNote tail;
        tail.midi = phrase.notes.back().midi;
        tail.beats = 0.4;
        tail.velocity = 1;
        tail.phonemes = {kRest};
        phrase.notes.push_back(tail);
        double from = run.at - lead;
        double span = sum + 0.4;
        if (from + span <= start) continue;           // over before the cursor
        std::vector<Pt> secs;
        for (const Pt &p : bends) secs.push_back({p.t * spb, p.v, p.slides});
        phrase.bends = bend_events(secs, from * spb, (from + span) * spb);
        std::vector<Pt> mod_secs;
        for (const Pt &p : mods) mod_secs.push_back({p.t * spb, p.v, p.slides});
        phrase.mods = bend_events(mod_secs, from * spb, (from + span) * spb, wheel_step);
        std::string key = phrase_key(t.voice_id, phrase);
        std::shared_ptr<const std::vector<float>> sung;
        auto kept = phrases_.find(key);
        if (kept != phrases_.end()) {
            kept->second.used = ++clock_;
            sung = kept->second.samples;
            if (kept->second.stopped_short) *stopped_short = true;
        } else {
            if (!singer) {
                std::string err;
                singer = Registry::get().singer(t.voice_id, &err);
                if (!singer) {
                    *error = "cannot sing " + Registry::get().voice_name(t.voice_id) + ": " + err;
                    return out;
                }
            }
            Sung y = singer->sing(phrase);
            if (!y.error.empty()) {
                // never kept: the next play asks the engine again
                *error = Registry::get().voice_name(t.voice_id) + ": " + y.error;
                return out;
            }
            if (y.stopped_short) *stopped_short = true;
            sung = std::make_shared<const std::vector<float>>(std::move(y.samples));
            phrases_[key] = {sung, y.stopped_short, ++clock_};
            phrase_bytes_ += sung->size() * sizeof(float);
            while (phrase_bytes_ > kPhraseBytes && phrases_.size() > 1) {
                auto oldest = phrases_.end();
                for (auto it = phrases_.begin(); it != phrases_.end(); ++it)
                    if (it->first != key && (oldest == phrases_.end() || it->second.used < oldest->second.used))
                        oldest = it;
                if (oldest == phrases_.end()) break;
                phrase_bytes_ -= oldest->second.samples->size() * sizeof(float);
                phrases_.erase(oldest);
            }
        }
        const std::vector<float> &ys = *sung;
        long i = round_half_even((from - start) * spb * kSampleRate);
        size_t skip = 0;
        if (i < 0) {                                  // began before the cursor
            skip = size_t(-i);
            i = 0;
        }
        if (skip >= ys.size()) continue;
        size_t len = ys.size() - skip;
        if (size_t(i) + len > out.size()) out.resize(size_t(i) + len, 0.0f);
        for (size_t k = 0; k < len; ++k) out[size_t(i) + k] += ys[skip + k];
    }
    return out;
}

Mixed Mixer::mix(const RenderRequest &req) {
    std::lock_guard<std::mutex> lock(mu_);
    Mixed res = mix_locked(req);
    if (res.ok && req.metronome_bar) {
        // on a copy: the song kept for next time is only ever the singing
        auto ticked = std::make_shared<std::vector<float>>(*res.samples);
        with_metronome(*ticked, res.channels, req.bpm, *req.metronome_bar, req.start);
        res.samples = ticked;
    }
    return res;
}

RenderResult Mixer::render(const RenderRequest &req, const std::string &out_path) {
    Mixed m;
    {
        std::lock_guard<std::mutex> lock(mu_);
        m = mix_locked(req);
    }
    RenderResult res;
    res.ok = m.ok;
    res.error = m.error;
    res.seconds = m.seconds;
    res.peak = m.peak;
    res.cached = m.cached;
    res.stopped_short = m.stopped_short;
    res.path = out_path;
    if (!m.ok) return res;
    std::vector<float> y = *m.samples;
    if (req.metronome_bar) with_metronome(y, m.channels, req.bpm, *req.metronome_bar, req.start);
    if (!write_wav(out_path, y, m.channels)) {
        res.ok = false;
        res.error = "could not write " + out_path;
    }
    return res;
}

Mixed Mixer::mix_locked(const RenderRequest &req) {
    Mixed res;
    // the metronome is put on afterwards and is no part of the key, so
    // switching it on and off never sings anything again
    RenderRequest core = req;
    core.metronome_bar.reset();
    std::string key = request_key(core);
    if (last_.ok && key == last_key_) {
        Mixed again = last_;
        again.cached = true;
        return again;
    }

    bool any = !req.backings.empty();
    for (const PartRequest &t : req.tracks) any = any || !t.notes.empty();
    if (!any) {
        res.error = "nothing to sing";
        return res;
    }
    bool stopped = false;
    // grouped by the reverb each part ends up with, in the order first met
    std::vector<std::pair<Reverb, std::vector<size_t>>> groups;
    std::vector<std::vector<float>> ys(req.tracks.size());
    Reverb song_reverb = clean_reverb(req.reverb);
    for (size_t k = 0; k < req.tracks.size(); ++k) {
        const PartRequest &t = req.tracks[k];
        std::string err;
        ys[k] = track(t, req.bpm, req.consonants, req.start, req.anticipate, &stopped, &err);
        if (!err.empty()) {
            res.error = err;
            return res;
        }
        Reverb rev = t.reverb ? clean_reverb(*t.reverb) : song_reverb;
        auto g = std::find_if(groups.begin(), groups.end(),
                              [&](const std::pair<Reverb, std::vector<size_t>> &x) { return x.first == rev; });
        if (g == groups.end()) groups.push_back({rev, {k}});
        else g->second.push_back(k);
    }
    size_t n = 0;
    for (const auto &y : ys) n = std::max(n, y.size());
    bool stereo = false;
    for (const PartRequest &t : req.tracks) stereo = stereo || std::fabs(t.pan) > 1e-6;
    for (const auto &g : groups) stereo = stereo || g.first.second > 0;

    std::vector<float> out;
    int channels = 1;
    double peak = 0;
    if (stereo) {
        channels = 2;
        struct Mix {
            Reverb rev;
            std::vector<float> mix, room;
        };
        std::vector<Mix> mixes;
        for (const auto &g : groups) {
            Mix m{g.first, std::vector<float>(n * 2, 0.0f), std::vector<float>(n * 2, 0.0f)};
            for (size_t k : g.second) {
                auto [gl, gr] = pan_gains(req.tracks[k].pan);
                const auto &y = ys[k];
                for (size_t i = 0; i < y.size(); ++i) {
                    m.mix[2 * i] += float(y[i] * gl);
                    m.mix[2 * i + 1] += float(y[i] * gr);
                    // the same voices with the panning left off, which is
                    // what the reverberator is given: panning moves the
                    // voices, not the room they are singing in
                    m.room[2 * i] += y[i];
                    m.room[2 * i + 1] += y[i];
                }
            }
            mixes.push_back(std::move(m));
        }
        for (size_t i = 0; i < n * 2; ++i) {
            double s = 0;
            for (const Mix &m : mixes) s += m.mix[i];
            peak = std::max(peak, std::fabs(s));
        }
        // several voices at once can add up past full scale; turning the mix
        // down is better than clipping it, and has to happen before the
        // reverb, which works on 16-bit samples
        if (peak > 1.0)
            for (Mix &m : mixes) {
                for (float &v : m.mix) v = float(v / peak);
                for (float &v : m.room) v = float(v / peak);
            }
        std::vector<std::vector<float>> done;
        for (Mix &m : mixes) {
            std::vector<float> wet = m.room;
            bool reverbed = m.rev.second > 0 && vocalwriter_reverb(wet, m.rev.first, m.rev.second);
            if (!reverbed) wet = m.room;
            // the panning added back, at the gain the dry signal is mixed in with
            double dry = reverbed ? 1.0 - m.rev.second / 100.0 : 1.0;
            for (size_t i = 0; i < n * 2 && i < wet.size(); ++i)
                wet[i] += float((m.mix[i] - m.room[i]) * dry);
            done.push_back(std::move(wet));
        }
        size_t longest = 0;
        for (const auto &d : done) longest = std::max(longest, d.size());
        out.assign(longest, 0.0f);
        for (const auto &d : done)
            for (size_t i = 0; i < d.size(); ++i) out[i] += d[i];
    } else {
        out.assign(n, 0.0f);
        for (size_t k = 0; k < req.tracks.size(); ++k) {
            double gl = pan_gains(req.tracks[k].pan).first;
            for (size_t i = 0; i < ys[k].size(); ++i) out[i] += float(ys[k][i] * gl);
        }
        for (float v : out) peak = std::max(peak, double(std::fabs(v)));
        if (peak > 1.0)
            for (float &v : out) v = float(v / peak);
    }

    // The backing tracks, under the voices and after their reverb. The file
    // is placed by the clock, not the beat: it is the same recording at any
    // tempo, and the cursor's beat is only where in it playing begins.
    if (!req.backings.empty()) {
        if (channels == 1) {
            std::vector<float> two(out.size() * 2);
            for (size_t i = 0; i < out.size(); ++i) two[2 * i] = two[2 * i + 1] = out[i];
            out.swap(two);
            channels = 2;
        }
        double from = req.start * 60.0 / std::max(req.bpm, 1e-6);
        for (const BackingRequest &b : req.backings) {
            std::string err;
            std::shared_ptr<const std::vector<float>> file = backing(b.path, &err);
            if (!file) {
                res.error = "the backing track " + b.name + " cannot be played: " + err;
                return res;
            }
            const std::vector<float> &x = *file;
            size_t frames = x.size() / 2;
            long at = round_half_even((b.start - from) * kSampleRate);
            size_t skip = at < 0 ? size_t(-at) : 0, to = at < 0 ? 0 : size_t(at);
            if (skip >= frames) continue;               // over before the cursor
            size_t need = (to + frames - skip) * 2;
            if (out.size() < need) out.resize(need, 0.0f);
            // a balance rather than a pan: the file is stereo already, and
            // turning it to one side turns the other side down
            double gl = b.volume * std::min(1.0, 1.0 - b.pan), gr = b.volume * std::min(1.0, 1.0 + b.pan);
            for (size_t k = skip; k < frames; ++k) {
                out[2 * (to + k - skip)] += float(x[2 * k] * gl);
                out[2 * (to + k - skip) + 1] += float(x[2 * k + 1] * gr);
            }
        }
        double all = 0;
        for (float v : out) all = std::max(all, double(std::fabs(v)));
        if (all > 1.0)
            for (float &v : out) v = float(v / all);
        peak = std::max(peak, all);
    }

    res.ok = true;
    res.channels = channels;
    res.seconds = double(out.size() / size_t(channels)) / kSampleRate;
    res.peak = peak;
    res.stopped_short = stopped;
    res.samples = std::make_shared<const std::vector<float>>(std::move(out));
    last_key_ = key;
    last_ = res;
    return res;
}

}  // namespace svs
