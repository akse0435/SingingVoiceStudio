// vwsong.h -- VocalWriter 2.0's own song files (.trk), opened as a song.
//
// Each vocal track comes in with all VocalWriter sings it with: syllables,
// phonemes, lyric text, velocity, voice changes and controls; the tempo
// changes become the song's. Instrument tracks are left out.
#pragma once

#include <string>
#include <vector>

#include "core/project.h"

namespace svs {

bool is_vocalwriter_song(const std::string &path);

struct VwSongImport {
    Song song;
    //: what was left out or changed, a sentence each, to be read out
    std::vector<std::string> notes;
};

//: `program_voices` is the voice bank's program map: which voice each program
//: change picks, -1 for none. Throws std::runtime_error with a readable
//: message.
VwSongImport import_vocalwriter_song(const std::string &path, const std::vector<int> &program_voices);

}  // namespace svs
