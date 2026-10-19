// spot5 - Handing the music to the background player and taking it back.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console suspends apps the user leaves, so this app cannot play at the
// home screen or in a game. The background player (background/background.cpp)
// can: a payload that runs cspot itself and streams to the "Spot5 Background"
// web app tile, whose audio the system keeps playing. Settings > Quit > Keep
// playing in the background starts it (through elfldr) with this app's login
// and what is playing, then the app closes; at the next launch the app asks for
// the music back.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace service
{

// What plays: the same fields both ways.
struct Playing
{
    std::string context_uri;
    std::vector<std::string> tracks; // all of them, in play order
    int index = 0;                   // the one playing
    std::uint32_t position_ms = 0;
    bool paused = false;
    bool shuffle = false;
    int phone_volume = 65535; // spirc volume, 0..65535
};

struct Handoff
{
    std::string credentials; // the saved login (paths::kCredentials)
    std::string client_id;
    std::string device_name;
    int quality = 2;
    int level = 10; // Settings volume, 0..10
    bool follow_phone = true;
    Playing playing;
};

// Starts the background player and hands it `handoff`. Blocks until it plays
// (up to about a minute: it signs in first). Returns false and sets `error` (a
// message for the user) if it did not take over.
bool hand_to_background(const Handoff &handoff, std::string &error);

// If the background player runs, takes the music back: returns what it played
// and makes it quit. Quick (nothing to wait for) when it does not run.
std::optional<Playing> take_back_from_background();

} // namespace service
