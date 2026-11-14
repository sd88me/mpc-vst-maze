// The Maze voice engine (src/maze_voice.c, a Schwung plugin_api_v2 DSP) compiled into the plugin, with the defines the 1.x
// build used. The SDK build compiles C++ only; the engine is written to compile as both.
#define MAZE_LFO 1
#define MAZE_VST 1
#include "../../../src/maze_voice.c"

void* maze_voice_api() { return move_plugin_init_v2(nullptr); }
