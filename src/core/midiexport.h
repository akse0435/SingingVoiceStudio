// midiexport.h -- a song out as a Standard MIDI File, as VocalWriter's own
// File > Export wrote one.
//
// A type 1 file at 240 ticks a beat: a first track with the time signature
// and the tempo and its changes, then a track for each part, on the MIDI
// channel chosen for it. A sung note carries its word as a Text event and its
// phonemes, spelled VocalWriter's way, as a Lyric event, both on the note; a
// played note is a note. The pitch bend goes out with the bend range it needs
// (RPN 0), and of VocalWriter's controls the volume (CC 7) and the sustain
// pedal (CC 64), as VocalWriter's export had them; the voice is a program
// change, the pan CC 10 and the mod wheel CC 1. A track's volume scales its
// velocities, as VocalWriter scaled them by its track level. The controls
// VocalWriter's export left out go on the controllers midifile.h names, so
// a song without them is the file VocalWriter wrote.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/project.h"

namespace svs {

//: One track of the song as it goes into the file, and its channel, 0-15.
struct MidiOut {
    const Track *track;
    int channel;
};

//: The tracks a MIDI file is made of: those with something sung or played
//: in them. A backing track is a sound file, and a track that only keeps the
//: tempo goes into the file's first track.
std::vector<const Track *> midi_export_tracks(const Song &song);

//: The channels VocalWriter's export starts the tracks on: a track the drum
//: kit plays on channel 10, the others from channel 1 up in order, 10 left
//: out, going round again after 16.
std::vector<int> default_midi_channels(const std::vector<const Track *> &tracks,
                                       const std::map<int, int> &program_map);

//: Write the song. `program_map` says which program change reaches each of
//: VocalWriter's voices (Studio's, from vocalwriter_program_voices). Throws
//: std::runtime_error with a readable message.
void export_midi(const std::string &path, const Song &song, const std::vector<MidiOut> &tracks,
                 const std::map<int, int> &program_map);

}  // namespace svs
