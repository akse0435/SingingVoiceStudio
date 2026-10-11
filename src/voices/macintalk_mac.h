// macintalk_mac.h -- Apple's own speech synthesis on macOS, for MacinTalk.
//
// On a Mac the MacinTalk voices are the system's: Fred, Agnes, Victoria and
// the rest are still installed (System Settings, Accessibility, Spoken
// Content, System Voice, Manage Voices, under English) and spoken through
// AVSpeechSynthesizer. What that engine no longer does is sing: the TUNE and
// PHON inputs Apple documented for MacinTalk were dropped some releases ago
// -- the engine reads them out as text -- so the singing is done here instead
// (see macintalk.cpp): each syllable is spoken from its phonemes, written in
// the International Phonetic Alphabet, which AVSpeech still accepts, with a
// flat baseline pitch near the note, and the speech is then retimed and
// retuned to the score (macintalk_dsp.h).
//
// Only the speaking is in the .mm file. AVSpeech delivers its samples through
// the main thread's run loop, so a render from another thread needs the main
// thread to be running its loop (the window always is; the command line
// renders on the main thread itself).
#pragma once

#include <string>
#include <vector>

namespace svs {
namespace macspeech {

//: One run of the utterance: `text` with an IPA transcription that AVSpeech
//: speaks in place of it, or, with `ipa` empty, text read as it is -- which
//: is how the embedded commands ("[[pbas 50]]", "[[slnc 400]]") are sent.
struct Piece {
    std::string text;
    std::string ipa;
};

//: Whether this Mac has the voice whose bundle is `bundle` ("Fred", "BadNews").
bool voice_present(const std::string &bundle);
//: What this macOS calls it ("Wobble" for Deranged), or empty if absent.
std::string system_voice_name(const std::string &bundle);
//: Speak the pieces as one utterance into mono floats; `*rate` is their
//: sample rate (22050 for these voices). False with `*err` set if the voice
//: is missing or the engine did not answer.
bool render(const std::string &bundle, const std::vector<Piece> &pieces, std::vector<float> *out,
            double *rate, std::string *err);

}  // namespace macspeech
}  // namespace svs
