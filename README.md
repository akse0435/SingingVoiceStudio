# Singing Voice Studio

VocalWriter Studio rebuilt in C++ and wxWidgets, with more singers in it, for
Windows and macOS. The window, the menus, the keys, the dialogs, the messages
and the project files are VocalWriter Studio's own (see
`C:\stuff\VocalWriter\app`); what is new is that a track can be sung by any of
five synthesisers:

| engine | voices | numbers | what it is |
|---|---|---|---|
| **VocalWriter 2.0** (KAE Labs, 2005) | 87, the bank's own | 0 - 86 | the original's PowerPC code lifted into C (`engine/`, VocalWriterC) |
| **DECtalk** | Paul, Betty, Harry, Frank, Dennis, Kit, Ursula, Rita, Wendy, Val | 1000 - 1009 | github.com/dectalk/dectalk, sung through its phoneme `<duration,pitch>` notation |
| **SSI-263** | SSI-263, Deep, Bright, Child | 2000 - 2003 | the chip itself, driven at register level (github.com/tgeczy/ssi263-speech) |
| **Microsoft** | Sam, Mike, Mary, each plain, in Hall, in Stadium, in Space, Whisper, RoboSoft One to Six | 3000 - 3050 | the SAPI 5 engine rebuilt in C, with the SAPI 4 voice modes (github.com/KamiKitsune420/ms-sam-mike-mary-decomp) |
| **MacinTalk** | Agnes, Albert, Bad News, Bahh, Bells, Boing, Bruce, Bubbles, Cellos, Deranged, Fred, Good News, Hysterical, Junior, Kathy, Pipe Organ, Princess, Ralph, Trinoids, Vicki, Victoria, Whisper, Zarvox, from each of Mac OS X 10.4 Tiger, 10.5 Leopard, 10.6 Snow Leopard and 10.7 Lion, and Alex from 10.5 on | 4000 - 4023, 4100 - 4123, 4200 - 4223, 4300 - 4323 | Apple's own engines: on Windows run by Panthera's host (github.com/tgeczy/panthera-speech) and sung through Apple's TUNE notation; on a Mac the system's own voices, spoken from phonemes and retuned to the score (see below) |

A voice's number is what a song saves, and it is fixed per engine, so a song
written with DECtalk Betty still says Betty on a machine without DECtalk and
says why it cannot sing there.

Everything renders through one path. A part is cut into phrases at its rests,
the consonants are moved in front of the beat so the vowel lands on it, each
phrase is sung by the part's voice and placed where the score puts it, and the
parts are panned and mixed, the ones sharing a reverb setting through one room.
VocalWriter's voices get exactly the calls the original made -- a song sung by
VocalWriter's voices here is the same file, sample for sample, as the Python
VocalWriter Studio renders, and a project saved here is the same file byte for
byte. The other engines are handed the same notes, the same phoneme timings
and one shared pitch curve (written note, bend, detune, portamento, vibrato),
each in its own terms: DECtalk a pitch per phoneme in Hz, the SSI-263 its
inflection register every 32 samples, Microsoft's vocoder twenty pitch knots
per acoustic unit. A new song, and one imported from MIDI, starts with the
portamento and reverb VocalWriter's own songs have: a portamento of 40, and
reverb room 40 at 24 per cent.

**The mod wheel** sits beside pitch bend on every note -- two fields in the
note editor, Mod points for the whole curve, a Mod column in the list, CC 1
when a MIDI file is imported. As General MIDI has it, the wheel is vibrato: at
rest the part sings with its own vibrato setting, and turning it up deepens it
towards full. VocalWriter's voices get it as their own vibrato-depth control,
the others through the shared pitch curve.

