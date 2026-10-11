#include "voices/registry.h"

namespace svs {

Registry &Registry::get() {
    static Registry instance;
    return instance;
}

Registry::Registry() {
    families_.push_back(make_vocalwriter_family());
    families_.push_back(make_dectalk_family());
    families_.push_back(make_ssi263_family());
    families_.push_back(make_microsoft_family());
    families_.push_back(make_macintalk_family());
}

std::vector<VoiceListing> Registry::voices() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!listed_) {
        for (auto &f : families_) {
            bool ok = f->available(nullptr);
            for (const VoiceEntry &v : f->voices())
                cache_.push_back({v.id, v.name, v.group.empty() ? f->name() : v.group, ok && v.installed});
        }
        listed_ = true;
    }
    return cache_;
}

Family *Registry::family_of(int voice_id) {
    int base = voice_id < 0 ? 0 : (voice_id / kFamilySpan) * kFamilySpan;
    for (auto &f : families_) {
        if (base == kVocalWriterBase && f->name() == "VocalWriter") return f.get();
        if (base == kDectalkBase && f->name() == "DECtalk") return f.get();
        if (base == kSsi263Base && f->name() == "SSI-263") return f.get();
        if (base == kMicrosoftBase && f->name() == "Microsoft") return f.get();
        if (base == kMacinTalkBase && f->name() == "MacinTalk") return f.get();
    }
    return nullptr;
}

std::string Registry::voice_name(int voice_id) {
    for (const VoiceListing &v : voices())
        if (v.id == voice_id) return v.name;
    return "voice " + std::to_string(voice_id);
}

std::unique_ptr<Singer> Registry::singer(int voice_id, std::string *err) {
    Family *f = family_of(voice_id);
    if (!f) {
        if (err) *err = "there is no voice " + std::to_string(voice_id);
        return nullptr;
    }
    std::string why;
    if (!f->available(&why)) {
        if (err) *err = why;
        return nullptr;
    }
    return f->singer(voice_id, err);
}

double Registry::headroom(int voice_id) {
    Family *f = family_of(voice_id);
    return f ? f->headroom(voice_id) : 1.0;
}

std::vector<std::string> Registry::status() {
    std::vector<std::string> out;
    for (auto &f : families_) {
        std::string why;
        bool ok = f->available(&why);
        // only the voices that can sing: MacinTalk lists the ones that cannot
        // as well, so the user can be told why
        size_t n = 0;
        for (const VoiceEntry &v : f->voices()) n += v.installed;
        out.push_back(f->name() + ": " +
                      (ok ? std::to_string(n) + " voices ready" : why));
    }
    return out;
}

}  // namespace svs

namespace svs {

bool resolve_voice_names(Song &song, const std::vector<VoiceListing> &voices) {
    bool moved = false;
    for (Track &t : song.tracks) {
        if (!t.voice_name || t.voice_name->empty()) continue;
        bool same = false;
        for (const VoiceListing &v : voices)
            if (t.voice_id && v.id == *t.voice_id && v.name == *t.voice_name) same = true;
        if (same) continue;
        for (const VoiceListing &v : voices)
            if (v.name == *t.voice_name) {
                t.voice_id = v.id;
                moved = true;
                break;
            }
    }
    return moved;
}

std::optional<std::string> saved_voice_name(int voice_id, const std::vector<VoiceListing> &voices) {
    // VocalWriter's own voices are only a number, as VocalWriter Studio
    // writes them, so a song sung by them alone is the same file it was
    if (voice_id < kFamilySpan) return std::nullopt;
    for (const VoiceListing &v : voices)
        if (v.id == voice_id) return v.name;
    return std::nullopt;
}

}  // namespace svs
