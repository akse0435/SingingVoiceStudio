#include "audio/player.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include "miniaudio.h"
#include "voices/singer.h"

namespace svs {

// One output device, opened the first time something is played and kept;
// started for each sound and stopped when it ends or is stopped. The audio
// thread reads the samples it is given and nothing else: the buffer is
// swapped under a lock it only ever tries for, so it never waits on the
// window -- if the window is mid-swap it plays a few milliseconds of silence.
struct Player::Impl {
    ma_device device;
    bool opened = false;
    std::mutex mu;
    std::shared_ptr<const std::vector<float>> samples;
    int channels = 1;
    std::atomic<size_t> frame{0};
    std::atomic<bool> done{true};

    static void callback(ma_device *d, void *out, const void *, ma_uint32 count) {
        Impl *self = static_cast<Impl *>(d->pUserData);
        float *o = static_cast<float *>(out);
        std::memset(o, 0, sizeof(float) * count * 2);
        std::unique_lock<std::mutex> lock(self->mu, std::try_to_lock);
        if (!lock.owns_lock() || !self->samples || self->done) return;
        const std::vector<float> &y = *self->samples;
        size_t ch = size_t(self->channels);
        size_t frames = y.size() / ch;
        size_t at = self->frame;
        size_t n = 0;
        for (; n < count && at + n < frames; ++n) {
            float l = y[(at + n) * ch];
            float r = ch == 2 ? y[(at + n) * ch + 1] : l;
            o[n * 2] = l;
            o[n * 2 + 1] = r;
        }
        self->frame = at + n;
        if (at + n >= frames) self->done = true;
    }

    bool open() {
        if (opened) return true;
        ma_device_config c = ma_device_config_init(ma_device_type_playback);
        c.playback.format = ma_format_f32;
        c.playback.channels = 2;
        c.sampleRate = kSampleRate;
        c.dataCallback = callback;
        c.pUserData = this;
        opened = ma_device_init(nullptr, &c, &device) == MA_SUCCESS;
        return opened;
    }
};

Player::Player() : impl_(new Impl) {}

Player::~Player() {
    if (impl_->opened) ma_device_uninit(&impl_->device);
}

bool Player::play(std::shared_ptr<const std::vector<float>> samples, int channels) {
    if (!samples || samples->empty() || !impl_->open()) return false;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->samples = std::move(samples);
        impl_->channels = channels == 2 ? 2 : 1;
        impl_->frame = 0;
        impl_->done = false;
    }
    if (ma_device_is_started(&impl_->device)) return true;
    return ma_device_start(&impl_->device) == MA_SUCCESS;
}

void Player::stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->done = true;
        impl_->samples.reset();
    }
    if (impl_->opened && ma_device_is_started(&impl_->device)) ma_device_stop(&impl_->device);
}

double Player::seconds() const { return double(impl_->frame) / kSampleRate; }

bool Player::playing() {
    if (!impl_->done) return true;
    // finished by itself: let the device go quiet rather than run on silence
    if (impl_->opened && ma_device_is_started(&impl_->device)) ma_device_stop(&impl_->device);
    return false;
}

}  // namespace svs