**A backing track** (Track > New backing track, Ctrl+Shift+B) is a sound file
-- WAV, MP3, FLAC or Ogg Vorbis -- played under the voices: the band the song
is sung over. It sits in the tracks list like any part, with its own volume,
pan, mute and solo, and Enter on it sets its file and where it starts, in
seconds into the song (below 0 leaves out that much of the file's opening).
It starts at half volume, as a finished recording is far louder than a voice
alone. It is mixed in after the reverb, which is the voices' room alone, and
is placed by the clock rather than the beat, so changing the tempo moves the
voices but not the recording. Play from the cursor starts the file at the
same moment; Export WAV includes it, Export tracks leaves it out (it is a file
already). The project saves where the file is; a project sent along with its
file in the same folder finds it there. VocalWriter Studio reads such a
project too, the backing track as a part with no notes.

Notes are written in VocalWriter's phoneme symbols whichever engine sings
them; each engine translates (the tables are at the top of
`src/voices/<engine>.cpp`). Add word uses VocalWriter's own dictionary.

## Data you supply

None of the synthesisers' data is in this program. It is looked for beside the
executable, the folder above it, the source tree, and `%SVS_DATA%` (on macOS
also the bundle's `Contents/Resources` and the folder the `.app` sits in):

```
assets\                     VocalWriter 2.0's files, laid out as in the
                            VocalWriter repository (VocalWriter.app\...\VocalWriter.rsrc,
                            GMSpeech.rsrc, GMBank.rsrc, EnglishLex)
voices\dectalk\             DECtalk.dll and dtalk_us.dic (scripts\build_dectalk.ps1 makes them)
voices\microsoft\           Sam.spd, Mike.spd, Mary.spd (any of them). Without them, the
                            installed copies are used: Common Files\SpeechEngines\Microsoft\TTS\1033
                            (the SAPI 5 voice package) or Common Files\Microsoft Shared\Speech\1033
                            (Windows XP)
```

MacinTalk is Apple's, run by Panthera: install Panthera's NVDA add-on
(github.com/tgeczy/panthera-speech), which brings its host, `panthera_host.exe`,
and extracts the speech files from your own Mac OS X install discs into NVDA's
configuration folder. That is where they are looked for, unless **Settings >
Panthera folder** names somewhere else (NVDA's configuration folder, or a folder holding
`panthera_host.exe` and the `tiger`, `leopard`, `snowleopard` and `lion` folders):

```
%APPDATA%\nvda\addons\pantheraspeech\synthDrivers\_panthera\panthera_host.exe
%APPDATA%\nvda\macintalk\tiger\         each with Speech\Voices, Speech\Synthesizers and
%APPDATA%\nvda\macintalk\leopard\       SpeechDictionary.framework; Leopard with libstdc++.6.0.4.dylib,
%APPDATA%\nvda\macintalk\snowleopard\   Snow Leopard with libstdc++.6.0.9.dylib, Lion with
%APPDATA%\nvda\macintalk\lion\          libstdc++.6.0.9.dylib and libc++abi.dylib
```

Also found as the add-on finds them: its older folders and pointer files
(`leopardspeech-data.txt` and the like, holding a folder's path) and `%TIGER_TREE%`,
`%LEOPARD_TREE%`, `%SNOWLEOPARD_TREE%`, `%LION_TREE%`; and besides, a running portable
NVDA's `userConfig`, `%NVDA_CONFIG%` for a copy started with `-c`, and
`voices\macintalk\` under the data roots (`panthera_host.exe`, the generation folders,
or `leopard.txt` and the like naming them). One host is started for each generation
the first time it sings, and stays until the program ends.

Fred, Junior, Kathy, Princess, Ralph, Zarvox, Trinoids and Boing (MacinTalk 3),
Agnes, Bruce and Victoria (MacinTalk Pro), Vicki, and Alex (Leopard and Snow Leopard)
sing on Tiger, Leopard and Snow Leopard; Albert, Bahh, Bubbles, Deranged and Whisper
sing in time, on a pitch of their own. Bad News, Bells, Cellos, Good News, Hysterical
and Pipe Organ play their own tune whatever they are given, so they are listed and
cannot sing. Tiger from 10.4.5 on (MacinTalk 3.4) cannot run off Apple's hardware.

Lion's voices are listed and cannot sing either: through Panthera, 10.7 takes a TUNE
and ignores its lengths and pitches. That looks like Panthera's host rather than Lion.
10.7 moved the reading of TUNE into SpeechDictionary, which hands the notes to
MacinTalk as CoreFoundation arrays of dictionaries, and the host's stand-in for
CoreFoundation has no `CFArrayCreate`, `CFDictionaryCreate`, `CFArrayGetCount` or
`CFArrayGetValueAtIndex`.

The engines are asked for each note without its vibrato, so that a held note is one
line of TUNE: a line follows eleven pitch points and no more, and a note cut into
several lines is heard starting again at every cut. Each vowel is then measured as
sung and moved, a period at a time, onto its exact pitch with the vibrato in; and
MacinTalk Pro's and Vicki's held vowels, which die away the longer they are held, are
kept at the loudness they began at.

On a Mac MacinTalk needs nothing supplied: the voices are the system's own
(System Settings, Accessibility, Spoken Content, System Voice, Manage Voices,
under English; macOS now calls Deranged Wobble, Hysterical Jester and Princess
Superstar). Apple's engine there no longer takes the TUNE or PHON input, so
each syllable is spoken from its phonemes in IPA with a flat baseline near
the note, and the speech is retimed and retuned to the score by
pitch-synchronous overlap-add (`src/voices/macintalk_dsp.cpp`): vibrato,
bends and glides included. The list shows one set of voices, numbered as
Snow Leopard's are on Windows, and a song from Windows sings with the Mac's
voice of the same name whichever generation it named. The six novelty voices
that play their own tune (Bells, Cellos, Pipe Organ, Bad News, Good News,
Hysterical) are listed and cannot sing, as on Windows.

On macOS DECtalk is `libtts.dylib`, built here with clang
(`scripts/build_dectalk_mac.sh`) and kept in the bundle's `Contents/Frameworks`.

The SSI-263's phoneme ROM is compiled in. An engine whose data is missing still
lists its voices, marked "(not installed)", and says what is missing in
Messages when the program starts.

## Build

msys2's mingw64 GCC, CMake and Ninja. wxWidgets is fetched from GitHub and built
with the program, statically, at a pinned master commit (see CMakeLists.txt), so
the first build takes a while longer:

```
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja git
sh build.sh
```

That gives `build\SingingVoiceStudio.exe`, the editor, and `build\svs.exe`, the
same program as a console program. Both are linked statically and need no
DLLs of their own. DECtalk only builds with Visual Studio, so it is built
separately and loaded at run time:

```
powershell -File scripts\build_dectalk.ps1
```

Or all of it at once -- DECtalk, the program, every engine's data gathered
beside it, a check that it sings from there, and a zip:

```
powershell -ExecutionPolicy Bypass -File scripts\build_all.ps1          (add -Clean to start afresh)
```

That makes `dist\SingingVoiceStudio\` and `dist\SingingVoiceStudio-<date>.zip`,
which runs on any 64-bit Windows with nothing installed. It carries KAE Labs'
and Microsoft's data, so it is for your own machines, not for publishing.

`-DSVS_ENGINES="ssi263;microsoft"` (through `SVS_CMAKE_ARGS`) builds without an
engine; its voices stay listed.

### macOS

Xcode's command line tools, CMake and Ninja (`brew install cmake ninja`):

```
sh scripts/build_mac.sh
```

That builds wxWidgets statically into `build-deps/wx` the first time, DECtalk
with clang, then the program, and gathers every engine's data into
`dist/Singing Voice Studio.app` (VocalWriter's files from `assets/` here or a
VocalWriter Studio checkout beside this one, Microsoft's from
`voices/microsoft`), checks that each engine sings from inside the bundle,
signs it with the Developer ID in the keychain, puts it on
`dist/SingingVoiceStudio-<version>-macos-<arch>.dmg`, and has Apple notarize
both. `--no-sign` gives a bundle to run here; `--no-notarize` skips Apple; see
the top of the script for the credentials it looks for. `cmake --build build`
alone gives `build/SingingVoiceStudio.app` and `build/svs`, which find the
data in the source tree.

The program is the same on a Mac, with Cmd where this file says Ctrl and
Option where it says Alt, two keys excepted: hearing one note is Option+H,
since Cmd+H hides the program, and quitting is Cmd+Q. The lists are native
macOS tables there, so VoiceOver reads them, and what a key did is announced
through VoiceOver. On the command line the program is
`Singing Voice Studio.app/Contents/MacOS/svs`, the same `svs` as on Windows.
Settings live in `~/Library/Application Support/Singing Voice Studio`.

### Releases

Pushing a tag cuts a release: GitHub Actions (`.github/workflows/release.yml`)
builds the Windows zip and the Mac disk image, each with every voice's data in
it as before, checks that each engine sings from it, and puts both on the
release for the tag, with the tag's message as its notes.

```
git tag -a v1.4 -m "what is new"
git push origin v1.4
```

The tag must match `SVS_VERSION` in CMakeLists.txt. The voice data is not in
the repository: `scripts/voice_data.py` fetches VocalWriter's files from the
VocalWriter Studio repository and Microsoft's voices from the newest release
that has them; DECtalk is built from source. The disk image is signed and
notarized when the repository has the signing secrets listed at the top of the
workflow, and is otherwise signed ad hoc. Run by hand, from the Actions tab, the
workflow builds both and keeps them as the run's artifacts without releasing.

## The keys

As VocalWriter Studio's, plus a few. F1 lists them all in a window. On a Mac read Cmd for
Ctrl and Option for Alt, and Option+H to hear a note.

| | |
|---|---|
| F6 | between the tracks list and the notes list |
| Ctrl+T | add a track |
| Ctrl+Shift+B | add a backing track, a sound file played under the voices |
| Enter on a track | its name, voice, volume, pan and voice controls; a backing track's file and where it starts |
| M / S on a track | mute / solo |
| Delete on a track | remove it |
| Ctrl+Up / Ctrl+Down | reorder the parts, or move a note |
| Ctrl+W / Ctrl+N / Ctrl+R | add a word / a note / a rest, after the selected note |
| Ctrl+Shift+R | a rest to the end of the bar |
| Ctrl+E, or Enter on a note | edit it, including its pitch bend |
| Ctrl+D, or Delete | remove it |
| Alt+Up / Alt+Down | transpose a semitone |
| Alt+Right / Alt+Left | a sixteenth note longer or shorter |
| Alt+Shift+Up / Alt+Shift+Down | transpose an octave |
| Alt+Shift+Right / Alt+Shift+Left | twice or half as long |
| Ctrl+Z / Ctrl+Shift+Z | undo / redo |
| Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+A | copy, cut, paste, select all |
| Ctrl+G | go to a bar |
| Ctrl+, | song settings |
| Ctrl+Shift+P | hear a note whenever it is nudged |
| Space | play from the cursor, or stop |
| Ctrl+P / Ctrl+H / Ctrl+M / Ctrl+. | play from the start / hear one note / metronome / stop |
| Ctrl+O / Ctrl+S | open and save a project |
| Ctrl+I | import a MIDI file |
| Ctrl+Shift+S / Ctrl+Shift+T | export one WAV / one WAV per track |

## From the command line

```
svs song.vws -o song.wav
svs tune.mid -o tune.wav --voice "DECtalk Betty" --tempo 96
svs song.vws --tracks stems
svs --list-voices
svs --pronounce daisy bicycle
```

## Layout

```
src/app/       the window, the dialogs, the command line (studio.cpp is VocalWriter
               Studio's app/studio.py, method for method; widgets.h is the list
               that is wxListCtrl on Windows and a native table on macOS;
               announce.cpp and announce_mac.mm speak to the screen reader)
src/core/      the song, projects, MIDI import, phonology, settings and recovery
src/audio/     the mixer (VocalWriter Studio's ppc/engine.py), WAV files, the player
               (player.cpp on Windows, player_mac.mm on macOS)
src/voices/    singer.h (what an engine is asked to do), the four engines, the shared
               pitch curve
engine/        VocalWriter's synthesiser in C (VocalWriterC)
third_party/   DECtalk, the SSI-263, Microsoft Sam's reconstruction, nlohmann/json
scripts/       the Windows build (build_all.ps1, build_dectalk.ps1) and the macOS
               build (build_mac.sh, build_dectalk_mac.sh)
tests/engines/ a standalone sing test for each engine
```

## Licences

The code here is MIT, as VocalWriter Studio's is. VocalWriter 2.0 and its data
are KAE Labs'; DECtalk, the SSI-263 emulation and the Sam reconstruction carry
their own licences in `third_party/`. Microsoft's voice files are Microsoft's
and are not included.
