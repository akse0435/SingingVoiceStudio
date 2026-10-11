// sing_test.cpp -- the MacinTalk singers, sung and measured.
//
//   macintalk_sing_test [--out DIR] [--gen tiger,leopard] [--voice Fred,Victoria]
//                       [--only daisy|pitch|timing|esses|bend]
//   macintalk_sing_test --calibrate [--gen ...] [--voice ...]
//
// Without Panthera and Apple's data it lists the voices, says what is
// missing, and checks the phoneme table. With them, every voice that can
// sing (or the ones named) sings these to WAV files in DIR:
//
//   daisy   a tune to listen to
//   pitch   an open vowel held on notes over two octaves, with no vibrato;
//           the F0 of each against the note (within 8 cents)
//   timing  notes a major third apart, each a voiced consonant and a vowel:
//           where each note's pitch begins against where the pitch curve
//           starts it (within 10 ms)
//   esses   every note an s after a held vowel: where the vowel is heard to
//           give way, shown and not counted (MacinTalk 3's vowels die away
//           after the s begins, MacinTalk Pro's fade before it)
//   bend    one long note with a deep vibrato, the mod wheel turned up and a
//           bend of two semitones; the F0 frame by frame against the curve
//   daisy again, which must be the same samples
//
// and every file is checked for its length (the phrase's own) and its peak.
// The voices that make their own pitch are only checked for that; Boing's
// and Trinoids' pitch is shown and not counted. --calibrate measures each
// voice's pitch offset across the range, and its loudness, without the
// tuning in macintalk.cpp, and prints the table's rows.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "audio/wav.h"
#include "core/paths.h"
#include "core/phonology.h"
#include "voices/family.h"
#include "voices/pitch.h"

namespace svs {
void macintalk_raw(bool raw);
std::string macintalk_self_check();
}  // namespace svs

using namespace svs;

