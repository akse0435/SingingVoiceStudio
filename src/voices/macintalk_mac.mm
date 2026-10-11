// macintalk_mac.mm -- speaking through AVSpeechSynthesizer (see macintalk_mac.h).
#include "voices/macintalk_mac.h"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace svs {
namespace macspeech {

namespace {

NSString *identifier_for(const std::string &bundle) {
    return [@"com.apple.speech.synthesis.voice." stringByAppendingString:[NSString stringWithUTF8String:bundle.c_str()]];
}

//: What a render collects; shared with the block that AVSpeech calls.
struct Collect {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<float> samples;
    double rate = 0.0;
    bool done = false;
};

//: One utterance at a time: AVSpeechSynthesizer is not promised to be
//: thread-safe, and the parts are sung one after another anyway.
std::mutex g_one_at_a_time;

void take(const std::shared_ptr<Collect> &c, AVAudioPCMBuffer *pcm) {
    const AVAudioFormat *fmt = pcm.format;
    AVAudioFrameCount n = pcm.frameLength;
    unsigned channels = fmt.channelCount;
    std::lock_guard<std::mutex> lock(c->mu);
    if (c->rate == 0.0) c->rate = fmt.sampleRate;
    size_t at = c->samples.size();
    c->samples.resize(at + n, 0.0f);
    // the first channel; these voices are mono
    if (pcm.floatChannelData) {
        const float *src = fmt.isInterleaved ? pcm.floatChannelData[0] : pcm.floatChannelData[0];
        unsigned stride = fmt.isInterleaved ? channels : 1;
        for (AVAudioFrameCount i = 0; i < n; ++i) c->samples[at + i] = src[i * stride];
    } else if (pcm.int16ChannelData) {
        const int16_t *src = pcm.int16ChannelData[0];
        unsigned stride = fmt.isInterleaved ? channels : 1;
        for (AVAudioFrameCount i = 0; i < n; ++i) c->samples[at + i] = src[i * stride] / 32768.0f;
    } else if (pcm.int32ChannelData) {
        const int32_t *src = pcm.int32ChannelData[0];
        unsigned stride = fmt.isInterleaved ? channels : 1;
        for (AVAudioFrameCount i = 0; i < n; ++i) c->samples[at + i] = float(src[i * stride] / 2147483648.0);
    }
}

}  // namespace

bool voice_present(const std::string &bundle) {
    @autoreleasepool {
        return [AVSpeechSynthesisVoice voiceWithIdentifier:identifier_for(bundle)] != nil;
    }
}

std::string system_voice_name(const std::string &bundle) {
    @autoreleasepool {
        AVSpeechSynthesisVoice *v = [AVSpeechSynthesisVoice voiceWithIdentifier:identifier_for(bundle)];
        return v ? std::string([v.name UTF8String]) : std::string();
    }
}

bool render(const std::string &bundle, const std::vector<Piece> &pieces, std::vector<float> *out,
            double *rate, std::string *err) {
    std::lock_guard<std::mutex> one(g_one_at_a_time);
    @autoreleasepool {
        AVSpeechSynthesisVoice *voice = [AVSpeechSynthesisVoice voiceWithIdentifier:identifier_for(bundle)];
        if (!voice) {
            if (err) *err = bundle + " is not among this Mac's voices";
            return false;
        }
        NSMutableAttributedString *text = [NSMutableAttributedString new];
        for (const Piece &p : pieces) {
            NSString *t = [NSString stringWithUTF8String:p.text.c_str()];
            if (!t || t.length == 0) continue;
            NSMutableAttributedString *part = [[NSMutableAttributedString alloc] initWithString:t];
            if (!p.ipa.empty()) {
                NSString *ipa = [NSString stringWithUTF8String:p.ipa.c_str()];
                if (ipa) [part addAttribute:AVSpeechSynthesisIPANotationAttribute value:ipa range:NSMakeRange(0, t.length)];
            }
            [text appendAttributedString:part];
            [text appendAttributedString:[[NSAttributedString alloc] initWithString:@" "]];
        }
        AVSpeechUtterance *utterance = [[AVSpeechUtterance alloc] initWithAttributedString:text];
        utterance.voice = voice;
        utterance.rate = AVSpeechUtteranceDefaultSpeechRate;
        utterance.pitchMultiplier = 1.0f;
        utterance.volume = 1.0f;
        utterance.preUtteranceDelay = 0.0;
        utterance.postUtteranceDelay = 0.0;

        auto collect = std::make_shared<Collect>();
        AVSpeechSynthesizer *synth = [AVSpeechSynthesizer new];
        [synth writeUtterance:utterance
             toBufferCallback:^(AVAudioBuffer *buffer) {
                 AVAudioPCMBuffer *pcm = [buffer isKindOfClass:[AVAudioPCMBuffer class]] ? (AVAudioPCMBuffer *)buffer : nil;
                 if (pcm && pcm.frameLength > 0) {
                     take(collect, pcm);
                     return;
                 }
                 // an empty buffer is the end
                 std::lock_guard<std::mutex> lock(collect->mu);
                 collect->done = true;
                 collect->cv.notify_all();
             }];

        // The samples arrive through the main run loop. On the main thread
        // that loop is run here; from another thread the main thread's loop
        // has to be running already, as the window's is.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        if ([NSThread isMainThread]) {
            while (true) {
                {
                    std::lock_guard<std::mutex> lock(collect->mu);
                    if (collect->done) break;
                }
                if (std::chrono::steady_clock::now() > deadline) break;
                [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
            }
        } else {
            std::unique_lock<std::mutex> lock(collect->mu);
            collect->cv.wait_until(lock, deadline, [&] { return collect->done; });
        }
        std::lock_guard<std::mutex> lock(collect->mu);
        if (!collect->done) {
            if (err)
                *err = "Apple's speech synthesizer did not answer within a minute (it answers through the "
                       "program's main event loop, which has to be running)";
            return false;
        }
        if (collect->samples.empty() || collect->rate <= 0) {
            if (err) *err = "Apple's speech synthesizer returned no sound";
            return false;
        }
        *out = std::move(collect->samples);
        *rate = collect->rate;
        return true;
    }
}

}  // namespace macspeech
}  // namespace svs
