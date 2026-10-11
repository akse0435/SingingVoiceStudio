/* miniaudio's implementation, compiled once, as FastPlay does it. The program
   uses its playback devices, and its decoders for a backing track's file:
   WAV, MP3 and FLAC are miniaudio's own, Ogg Vorbis is stb_vorbis, which
   miniaudio uses when it comes first. The encoders and generators are left
   out. */
#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"
