// VocalWriter's engine, driven the way VocalWriter 2.0 drives it: each part
// is sung as a track of a VocalWriter song, through the application's own
// parser and sequencer (vw_sequencer.h), so the samples are the same samples.
#include "voices/vocalwriter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>

#include "core/paths.h"
#include "core/phonology.h"
#include "voices/controls.h"

extern "C" {
#include "vw_editor.h"
}
#include "voices/vw_sequencer.h"

namespace svs {

namespace {

const char *const kRsrc = "assets\\VocalWriter.app\\Contents\\Resources\\VocalWriter.rsrc";
const char *const kGmSpeech = "assets\\GMSpeech.rsrc";
const char *const kGmBank = "assets\\GMBank.rsrc";
const char *const kEnglishLex = "assets\\EnglishLex";

const int kTicksPerBeat = 240;
//: the portamento VocalWriter's engine starts every voice with, its glide
//: table's twentieth step: a song that wants another, none among them, says so
const int kEnginePortamento = 40;

//: Python's round(): halves go to the even neighbour.
long round_half_even(double x) { return long(std::nearbyint(x)); }

//: A bend as MIDI's 14-bit bend about zero, in a range of `range` semitones.
int bend_raw(double semitones, int range) {
    long v = round_half_even(semitones / range * 8192);
    return int(std::max(-8192L, std::min(8191L, v)));
}

}  // namespace

// -- the data ---------------------------------------------------------------

VwAssets &VwAssets::get() {
    static VwAssets instance;
    return instance;
}

VwAssets::VwAssets() {
    const char *required[] = {kRsrc, kGmSpeech, kEnglishLex};
    for (const char *r : required)
        if (!data_exists(r)) missing_.push_back(r);
    root_ = data_roots().empty() ? "." : data_roots()[0];
    if (!missing_.empty()) return;
    std::string rsrc_path = find_data(kRsrc);
    root_ = rsrc_path.substr(0, rsrc_path.size() - std::strlen(kRsrc));
    ok_ = read_file(rsrc_path, &rsrc) && read_file(find_data(kGmSpeech), &gmspeech) &&
          read_file(find_data(kEnglishLex), &lexicon);
    // The instrument bank is not needed to sing with the natural voices; a
    // copy without it still works, with fewer voices to choose from.
    if (data_exists(kGmBank)) read_file(find_data(kGmBank), &bank);
}

vw_editor *VwAssets::open_editor() const {
    if (!ok_) return nullptr;
    vw_editor *e = vw_ed_open(rsrc.data(), rsrc.size(), gmspeech.data(), gmspeech.size());
    if (!e) return nullptr;
    if (!bank.empty() && vw_ed_bank(e, bank.data(), bank.size()) != 0) {
        // a damaged bank leaves the natural voices working
    }
    return e;
}

// -- the dictionary -----------------------------------------------------------

Lexicon &Lexicon::get() {
    static Lexicon instance;
    return instance;
}

Lexicon::Lexicon() {
    VwAssets &a = VwAssets::get();
    if (!a.ok()) return;
    ed_ = a.open_editor();
    // the engine reads the dictionary where it lies, so it must stay put
    if (ed_ && vw_ed_lexicon(ed_, a.lexicon.data(), a.lexicon.size()) != 0) {
        vw_ed_close(ed_);
        ed_ = nullptr;
    }
}

std::vector<std::string> Lexicon::phonemes(const std::string &word, bool marked) {
    std::string clean;
    for (unsigned char c : word)
        if (std::isalpha(c) || c == '\'') clean.push_back(char(c));
    if (clean.empty() || !ed_) return {};
    std::lock_guard<std::mutex> lock(mu_);
    unsigned char out[10 * 9] = {0};
    int n = vw_ed_word(ed_, clean.c_str(), out);
    if (n <= 0) return {};
    const auto &table = svs::phonemes();
    std::vector<std::string> phones;
    std::vector<int> syllable;
    for (int i = 0; i < n && i < 10; ++i) {
        int count = out[i * 9];
        for (int k = 0; k < count && k < 8; ++k) {
            int code = out[i * 9 + 1 + k];
            if (code < int(table.size())) {
                phones.push_back(table[size_t(code)].symbol);
                syllable.push_back(i);
            }
        }
    }
    if (phones.empty()) return {};
    std::vector<std::string> sounds = allophones(phones);
    if (!marked) return sounds;
    // An allophone stands for one phoneme or for two run together ("AX r"
    // is ER); it goes in the syllable of the first.
    std::vector<std::string> out_marked;
    size_t i = 0;
    for (const std::string &s : sounds) {
        if (i >= phones.size()) break;
        if (!out_marked.empty() && syllable[i] != syllable[i - 1] && out_marked.back() != kSyllableBreak)
            out_marked.push_back(kSyllableBreak);
        out_marked.push_back(s);
        size_t took = s != phones[i] && s != "LX" ? 2 : 1;
        // a pair run over a break leaves the break after it
        if (took == 2 && i + 1 < phones.size() && syllable[i + 1] != syllable[i] &&
            i + 2 < phones.size())
            out_marked.push_back(kSyllableBreak);
        i += took;
    }
    return out_marked;
}

// -- singing -------------------------------------------------------------------
//
// A part is sung through VocalWriter's own play path (vw_sequencer.h): its
// parser lays the consonants before the beats and fits the lengths to the
// speech rate, and its sequencer sings the track as VocalWriter sang it.

namespace {

//: The sequencer's engine: opened once, as the application opened it at
//: launch, and used by one part at a time.
struct SeqEngine {
    std::mutex mu;
    vwseq_engine *e = nullptr;
    bool tried = false;
    std::string error;
};

SeqEngine &seq_engine() {
    static SeqEngine instance;
    return instance;
}

//: Called with the engine's lock held.
bool open_seq_engine(SeqEngine &s) {
    if (s.tried) return s.e != nullptr;
    s.tried = true;
    std::string rsrc = find_data(kRsrc), gm = find_data(kGmSpeech);
    std::string bank = data_exists(kGmBank) ? find_data(kGmBank) : std::string();
    int rc = 0;
    s.e = vwseq_open(rsrc.c_str(), gm.c_str(), bank.empty() ? nullptr : bank.c_str(), &rc);
    // a damaged bank leaves the natural voices working
    if (!s.e && !bank.empty()) s.e = vwseq_open(rsrc.c_str(), gm.c_str(), nullptr, &rc);
    if (!s.e) s.error = "VocalWriter's sequencer would not start (" + std::to_string(rc) + ")";
    return s.e != nullptr;
}

//: VocalWriter's lyric text: only its last character matters to the
//: parser, a "-" joining the syllable to the next one as a word.
std::string lyric_text(const std::string &word) {
    std::string out;
    for (unsigned char c : word)
        if (std::isalpha(c) || c == '-' || c == '=' || c == '\'') out.push_back(char(c));
    if (out.size() > 12) out = out.substr(0, 11) + out.back();
    return out;
}

bool is_rest_note(const SungNote &n) {
    for (const std::string &p : n.phonemes)
        if (p != kRest && phoneme_code(p) >= 0) return false;
    return true;
}

void control(std::vector<vwseq_event> &ev, int32_t t, int status, int value) {
    ev.push_back({t, status, 0, std::max(0, std::min(127, value))});
}

//: a 14-bit control about 8192: pitch bend, detune and chorus
void control14(std::vector<vwseq_event> &ev, int32_t t, int status, int value) {
    int v = std::max(0, std::min(16383, value + 8192));
    ev.push_back({t, status, v & 127, v >> 7});
}

//: Over 32767, as the WAV writer multiplies back, so an export of a
//: part holds the very samples VocalWriter wrote.
int collect(void *user, const int16_t *samples, size_t count) {
    auto *out = static_cast<std::vector<float> *>(user);
    for (size_t i = 0; i < count; ++i) out->push_back(samples[i] / 32767.0f);
    return 0;
}

//: A part as the sequencer takes it, with the notes and events it points at.
struct SeqPart {
    std::vector<vwseq_note> notes;
    std::vector<vwseq_event> events;
    std::vector<vwseq_tempo> tempos;
    vwseq_part p{};
    //: what of the level is finer than the track level's whole percent
    double residual = 1.0;
};

//: False if the part sings nothing.
bool to_seq(const Part &part, int voice, SeqPart *sp) {
    if (part.notes.empty()) return false;
    // The sequencer's tempo is a whole number; a tempo between two is
    // kept by counting ticks at the whole one. Tempo changes are whole, and
    // go in as the song's own.
    auto whole = [](double bpm) { return int(std::max(10L, std::min(250L, std::lround(bpm)))); };
    int tempo = whole(part.bpm);
    double tick_scale = part.tempos.empty() ? kTicksPerBeat * tempo / std::max(part.bpm, 1e-6) : kTicksPerBeat;
    auto ticks = [&](double beats) { return int32_t(std::llround(beats * tick_scale)); };
    struct Span {
        double tick, second;
        int bpm;
    };
    std::vector<Span> spans = {{0.0, 0.0, tempo}};
    for (const auto &c : part.tempos) {
        const Span &last = spans.back();
        double tick = c.first * kTicksPerBeat;
        if (tick <= last.tick) continue;
        spans.push_back({tick, last.second + (tick - last.tick) * 60.0 / (kTicksPerBeat * last.bpm), whole(c.second)});
        sp->tempos.push_back({int32_t(std::llround(tick)), spans.back().bpm});
    }
    auto span_at = [&](auto key, double v) {
        size_t i = spans.size() - 1;
        while (i > 0 && spans[i].*key > v) --i;
        return spans[i];
    };
    auto at_second = [&](double s) {
        Span sp_ = span_at(&Span::second, std::max(0.0, s));
        return int32_t(std::llround(sp_.tick + (std::max(0.0, s) - sp_.second) * sp_.bpm * kTicksPerBeat / 60.0));
    };
    auto second_of = [&](double tick) {
        Span sp_ = span_at(&Span::tick, tick);
        return sp_.second + (tick - sp_.tick) * 60.0 / (kTicksPerBeat * sp_.bpm);
    };

    // placed by where each note begins, so that nothing drifts
    std::vector<vwseq_note> &notes = sp->notes;
    double beat = 0.0;
    int32_t end = 0;
    for (size_t i = 0; i < part.notes.size(); ++i) {
        const SungNote &n = part.notes[i];
        int32_t a = ticks(beat), b = ticks(beat + n.beats);
        beat += n.beats;
        if (b <= a) continue;
        vwseq_note v{};
        v.start = a;
        v.duration = b - a;
        v.key = n.midi;
        v.velocity = n.velocity;
        if (is_rest_note(n)) {
            // a rest carries the coming note's key, as VocalWriter
            // writes it: that note's first consonants are sung in it
            v.rest = 1;
            for (size_t k = i + 1; k < part.notes.size(); ++k)
                if (!is_rest_note(part.notes[k])) {
                    v.key = part.notes[k].midi;
                    break;
                }
            if (!notes.empty() && notes.back().rest && notes.back().start + notes.back().duration == a) {
                notes.back().duration += v.duration;
                end = b;
                continue;
            }
        } else {
            std::string text = lyric_text(n.word);
            std::memcpy(v.text, text.c_str(), text.size() + 1);
            int count = 0;
            for (const std::string &p : n.phonemes) {
                int code = phoneme_code(p);
                if (code < 0 || p == kRest || count == 12) continue;
                v.phonemes[1 + count++] = (unsigned char)code;
            }
            v.phonemes[0] = (unsigned char)count;
        }
        notes.push_back(v);
        end = b;
    }
    if (notes.empty()) return false;
    // the sequencer finishes a track once its voice has sung a rest
    if (!notes.back().rest) {
        vwseq_note r{};
        r.start = end;
        r.duration = kTicksPerBeat;
        r.key = notes.back().key;
        r.velocity = notes.back().velocity;
        r.rest = 1;
        notes.push_back(r);
    }

    // The part's settings at the start, only those off their defaults,
    // so the voice starts as a VocalWriter song starts it; then the
    // bends and the wheel as they move.
    std::vector<vwseq_event> &ev = sp->events;
    size_t count;
    const VoiceControl *c = svs::voice_controls(&count);
    VoiceStyle style = clean_voice(part.style);
    for (size_t i = 0; i < count; ++i) {
        int value = style.*(c[i].field);
        std::string call = c[i].call;
        if (value == (call == "Speech_Portamento" ? kEnginePortamento : c[i].def)) continue;
        if (call == "Speech_Color") control(ev, 0, VWSEQ_COLOR, value);
        else if (call == "Speech_VibDepth") control(ev, 0, VWSEQ_VIB_DEPTH, value);
        else if (call == "Speech_VibFreq") control(ev, 0, VWSEQ_VIB_FREQ, value);
        else if (call == "Speech_Chorus") control14(ev, 0, VWSEQ_CHORUS, value);
        else if (call == "Speech_Breath") control(ev, 0, VWSEQ_BREATH, value);
        else if (call == "Speech_Detune") control14(ev, 0, VWSEQ_DETUNE, value);
        else if (call == "Speech_Portamento") control(ev, 0, VWSEQ_PORTAMENTO, value);
    }
    if (part.any_bends) {
        // VocalWriter's own range of two semitones if the bends fit it,
        // else the narrowest that holds them
        double widest = 0;
        for (const BendEvent &b : part.bends) widest = std::max(widest, std::fabs(b.semitones));
        int range = widest <= 2.0 + 1e-9 ? 2 : int(std::min(12.0, std::ceil(widest - 1e-9)));
        if (range != 2) control(ev, 0, VWSEQ_PB_RANGE, range);
        for (const BendEvent &b : part.bends)
            control14(ev, at_second(b.seconds), VWSEQ_PITCH_BEND, bend_raw(b.semitones, range));
    }
    if (!part.mods.empty()) {
        int last = -1;
        for (const BendEvent &m : part.mods) {
            int v = int(std::lrint(effective_vibrato(style.vibrato, m.semitones)));
            if (v != last) control(ev, at_second(m.seconds), VWSEQ_VIB_DEPTH, v);
            last = v;
        }
    }
    std::stable_sort(ev.begin(), ev.end(),
                     [](const vwseq_event &a, const vwseq_event &b) { return a.time < b.time; });

    vwseq_part &p = sp->p;
    p = vwseq_part{};
    p.bpm = tempo;
    p.voice = voice;
    p.notes = notes.data();
    p.note_count = int(notes.size());
    p.events = ev.data();
    p.event_count = int(ev.size());
    p.channel = part.channel;
    p.consonants = part.consonants;
    p.tempos = sp->tempos.data();
    p.tempo_count = int(sp->tempos.size());
    p.max_seconds = second_of(end) + 10.0;

    // the level as VocalWriter's track level, in whole percent, and what is
    // finer than that on the samples; a voice VocalWriter sang past full
    // scale is left as it sang it
    p.track_level = 100;
    sp->residual = part.level;
    if (part.level > 0 && part.level <= 1.0) {
        p.track_level = std::max(1, int(std::lround(part.level * 100)));
        sp->residual = part.level / (p.track_level / 100.0);
    }
    return true;
}

class VwSinger : public Singer {
public:
    VwSinger(int voice_id) : voice_id_(voice_id) {}

