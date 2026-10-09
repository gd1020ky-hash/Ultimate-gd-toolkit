#pragma once
#include <Geode/Geode.hpp>
#include <vector>

struct Click { int f; bool down; int btn; bool p1; };

// implemented in main.cpp
extern int g_frame;                       // physics-tick counter shared with the macro player
void botDone(std::vector<Click> clicks);  // save the found path and replay it
bool botSafeMode();                       // true while macro/speed/bot/noclip is in use
bool botNoclip();                         // noclip toggle from the menu

namespace bot {
    extern bool armed;                    // search when the next level starts
    bool busy();                          // a search is running
    bool injecting();                     // the bot itself is sending a click
    void begin(PlayLayer* pl);            // call at the end of PlayLayer::setupHasCompleted
    void cancel();                        // call when leaving the level
    void tick(GJBaseGameLayer* gl);       // call at the start of processCommands
}
