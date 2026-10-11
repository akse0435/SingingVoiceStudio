#include "audio/decode.h"

#include "core/paths.h"
#include "miniaudio.h"
#include "voices/singer.h"

namespace svs {

const char *const kAudioWildcard =
    "Sound files (*.wav;*.mp3;*.flac;*.ogg)|*.wav;*.mp3;*.flac;*.ogg|All files (*.*)|*.*";

// The file is read into memory first and decoded from there: the decoders'
// own ways of opening a file do not all take a name outside ASCII on Windows.
bool decode_audio(const std::string &path, std::vector<float> *stereo, std::string *error) {
    stereo->clear();
    std::vector<unsigned char> bytes;
    if (!read_file(path, &bytes) || bytes.empty()) {
        *error = file_exists(path) ? "cannot read " + path : "there is no file " + path;
        return false;
    }
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 2, ma_uint32(kSampleRate));
    // the steepest filter the resampler has, for a file at 48 kHz or 22 kHz
    config.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER;
    ma_decoder decoder;
    if (ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &decoder) != MA_SUCCESS) {
        *error = base_name(path) + " is not a WAV, MP3, FLAC or Ogg Vorbis file this program can read";
        return false;
    }
    const ma_uint64 chunk = 65536;
    std::vector<float> buf(size_t(chunk) * 2);
    while (true) {
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&decoder, buf.data(), chunk, &got);
        stereo->insert(stereo->end(), buf.begin(), buf.begin() + long(got) * 2);
        if (r != MA_SUCCESS || got < chunk) break;
    }
    ma_decoder_uninit(&decoder);
    if (stereo->empty()) {
        *error = base_name(path) + " has no sound in it";
        return false;
    }
    return true;
}

}  // namespace svs