namespace {

struct Syl {
    int midi;
    double beats;
    const char *phonemes;
};

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

Phrase make_phrase(double bpm, const std::vector<Syl> &syls, int vibrato, int velocity = 127) {
    Phrase p;
    p.bpm = bpm;
    p.style.vibrato = vibrato;
    for (const Syl &s : syls) {
        SungNote n;
        n.midi = s.midi;
        n.beats = s.beats;
        n.velocity = velocity;
        n.phonemes = split(s.phonemes, ' ');
        n.durations_ms = syllable_lengths(n.phonemes, s.beats * 60000.0 / bpm);
        p.notes.push_back(n);
    }
    SungNote rest;
    rest.midi = syls.back().midi;
    rest.beats = 0.4;
    rest.velocity = 1;
    rest.phonemes = {kRest};
    rest.durations_ms = {0.4 * 60000.0 / bpm};
    p.notes.push_back(rest);
    return p;
}

// Normalised autocorrelation of x[a, a+n) at a fractional lag.
double nac(const std::vector<float> &x, long a, long n, double lag) {
    long L = long(lag);
    double fr = lag - L, s = 0, e0 = 0, e1 = 0;
    for (long i = a; i < a + n; ++i) {
        if (i + L + 1 >= long(x.size())) break;
        double u = x[size_t(i)];
        double v = (1 - fr) * x[size_t(i + L)] + fr * x[size_t(i + L + 1)];
        s += u * v;
        e0 += u * u;
        e1 += v * v;
    }
    return e0 > 0 && e1 > 0 ? s / std::sqrt(e0 * e1) : 0.0;
}

// F0 of x[a, b) near `guess`, to a fraction of a cent: the correlation peak
// a whole number of periods on, nearest 20 ms (as the SSI-263's test does).
double estimate_f0(const std::vector<float> &x, long a, long b, double guess) {
    const double sr = kSampleRate;
    double period = sr / guess;
    int k = std::max(1, int(std::lround(0.02 * sr / period)));
    long centre = std::lround(k * period), span = std::lround(0.3 * period);
    long n = b - a - centre - span - 2;
    if (n <= 0) return 0.0;
    auto r = [&](long lag) {
        double s = 0;
        for (long i = a; i < a + n; ++i) s += double(x[size_t(i)]) * x[size_t(i + lag)];
        return s;
    };
    long best = centre;
    double top = -1e300;
    for (long lag = centre - span; lag <= centre + span; ++lag) {
        double v = r(lag);
        if (v > top) {
            top = v;
            best = lag;
        }
    }
    double y0 = r(best - 1), y1 = top, y2 = r(best + 1);
    double d = y0 - 2 * y1 + y2;
    double frac = d != 0 ? 0.5 * (y0 - y2) / d : 0.0;
    return sr * k / (best + frac);
}

// The F0 of a frame near `guess` (within a semitone), or 0 if unvoiced.
double frame_f0(const std::vector<float> &x, long a, long n, double guess) {
    const double sr = kSampleRate;
    double p0 = sr / guess, best = -1, at = 0;
    for (double lag = p0 / 1.07; lag <= p0 * 1.07; lag += 0.25) {
        double v = nac(x, a, n, lag);
        if (v > best) {
            best = v;
            at = lag;
        }
    }
    if (best < 0.7) return 0.0;
    double y0 = nac(x, a, n, at - 0.25), y2 = nac(x, a, n, at + 0.25);
    double d = y0 - 2 * best + y2;
    if (d != 0) at += 0.25 * 0.5 * (y0 - y2) / d;
    return sr / at;
}

double rms_ms(const std::vector<float> &x, double ms) {
    long a = std::lround(ms * kSampleRate / 1000.0);
    long b = std::lround((ms + 1) * kSampleRate / 1000.0);
    a = std::max(0L, a);
    b = std::min(b, long(x.size()));
    double s = 0;
    for (long i = a; i < b; ++i) s += double(x[size_t(i)]) * x[size_t(i)];
    return b > a ? std::sqrt(s / double(b - a)) : 0.0;
}

// Where a note starts, from the sound, in ms after where it should: the
// first comes out of silence, the others go from a held vowel to an s, far
// quieter -- the middle of the first five milliseconds, from 25 ms before
// the note, whose level is below half the vowel's before it.
double onset_ms(const std::vector<float> &x, double start_ms, bool first) {
    if (first) {
        for (double m = start_ms; m < start_ms + 60; m += 1)
            if (rms_ms(x, m) > 0.003) return m - start_ms;
        return 999;
    }
    double ref = 0;
    for (double m = start_ms - 60; m < start_ms - 20; m += 1) ref += rms_ms(x, m);
    ref /= 40;
    for (double m = start_ms - 25; m < start_ms + 60; m += 1) {
        double v = 0;
        for (int j = 0; j < 5; ++j) v += rms_ms(x, m + j);
        if (v / 5 < 0.5 * ref) return m + 2.5 - start_ms;
    }
    return 999;
}

double peak_of(const std::vector<float> &x) {
    double p = 0;
    for (float f : x) p = std::max(p, double(std::fabs(f)));
    return p;
}

// Which voices make their own pitch, and where each one sings.
bool own_pitch(const std::string &display) {
    for (const char *n : {"Albert", "Bahh", "Bubbles", "Deranged", "Whisper"})
        if (display == n) return true;
    return false;
}
int home_low(const std::string &display) {
    for (const char *n : {"Kathy", "Princess", "Junior", "Victoria", "Agnes", "Vicki"})
        if (display == n) return 55;   // G3 to G5
    if (display == "Boing") return 36;  // C2 to C4
    return 43;                          // G2 to G4
}

struct Options {
    std::string out = ".";
    std::vector<std::string> gens, voices;
    bool calibrate = false;
    std::string only;     // one phrase alone: daisy, pitch, timing or bend
};

bool wanted(const std::vector<std::string> &list, std::string s) {
    if (list.empty()) return true;
    for (char &c : s) c = char(std::tolower((unsigned char)c));
    for (std::string w : list) {
        for (char &c : w) c = char(std::tolower((unsigned char)c));
        if (w == s) return true;
    }
    return false;
}

std::string slug(std::string s) {
    for (char &c : s) c = c == ' ' ? '_' : char(std::tolower((unsigned char)c));
    return s;
}

// --calibrate: one voice's row of the tuning table.
void calibrate(Family &fam, const VoiceEntry &v, const std::string &display, int gen) {
    std::string err;
    auto singer = fam.singer(v.id, &err);
    if (!singer) return;
    // pitch: a held AA asked for at every third semitone, its F0 in the
    // middle of the note
    double cents[17];
    bool pitched = !own_pitch(display);
    for (int i = 0; i < 17; ++i) {
        cents[i] = 0.0;
        if (!pitched) continue;
        int midi = 36 + 3 * i;
        Phrase p = make_phrase(60, {{midi, 1.2, "AA"}}, 0);
        Sung s = singer->sing(p);
        double want = PitchCurve::midi_to_hz(midi);
        long a = long(0.4 * kSampleRate), b = long(1.0 * kSampleRate);
        // Near the pitch asked for (Boing near three eighths of it, or five
        // twelfths low down), wherever the period correlates best: an octave
        // out is never taken for the note.
        std::vector<double> ratios = {1.0};
        if (display == "Boing") ratios = {0.375, 0.42};
        double best = -1, at = 0;
        for (double ratio : ratios)
            for (double c = -250; c <= 250; c += 2) {
                double f = want * ratio * std::pow(2.0, c / 1200.0);
                double r = nac(s.samples, a, std::min(b - a, long(0.08 * kSampleRate)), kSampleRate / f);
                if (r > best) {
                    best = r;
                    at = f;
                }
            }
        double f0 = best > 0.5 ? estimate_f0(s.samples, a, b, at) : 0.0;
        cents[i] = f0 > 0 ? 1200.0 * std::log2(f0 / want) : 9999;
    }
    // What cannot be trusted is left out and filled from its neighbours:
    // pitches the engine cannot reach (MacinTalk 3 stops at 790 Hz), and a
    // reading that differs from both neighbours by more than a quarter tone
    // (Trinoids, three voices at once, and Zarvox's buzz can fool the
    // correlation on the odd note).
    bool mtk3 = display != "Agnes" && display != "Bruce" && display != "Victoria" && display != "Vicki" && display != "Alex";
    bool keep[17];
    for (int i = 0; i < 17; ++i) {
        double ask = PitchCurve::midi_to_hz(36 + 3 * i);
        keep[i] = std::fabs(cents[i]) < 3000 && (!mtk3 || ask <= 760.0) && (mtk3 || ask <= 1150.0);
    }
    for (int i = 0; i < 17; ++i) {
        if (!keep[i]) continue;
        double l = i > 0 && keep[i - 1] ? cents[i - 1] : NAN, r = i < 16 && keep[i + 1] ? cents[i + 1] : NAN;
        bool off_l = std::isnan(l) || std::fabs(cents[i] - l) > 35, off_r = std::isnan(r) || std::fabs(cents[i] - r) > 35;
        if (off_l && off_r && !(std::isnan(l) && std::isnan(r))) keep[i] = false;
    }
    for (int i = 0; i < 17; ++i) {
        if (keep[i]) continue;
        int a = i - 1, b = i + 1;
        while (a >= 0 && !keep[a]) --a;
        while (b < 17 && !keep[b]) ++b;
        if (a >= 0 && b < 17) cents[i] = cents[a] + (cents[b] - cents[a]) * (i - a) / double(b - a);
        else if (a >= 0) cents[i] = cents[a];
        else if (b < 17) cents[i] = cents[b];
        else cents[i] = 0.0;
    }
    // loudness: "daisy" in the voice's own range at full velocity
    int lo = home_low(display);
    Phrase daisy = make_phrase(150, {
        {lo + 12, 3, "d EY"}, {lo + 9, 3, "z IY"}, {lo + 5, 3, "d EY"}, {lo, 3, "z IY"},
        {lo + 2, 1, "g IH v"}, {lo + 4, 1, "m IY"}, {lo + 5, 1, "y UR"},
        {lo + 2, 2, "AE n"}, {lo + 5, 1, "s ER"}, {lo, 5, "d UW"},
    }, 31);
    Sung s = singer->sing(daisy);
    double peak = peak_of(s.samples);
    double volm = 0.25, gain = peak > 0 ? 0.5 / peak : 1.0;
    if (peak > 0.6) {             // keep the host's samples well inside 16 bits
        volm = 0.25 * 0.6 / peak;
        gain = 0.5 / 0.6;
    }
    std::printf("    {%d, \"%s\", %.3f, %.3f, {", gen, display == "Bad News" ? "BadNews" : display == "Good News" ? "GoodNews" : display == "Pipe Organ" ? "Organ" : display.c_str(), volm, gain);
    for (int i = 0; i < 17; ++i) std::printf("%s%.1f", i ? ", " : "", cents[i]);
    std::printf("}},\n");
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) o.out = argv[++i];
        else if (a == "--gen" && i + 1 < argc) o.gens = split(argv[++i], ',');
        else if (a == "--voice" && i + 1 < argc) o.voices = split(argv[++i], ',');
        else if (a == "--calibrate") o.calibrate = true;
        else if (a == "--only" && i + 1 < argc) o.only = argv[++i];
        else {
            std::printf("usage: macintalk_sing_test [--out DIR] [--gen G,...] [--voice V,...] [--calibrate]\n");
            return 2;
        }
    }
    int failures = 0;
    std::string bad = macintalk_self_check();
    std::printf("phoneme table: %s\n", bad.empty() ? "ok" : bad.c_str());
    if (!bad.empty()) ++failures;

    std::unique_ptr<Family> fam = make_macintalk_family();
    std::string why;
    bool ok = fam->available(&why);
    std::printf("%s: %s\n", fam->name().c_str(), ok ? "available" : why.c_str());
    std::vector<VoiceEntry> voices = fam->voices();
    // the list is fixed: 24 voices in each of four generations (Alex is listed in Tiger too, and is not there)
    if (voices.size() != 96) {
        std::printf("FAIL: %zu voices listed, not 96\n", voices.size());
        ++failures;
    }
    std::string group;
    for (const VoiceEntry &v : voices) {
        if (v.group != group) {
            group = v.group;
            std::printf("%s:", group.c_str());
        }
        std::string display = v.name.substr(v.group.size() + 1);
        std::printf(" %d %s%s", v.id, display.c_str(), v.installed ? "" : " (-)");
        if (&v == &voices.back() || (&v + 1)->group != group) std::printf("\n");
    }
    if (!ok) {
        std::printf("%s\n", failures ? "FAILED" : "nothing to sing with; the table is ok");
        return failures ? 1 : 0;
    }
    make_dirs(o.out);
    if (o.calibrate) macintalk_raw(true);

    for (const VoiceEntry &v : voices) {
        std::string display = v.name.substr(v.group.size() + 1);
        std::string gen_key = v.group.substr(10);   // after "MacinTalk "
        gen_key.erase(std::remove(gen_key.begin(), gen_key.end(), ' '), gen_key.end());
        int gen = (v.id - kMacinTalkBase) / 100;
        if (!wanted(o.gens, gen_key) || !wanted(o.voices, display)) continue;
        std::string err;
        if (!v.installed) {
            fam->singer(v.id, &err);
            if (!o.voices.empty()) std::printf("%s: %s\n", v.name.c_str(), err.c_str());
            continue;
        }
        if (o.calibrate) {
            calibrate(*fam, v, display, gen);
            continue;
        }
        auto singer = fam->singer(v.id, &err);
        if (!singer) {
            std::printf("%s: %s\n", v.name.c_str(), err.c_str());
            ++failures;
            continue;
        }
        std::string tag = slug(gen_key + "_" + display);
        int lo = home_low(display);
        bool pitched = !own_pitch(display);
        // Boing springs into every note and sings three eighths of it, and
        // Trinoids is three voices at once; what they sing is shown and not
        // counted
        bool novelty = display == "Boing" || display == "Trinoids";
        Phrase daisy = make_phrase(150, {
            {lo + 12, 3, "d EY"}, {lo + 9, 3, "z IY"}, {lo + 5, 3, "d EY"}, {lo, 3, "z IY"},
            {lo + 2, 1, "g IH v"}, {lo + 4, 1, "m IY"}, {lo + 5, 1, "y UR"},
            {lo + 2, 2, "AE n"}, {lo + 5, 1, "s ER"}, {lo, 5, "d UW"},
        }, 31);
        std::vector<Syl> held;
        for (int m = lo; m <= lo + 24; m += 4) held.push_back({m, 1, "AA"});
        Phrase pitch = make_phrase(60, held, 0);
        // every note a voiced consonant and a vowel, a major third from the
        // one before, so that where each begins is where its pitch begins
        Phrase timing = make_phrase(120, {
            {lo + 5, 0.5, "l AA"}, {lo + 9, 0.5, "m IY"}, {lo + 5, 0.5, "n AA"}, {lo + 9, 0.5, "w IY"},
            {lo + 5, 0.5, "y AA"}, {lo + 9, 0.5, "l IY"}, {lo + 5, 0.5, "m AA"}, {lo + 9, 1.0, "n IY"},
        }, 31);
        // and the same with an s to start each note, to see where the vowel
        // before it gives way (no pitch to go by: what is heard)
        Phrase esses = make_phrase(120, {
            {lo + 5, 0.5, "s AA"}, {lo + 7, 0.5, "s AA"}, {lo + 9, 0.5, "s IY"}, {lo + 10, 0.5, "s AA"},
            {lo + 12, 0.5, "s AA"}, {lo + 10, 0.5, "s IY"}, {lo + 9, 0.5, "s AA"}, {lo + 7, 1.0, "s AA"},
        }, 31);
        Phrase bend = make_phrase(60, {{lo + 9, 4, "l AA"}}, 60);
        for (int k = 0; k <= 400; ++k) {      // a bend up two semitones over the middle second
            double t = k * 0.005 + 1.5;
            double f = std::min(1.0, std::max(0.0, (t - 1.5) / 1.0));
            bend.bends.push_back({t, 2.0 * f});
        }
        for (int k = 0; k <= 800; ++k)        // and the mod wheel up from 0 to 127 over four seconds
            bend.mods.push_back({k * 0.005, std::min(127.0, k * 0.005 / 4.0 * 127.0)});

        struct Job {
            const char *name;
            const Phrase *phrase;
        } jobs[] = {{"daisy", &daisy}, {"pitch", &pitch}, {"timing", &timing}, {"esses", &esses}, {"bend", &bend}};
        std::vector<float> first_daisy;
        for (const Job &job : jobs) {
            if (!o.only.empty() && o.only != job.name) continue;
            PitchCurve curve(*job.phrase);
            Sung s = singer->sing(*job.phrase);
            double peak = peak_of(s.samples);
            long want = long(std::llround(curve.total() * kSampleRate));
            write_wav(join_path(o.out, "mt_" + tag + "_" + job.name + ".wav"), s.samples, 1);
            bool fine = long(s.samples.size()) == want && peak > 0.02 && peak < 0.99 && !s.stopped_short &&
                        s.error.empty();
            if (!fine) ++failures;
            std::printf("%-30s %-6s %6.3f s (curve %6.3f s)  peak %.3f  %s%s%s\n", v.name.c_str(), job.name,
                        s.samples.size() / double(kSampleRate), curve.total(), peak, fine ? "ok" : "FAIL",
                        s.stopped_short ? " stopped short" : "", s.error.empty() ? "" : (": " + s.error).c_str());
            if (job.phrase == &daisy) first_daisy = s.samples;
            if (job.phrase == &pitch && pitched) {
                double worst = 0;
                std::printf("    pitch (cents):");
                for (size_t i = 0; i + 1 < curve.notes(); ++i) {
                    double a = curve.note_start(i), b = curve.note_end(i), m = b - a;
                    long sa = long((a + 0.3 * m) * kSampleRate), sb = long((b - 0.15 * m) * kSampleRate);
                    double want_hz = PitchCurve::midi_to_hz(job.phrase->notes[i].midi);
                    double f0 = estimate_f0(s.samples, sa, sb, want_hz);
                    double c = f0 > 0 ? 1200.0 * std::log2(f0 / want_hz) : 9999;
                    worst = std::max(worst, std::fabs(c));
                    std::printf(" %d:%+.1f", job.phrase->notes[i].midi, c);
                }
                // within 8 cents: MacinTalk Pro's steps are 50 cents apart by D5
                bool good = worst <= 8.0 || novelty;
                if (!good) ++failures;
                std::printf("  worst %.1f  %s\n", worst, good ? "ok" : "FAIL");
            }
            if (job.phrase == &timing && pitched) {
                // where the pitch of the note before gives way to this one's:
                // the moment that best divides the looks that hear the first
                // from those that hear the second
                double worst = 0;
                std::printf("    note starts by pitch (ms after the note):");
                for (size_t i = 1; i + 1 < curve.notes(); ++i) {
                    double t0 = curve.note_start(i);
                    double fa = PitchCurve::midi_to_hz(job.phrase->notes[i - 1].midi);
                    double fb = PitchCurve::midi_to_hz(job.phrase->notes[i].midi);
                    std::vector<int> heard;
                    for (double t = t0 - 0.08; t < t0 + 0.08; t += 0.001) {
                        long n = long(2.5 * kSampleRate / std::min(fa, fb));
                        long a = long(t * kSampleRate) - n / 2;
                        double ra = 0, rb = 0;
                        for (double d = -0.03; d <= 0.0301; d += 0.01) {
                            ra = std::max(ra, nac(s.samples, a, n, kSampleRate / fa * (1 + d)));
                            rb = std::max(rb, nac(s.samples, a, n, kSampleRate / fb * (1 + d)));
                        }
                        heard.push_back(std::max(ra, rb) < 0.6 ? 0 : ra > rb ? -1 : 1);
                    }
                    int n = int(heard.size()), best = -1000000, at = 0, before = 0, after = 0;
                    for (int v : heard) after += v == 1;
                    for (int j = 0; j <= n; ++j) {
                        if (before + after > best) {
                            best = before + after;
                            at = j;
                        }
                        if (j < n) {
                            before += heard[size_t(j)] == -1;
                            after -= heard[size_t(j)] == 1;
                        }
                    }
                    double late = (-0.08 + at * 0.001) * 1000.0;
                    worst = std::max(worst, std::fabs(late));
                    std::printf(" %+.0f", late);
                }
                // within 10 ms: the looks are 2.5 periods long, and the
                // consonant between the notes is where the change is heard
                bool good = worst <= 10.0 || novelty;
                if (!good) ++failures;
                std::printf("  %s\n", good ? "ok" : "FAIL");
            }
            if (job.phrase == &esses) {
                std::printf("    where each s is heard to begin (ms after the note, not counted):");
                for (size_t i = 0; i + 1 < curve.notes(); ++i)
                    std::printf(" %+.1f", onset_ms(s.samples, curve.note_start(i) * 1000.0, i == 0));
                std::printf("\n");
            }
            if (job.phrase == &bend && pitched) {
                // every 10 ms of the vowel, 30 ms frames, against the curve
                std::vector<double> errs;
                for (double t = 0.3; t + 0.03 < curve.note_end(0) - 0.1; t += 0.01) {
                    double sum = 0;
                    for (int k = 0; k < 7; ++k) sum += curve.midi_at(t + k * 0.005);
                    double mid = sum / 7;
                    double f = frame_f0(s.samples, long(t * kSampleRate), long(0.03 * kSampleRate),
                                        PitchCurve::midi_to_hz(mid));
                    if (f > 0) errs.push_back(std::fabs(1200.0 * std::log2(f / PitchCurve::midi_to_hz(mid))));
                }
                std::sort(errs.begin(), errs.end());
                double med = errs.empty() ? 999 : errs[errs.size() / 2];
                double p90 = errs.empty() ? 999 : errs[errs.size() * 9 / 10];
                bool good = (!errs.empty() && med <= 12 && p90 <= 35) || novelty;
                if (!good) ++failures;
                std::printf("    bend and vibrato: %zu frames, |error| median %.1f cents, 90%% %.1f  %s\n",
                            errs.size(), med, p90, good ? "ok" : "FAIL");
            }
        }
        if (o.only.empty() || o.only == "daisy") {
            Sung again = singer->sing(daisy);
            bool same = again.samples == first_daisy;
            // Snow Leopard's concatenative engine (Vicki, Alex) runs on threads of
            // its own and does not sing a phrase the same way twice
            bool restless = gen == 2 && (display == "Vicki" || display == "Alex");
            if (!same && !restless) ++failures;
            std::printf("    daisy again: %s\n", same ? "the same samples"
                                                       : restless ? "different (the engine's own; not counted)"
                                                                  : "DIFFERENT");
        }
    }
    if (o.calibrate) return 0;
    std::printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