    //: A phrase is a part that begins `room` beats before it, so that its
    //: first consonants are sung before the beat as in the whole track.
    Sung sing(const Phrase &phrase) override {
        Part part;
        part.bpm = phrase.bpm;
        part.bends = phrase.bends;
        part.any_bends = phrase.any_bends;
        part.mods = phrase.mods;
        part.style = phrase.style;
        part.level = phrase.level;
        part.consonants = phrase.consonants;
        part.channel = phrase.channel;
        if (phrase.room > 0) {
            SungNote lead;
            lead.midi = phrase.notes.front().midi;
            lead.beats = phrase.room;
            lead.phonemes = {kRest};
            part.notes.push_back(lead);
        }
        part.notes.insert(part.notes.end(), phrase.notes.begin(), phrase.notes.end());
        part.tempos = phrase.tempos;
        Sung out = sing_part(part);
        out.starts_before = seconds_into(phrase.bpm, phrase.tempos, phrase.room);
        return out;
    }

    Sung sing_part(const Part &part) override {
        Sung out;
        SeqPart sp;
        if (!to_seq(part, voice_id_, &sp)) return out;
        SeqEngine &s = seq_engine();
        std::lock_guard<std::mutex> lock(s.mu);
        if (!open_seq_engine(s)) {
            out.error = s.error;
            return out;
        }
        int rc = vwseq_sing(s.e, &sp.p, collect, &out.samples);
        if (rc != 0) {
            out.error = "VocalWriter's sequencer stopped (" + std::to_string(rc) + ")";
            out.samples.clear();
            return out;
        }
        if (sp.residual != 1.0)
            for (float &v : out.samples) v = float(v * sp.residual);
        return out;
    }

private:
    int voice_id_;
};

class VocalWriterFamily : public Family {
public:
    std::string name() const override { return "VocalWriter"; }

