// decode.h -- a sound file read whole, for a backing track.
#pragma once

#include <string>
#include <vector>

namespace svs {

//: The sound files a backing track can play, for a file dialog.
extern const char *const kAudioWildcard;

//: WAV, MP3, FLAC or Ogg Vorbis, read into memory as interleaved stereo
//: floats at the program's own rate (a mono file is put in both channels).
//: Returns false, with `error` saying why, on anything else.
bool decode_audio(const std::string &path, std::vector<float> *stereo, std::string *error);

}  // namespace svs
