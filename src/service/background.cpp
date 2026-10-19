// spot5 - Handing the music to the background player and taking it back.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "service/background.hpp"

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <exception>

#include "nlohmann/json.hpp"
#include "platform/ps5/system.hpp"
#include "service/elf_loader.hpp"

namespace service
{

namespace
{

constexpr const char *kPayload = "/app0/assets/spot5-background.elf";
constexpr int kControlPort = 8797; // background/background.cpp
constexpr std::size_t kMaxState = 4u << 20;

bool send_all(int fd, const std::string &data)
{
    std::size_t sent = 0;
    while (sent < data.size())
    {
        const ssize_t now = send(fd, data.data() + sent, data.size() - sent, 0);
        if (now <= 0)
            return false;
        sent += static_cast<std::size_t>(now);
    }
    return true;
}

// One line, without "\n"; false on a timeout or a closed link.
bool recv_line(int fd, std::string &line)
{
    line.clear();
    char c;
    while (line.size() < 512)
    {
        if (recv(fd, &c, 1, 0) != 1)
            return false;
        if (c == '\n')
            return true;
        line += c;
    }
    return false;
}

void set_timeout(int fd, int seconds)
{
    timeval timeout{seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

nlohmann::json playing_json(const Playing &p)
{
    return {{"context_uri", p.context_uri}, {"tracks", p.tracks}, {"index", p.index},
            {"position_ms", p.position_ms}, {"paused", p.paused}, {"shuffle", p.shuffle},
            {"phone_volume", p.phone_volume}};
}

} // namespace

bool hand_to_background(const Handoff &handoff, std::string &error)
{
    if (!send_to_elf_loader(kPayload, "Background play", error))
        return false;
    // A previous copy is asked to quit first, which takes it a few seconds.
    const int fd = open_ready_link(kControlPort, 12);
    if (fd < 0)
    {
        error = "The background player did not start";
        return false;
    }
    nlohmann::json body = playing_json(handoff.playing);
    body["credentials"] = handoff.credentials;
    body["client_id"] = handoff.client_id;
    body["device_name"] = handoff.device_name;
    body["quality"] = handoff.quality;
    body["level"] = handoff.level;
    body["follow_phone"] = handoff.follow_phone;
    const std::string text = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::string reply;
    set_timeout(fd, 60); // it signs in to Spotify before it answers
    const bool answered =
        send_all(fd, "HANDOFF " + std::to_string(text.size()) + "\n" + text) && recv_line(fd, reply);
    close(fd);
    if (!answered)
    {
        error = "The background player did not answer";
        return false;
    }
    if (reply != "OK")
    {
        error = "Background play failed: " + (reply.rfind("ERR ", 0) == 0 ? reply.substr(4) : reply);
        return false;
    }
    hui::sys::log("[APP] music handed to the background player");
    return true;
}

std::optional<Playing> take_back_from_background()
{
    const int fd = connect_loopback(kControlPort);
    if (fd < 0)
        return std::nullopt; // not running: the usual case
    set_timeout(fd, 10);
    std::string ready, header, text;
    bool ok = recv_line(fd, ready) && ready == "READY" && send_all(fd, "TAKEBACK\n") && recv_line(fd, header) &&
              header.rfind("STATE ", 0) == 0;
    const std::size_t size = ok ? std::strtoul(header.c_str() + 6, nullptr, 10) : 0;
    ok = ok && size <= kMaxState;
    while (ok && text.size() < size)
    {
        char buffer[4096];
        const ssize_t got = recv(fd, buffer, std::min(sizeof(buffer), size - text.size()), 0);
        if (got <= 0)
            ok = false;
        else
            text.append(buffer, static_cast<std::size_t>(got));
    }
    close(fd);
    if (!ok)
    {
        hui::sys::log("[APP] background player did not hand the music back");
        return std::nullopt;
    }
    try
    {
        const auto j = nlohmann::json::parse(text);
        Playing p;
        p.context_uri = j.value("context_uri", std::string());
        p.tracks = j.value("tracks", std::vector<std::string>());
        p.index = std::max(0, j.value("index", 0));
        p.position_ms = j.value("position_ms", 0u);
        p.paused = j.value("paused", false);
        p.shuffle = j.value("shuffle", false);
        p.phone_volume = j.value("phone_volume", 65535);
        hui::sys::log("[APP] took the music back from the background player (%zu tracks)", p.tracks.size());
        return p;
    }
    catch (const std::exception &e)
    {
        hui::sys::log("[APP] background player state unreadable: %s", e.what());
        return std::nullopt;
    }
}

} // namespace service