    bool sings_parts() const override { return true; }

    std::vector<VoiceEntry> voices() override {
        std::lock_guard<std::mutex> lock(mu_);
        load_names();
        std::vector<VoiceEntry> out;
        for (size_t i = 0; i < names_.size(); ++i)
            if (!names_[i].empty()) out.push_back({kVocalWriterBase + int(i), names_[i]});
        return out;
    }

    bool available(std::string *why) override {
        VwAssets &a = VwAssets::get();
        if (a.ok()) return true;
        if (why) {
            std::string list;
            for (const std::string &m : a.missing()) list += (list.empty() ? "" : ", ") + m;
            *why = "VocalWriter 2.0's own files are not here: " + list;
        }
        return false;
    }

    std::unique_ptr<Singer> singer(int voice_id, std::string *err) override {
        if (!available(err)) return nullptr;
        int index = voice_id - kVocalWriterBase;
        std::unique_ptr<vw_editor, void (*)(vw_editor *)> e(VwAssets::get().open_editor(), vw_ed_close);
        if (!e) {
            if (err) *err = "VocalWriter's engine would not start";
            return nullptr;
        }
        int rc = vw_ed_voice(e.get(), index);
        if (rc == -2) {
            if (err) *err = "that voice is built on the instrument bank, and GMBank.rsrc is not there";
            return nullptr;
        }
        if (rc != 0) {
            if (err) *err = "there is no voice " + std::to_string(index) + " in the bank";
            return nullptr;
        }
        return std::make_unique<VwSinger>(index);
    }

private:
    void load_names() {
        if (loaded_) return;
        loaded_ = true;
        std::unique_ptr<vw_editor, void (*)(vw_editor *)> e(VwAssets::get().open_editor(), vw_ed_close);
        if (!e) return;
        int count = vw_ed_voice_count(e.get());
        for (int i = 0; i < count; ++i) {
            const char *n = vw_ed_voice_name_at(e.get(), i);
            names_.push_back(n ? mac_roman_to_utf8(n) : "");
        }
    }

