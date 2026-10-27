// phonology.h -- the phonemes a note can sing, and how a word divides into
// syllables and a syllable into time.
//
// The phoneme set is VocalWriter's: fifty-seven symbols, ARPABET with its
// allophones, in the engine's own internal order. It is the studio's one
// alphabet -- a note says what it sings in these symbols whichever engine
// sings it, and each engine translates them into its own.
#pragma once

#include <string>
#include <vector>

namespace svs {

struct PhonemeInfo {
    const char *symbol;
    //: 0, 1 and 2 are vowels, 3 consonants, 4 sonorants
    int manner;
    //: the longest and the shortest the tables allow, in milliseconds
    int max_ms;
    int min_ms;
};

//: Put between syllables in a looked-up pronunciation, where the dictionary
//: divides the word (Lexicon::phonemes); never sung.
inline const char *const kSyllableBreak = ".";

//: Every phoneme, in VocalWriter's internal order (its phoneme codes).
const std::vector<PhonemeInfo> &phonemes();
const PhonemeInfo *phoneme(const std::string &symbol);
int phoneme_code(const std::string &symbol);

//: The picker's rows: the symbol and an example word, in the order the
//: application's own palette shows them, followed by the ones it leaves out.
struct PaletteRow {
    std::string symbol;
    std::string example;
};
const std::vector<PaletteRow> &palette();

//: How a symbol written elsewhere -- VocalWriter's MIDI exports -- is
//: spelled in the engine's table.
std::string palette_alias(const std::string &symbol);

bool is_nucleus(const std::string &symbol);
bool is_vowel(const std::string &symbol);

//: The vowel a note holding on to `vowel` sings when the vowel's whole sound
//: is left to the last note: a diphthong's first part, an r-coloured vowel's
//: plain one. The vowel itself if it is neither.
std::string first_sound(const std::string &vowel);

//: The vowel the notes after the first sing when `vowel` opens on a glide
//: that is sung once, on the first note: YU's y, so UW is held after it.
//: The vowel itself if it has none.
std::string after_glide(const std::string &vowel);

bool legal_onset(const std::vector<std::string> &syms);
std::vector<std::vector<std::string>> syllabify(const std::vector<std::string> &phonemes);
std::vector<std::vector<std::string>> regroup(const std::vector<std::string> &phonemes, int count);

//: (opening, closing) scale on the consonant lengths for a setting.
void consonant_factors(double consonants, double *onset, double *coda);

//: Which phoneme a note is held on: its first vowel, or, in a note with no
//: vowel at all, the phoneme with the most room to stretch -- the m of a hum.
//: -1 for a note with no phonemes. What comes before it is the note's onset.
int held_phoneme(const std::vector<std::string> &syms);

//: Split a note's time over its phonemes the way singing does: the
//: consonants short, the vowels holding the rest. See ppc/midi.py in the
//: VocalWriter repository, which measured it against the original.
std::vector<double> syllable_lengths(const std::vector<std::string> &syms,
                                     double ms, double consonants = 1.0);

//: The dictionary's allophony: r-coloured vowels, dark and syllabic l, YU.
std::vector<std::string> allophones(const std::vector<std::string> &phones);

}  // namespace svs
