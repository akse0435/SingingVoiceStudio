// family.h -- one synthesiser and the voices it offers.
//
// Every voice in the program has a number that is saved with the song, so the
// numbers are fixed per engine and never depend on what happens to be
// installed: a song written with DECtalk's Betty still says Betty on a machine
// without DECtalk, and says why it cannot sing.
//
//      0 -  999   VocalWriter's voice bank, in the bank's own order
//   1000 - 1999   DECtalk
//   2000 - 2999   the SSI-263
//   3000 - 3999   Microsoft Sam, Mike and Mary, and the SAPI 4 voice modes
//   4000 - 4999   MacinTalk: Apple's voices from Mac OS X 10.4 to 10.7, a
//                 hundred numbers to each generation
//
// The number is not the only thing a song keeps: a voice outside
// VocalWriter's bank is saved with its name as well (see Track::voice_name),
// and the name wins if the two ever disagree.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "voices/singer.h"

namespace svs {

enum : int {
    kVocalWriterBase = 0,
    kDectalkBase = 1000,
    kSsi263Base = 2000,
    kMicrosoftBase = 3000,
    kMacinTalkBase = 4000,
    kFamilySpan = 1000,
};

struct VoiceEntry {
    int id = 0;
    std::string name;    //: as the voice list shows it, e.g. "DECtalk Betty"
    //: The engine as the Engine box shows it, when one family is several
    //: there -- "MacinTalk Leopard" -- or empty for the family's own name.
    //: A voice's name starts with its group's, so the Voice box can leave
    //: it off ("Fred" beside "MacinTalk Leopard").
    std::string group;
    //: false for a voice whose data is missing although its family's is not
    //: (one Mac OS X generation extracted and not another)
    bool installed = true;
};

class Family {
public:
    virtual ~Family() = default;
    //: "VocalWriter", "DECtalk", "SSI-263", "Microsoft"
    virtual std::string name() const = 0;
    //: The voices, always the same list, installed or not.
    virtual std::vector<VoiceEntry> voices() = 0;
    //: Whether it can sing at all; if not, `why` says what is missing in a
    //: sentence that can be read out ("DECtalk's dictionary, dtalk_us.dic,
    //: is not in voices\dectalk").
    virtual bool available(std::string *why) = 0;
    //: A singer for one of its voices, or null with `err` set. A singer is
    //: used from one thread at a time; the renderer makes one per phrase or
    //: keeps one per part, as it likes.
    virtual std::unique_ptr<Singer> singer(int voice_id, std::string *err) = 0;
    //: How loud a voice comes out at full level, as a factor to multiply by
    //: so that it does not clip. 1.0 leaves it alone. The default measures
    //: nothing; an engine with very uneven voices overrides it.
    virtual double headroom(int /*voice_id*/) { return 1.0; }
};

std::unique_ptr<Family> make_vocalwriter_family();
std::unique_ptr<Family> make_dectalk_family();
std::unique_ptr<Family> make_ssi263_family();
std::unique_ptr<Family> make_microsoft_family();
std::unique_ptr<Family> make_macintalk_family();

//: Where Panthera is, as the user has named it in the settings: NVDA's
//: configuration folder (with Panthera's add-on and its macintalk folder in
//: it), or a folder holding panthera_host.exe and the tiger, leopard,
//: snowleopard and lion folders. It is looked in before anywhere else; empty
//: is only the usual places. MacinTalk looks again the next time it is asked.
void set_macintalk_folder(const std::string &folder);

}  // namespace svs