    static std::string mac_roman_to_utf8(const char *s) {
        std::string out;
        for (; *s; ++s) {
            unsigned char c = (unsigned char)*s;
            if (c < 0x80) out.push_back(char(c));
            else out += "?";
        }
        return out;
    }

    std::mutex mu_;
    bool loaded_ = false;
    std::vector<std::string> names_;
};

}  // namespace

std::unique_ptr<Family> make_vocalwriter_family() { return std::make_unique<VocalWriterFamily>(); }

std::vector<int> vocalwriter_program_voices() {
    std::vector<int> out(128, -1);
    std::unique_ptr<vw_editor, void (*)(vw_editor *)> e(VwAssets::get().open_editor(), vw_ed_close);
    if (!e) return out;
    for (int p = 0; p < 128; ++p) out[size_t(p)] = vw_ed_program_voice(e.get(), p);
    return out;
}

bool vocalwriter_song(const std::vector<std::pair<int, Part>> &parts, int room, int wet,
                      std::vector<float> *stereo, std::string *error) {
    std::vector<SeqPart> seq(parts.size());
    std::vector<vwseq_part> sung;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!to_seq(parts[i].second, parts[i].first - kVocalWriterBase, &seq[i])) continue;
        // a level between VocalWriter's whole percents is put on the samples,
        // which one mix cannot do part by part
        if (seq[i].residual != 1.0) return false;
        sung.push_back(seq[i].p);
    }
    if (sung.empty() || sung.size() > 31) return false;
    SeqEngine &s = seq_engine();
    std::lock_guard<std::mutex> lock(s.mu);
    if (!open_seq_engine(s)) {
        *error = s.error;
        return false;
    }
    vwseq_song song{sung.data(), int(sung.size()), room, wet};
    stereo->clear();
    int rc = vwseq_sing_song(s.e, &song, collect, stereo);
    if (rc != 0) {
        *error = "VocalWriter's sequencer stopped (" + std::to_string(rc) + ")";
        stereo->clear();
        return false;
    }
    return true;
}

