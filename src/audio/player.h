// player.h -- playing audio straight from memory, and knowing whether it is
// still playing.
//
// That second part is why this exists: without it one key cannot both start
// the song and stop it. It used to play WAV files -- Windows' media control
// interface on Windows, an AVAudioPlayer on the Mac -- which meant every Play
// wrote the whole song to disk and read it back before a sound came out. Now
// the samples go from the mixer to the sound card through miniaudio, the same
// on every system, and nothing touches the disk.
#pragma once

#include <memory>
#include <vector>

namespace svs {

class Player {
public:
    Player();
    ~Player();
    //: Start these samples from the beginning: interleaved when `channels`
    //: is 2, 44.1 kHz. True if they are playing. The samples are shared, not
    //: copied, so starting a long song is instant.
    bool play(std::shared_ptr<const std::vector<float>> samples, int channels);
    void stop();
    //: Whether a sound is still playing.
    bool playing();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace svs