std::string vocalwriter_engine_name() { return VwAssets::get().ok() ? "C engine" : "not available"; }

bool vocalwriter_reverb(std::vector<float> &mix, int room, int wet) {
    if (wet <= 0 || mix.empty()) return false;
    std::unique_ptr<vw_editor, void (*)(vw_editor *)> e(VwAssets::get().open_editor(), vw_ed_close);
    if (!e) return false;
    if (vw_ed_reverb(e.get(), room / 100.0f, wet / 100.0f) != 0) return false;
    // The reverberator works on 16-bit samples, 220 frames at a time, as the
    // application's own sound buffers did; the tail needs a second of room.
    size_t frames_in = mix.size() / 2;
    size_t padded = frames_in + kSampleRate;
    size_t frames = (padded / 220) * 220;
    std::vector<int16_t> pcm(frames * 2, 0);
    for (size_t i = 0; i < std::min(frames * 2, mix.size()); ++i) {
        float c = std::max(-1.0f, std::min(1.0f, mix[i]));
        pcm[i] = int16_t(c * 32767.0f);
    }
    vw_ed_reverberate(e.get(), pcm.data(), int32_t(frames));
    // keep whatever of the tail is not silence
    size_t last = 0;
    for (size_t f = 0; f < frames; ++f)
        if (std::abs(pcm[2 * f]) > 1 || std::abs(pcm[2 * f + 1]) > 1) last = f + 1;
    size_t keep = std::max(frames_in, std::min(last, frames));
    if (!last) keep = frames_in;
    mix.assign(keep * 2, 0.0f);
    for (size_t i = 0; i < keep * 2; ++i) mix[i] = pcm[i] / 32767.0f;
    return true;
}

}  // namespace svs
