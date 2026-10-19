// spot5 - Background player: keeps the music going at the home screen and in games.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A payload (sent to elfldr by the app, like the pairing helper) that takes over
// what the app was playing. Apps are suspended when the user leaves them, but the
// system music core keeps playing an <audio> element of a web app tile, at the
// home screen and during games. So this payload runs cspot and serves its PCM in
// real time as one endless WAV "file" on http://127.0.0.1:8795/live.wav, and the
// "Spot5 Background" tile (a webAppUri page, docs/background/index.html, installed
// by this payload) plays it.
//
// The approach, the live WAV sink, the page's image-only song info and the tile
// installer follow Maksio2019/ps5-spotify (payload/spotify_daemon.cpp,
// probes/webapp_install.c, index.html; GPL-3.0-or-later), which found it and
// verified it on hardware (its howitworks.md has the measurements).
//
// Control link, 127.0.0.1:8797 (one request per connection, after our "READY\n"):
//   HANDOFF <n>\n<json>  the app's login and what it plays (see Handoff); we answer
//                        "OK\n" once it plays here, or "ERR <why>\n"
//   TAKEBACK\n           the app is back: we answer "STATE <n>\n<json>" (what plays
//                        now, same fields) and quit
// Live port, 8795 (any address): /live.wav, /status, /state.png, /meta.png,
// /ver.png, /pos.png, /control.png?a=toggle|next|prev, /quit.
// Files: /data/spot5/background.log (+ .prev), covers for the song toast in
// /data/spot5/covers, optional page address in /data/spot5/background_url.txt.
// Nothing is saved about the login: the app owns it.

#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "BellLogger.h"
#include "CSpotContext.h"
#include "HTTPClient.h"
#include "LoginBlob.h"
#include "MercurySession.h"
#include "SpircHandler.h"
#include "TrackPlayer.h"
#include "TrackQueue.h"
#include "X509Bundle.h"
#include "nlohmann/json.hpp"

extern "C"
{
#include <ps5/kernel.h>
}

// Files built in (paths from tools/build-background.sh).
#define INCASSET(name, file)                                                                                \
    __asm__(".section .rodata\n"                                                                            \
            ".global " #name "\n"                                                                           \
            ".global " #name "_end\n"                                                                       \
            ".align 16\n" #name ":\n"                                                                       \
            ".incbin \"" file "\"\n" #name "_end:\n"                                                        \
            ".previous\n");                                                                                 \
    extern "C" const std::uint8_t name[];                                                                   \
    extern "C" const std::uint8_t name##_end[];

INCASSET(kCertBundle, CERT_BUNDLE_PATH)
INCASSET(kTileIcon, TILE_ICON_PATH)

namespace
{
constexpr int kLivePort = 8795;
constexpr int kControlPort = 8797;
constexpr const char *kDataDir = "/data/spot5";
constexpr const char *kLogPath = "/data/spot5/background.log";
constexpr const char *kLogPrevious = "/data/spot5/background.prev.log";
constexpr const char *kCoverDir = "/data/spot5/covers";
constexpr const char *kUrlFile = "/data/spot5/background_url.txt";

// The tile. Its page must be https (the web app launcher refuses http pages); it
// may still load audio and images, not data, from the console over plain http.
constexpr const char *kTileId = "SPOT50001";
constexpr const char *kTileName = "Spot5 Background";
// A build may point it elsewhere (tools/build-background.sh, BACKGROUND_PAGE_URL).
#ifdef BACKGROUND_PAGE_URL
constexpr const char *kDefaultPageUrl = BACKGROUND_PAGE_URL;
#else
constexpr const char *kDefaultPageUrl = "https://abhi245y.github.io/spot5/background/";
#endif

// The music core holds about this much audio before it is heard (WAV; MP3 is
// 3-5 s): the position we report lags by it, and the song toast waits for it.
constexpr int kHeardDelayMs = 1800;

constexpr int kRate = 44100; // cspot's rate; the music core takes 44.1 kHz WAV
constexpr int kBytesPerFrame = 4; // 16-bit stereo
constexpr long long kBytesPerSecond = static_cast<long long>(kRate) * kBytesPerFrame;
constexpr int kWavHeaderSize = 44;
// The "file" the music core sees: a header and 6 hours of PCM (the WAV size
// fields are 32 bits). A 6-hour session reconnects at the end like after an error.
constexpr long long kWavDataSize = 6LL * 3600 * kBytesPerSecond;
constexpr long long kLiveLength = kWavHeaderSize + kWavDataSize;
constexpr int kChunkFrames = kRate / 20; // 50 ms
// New listeners start this far behind the live point so they can buffer at once.
constexpr int kDefaultBacklogMs = 250;

// As the app's Session::play: at most this many tracks per Spirc frame, more
// appended when this few are left.
constexpr std::size_t kQueueWindow = 100;
constexpr std::size_t kQueueLowWater = 10;

std::atomic<bool> g_quit{false};

// ---------------------------------------------------------------- utilities

struct NotifyRequest
{
    char useless1[45];
    char message[3075];
};
extern "C" int sceKernelSendNotificationRequest(int, NotifyRequest *, std::size_t, int);

void notify(const char *format, ...)
{
    NotifyRequest request;
    std::memset(&request, 0, sizeof request);
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(request.message, sizeof request.message, format, arguments);
    va_end(arguments);
    sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
    std::printf("[notify] %s\n", request.message);
}

// System libraries are loaded with dlopen() after start: linking them can hang a
// payload before main() (ps5-spotify's finding).
template <typename F> F system_function(const char *library, const char *name)
{
    void *handle = dlopen(library, RTLD_LAZY);
    F function = handle != nullptr ? reinterpret_cast<F>(dlsym(handle, name)) : nullptr;
    std::printf("[main] %s %s\n", name, function != nullptr ? "found" : "missing");
    return function;
}

using NotificationSend = int (*)(int user, bool logged, const char *payload);
NotificationSend g_notification_send = nullptr;

// The SDK notify sample's rich toast: cover, title and artist.
bool rich_toast(const std::string &icon, const std::string &message, const std::string &sub_message)
{
    if (g_notification_send == nullptr)
        return false;
    static std::atomic<unsigned> counter{0};
    const std::time_t now = std::time(nullptr);
    char created[32];
    std::strftime(created, sizeof created, "%Y-%m-%dT%H:%M:%S.000Z", std::gmtime(&now));
    using nlohmann::json;
    const json icon_json = {{"type", "Url"}, {"parameters", {{"url", icon}}}};
    const json toast = {
        {"rawData",
         {{"viewTemplateType", "InteractiveToastTemplateB"},
          {"channelType", "Downloads"},
          {"useCaseId", "IDC"},
          {"toastOverwriteType", "No"},
          {"isImmediate", true},
          {"priority", 100},
          {"viewData",
           {{"icon", icon_json}, {"message", {{"body", message}}}, {"subMessage", {{"body", sub_message}}}}},
          {"platformViews",
           {{"previewDisabled", {{"viewData", {{"icon", icon_json}, {"message", {{"body", message}}}}}}}}}}},
        {"createdDateTime", created},
        {"localNotificationId", std::to_string(static_cast<unsigned>(now) * 16 + (counter++ % 16))}};
    const std::string text = toast.dump(-1, ' ', false, json::error_handler_t::replace);
    const int result = g_notification_send(0xFE /* the system user */, true, text.c_str());
    return result >= 0;
}

std::string read_file(const char *path)
{
    std::string content;
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return content;
    char buffer[4096];
    ssize_t got;
    while ((got = read(fd, buffer, sizeof buffer)) > 0)
        content.append(buffer, static_cast<std::size_t>(got));
    close(fd);
    return content;
}

bool write_file(const std::string &path, const void *data, std::size_t size)
{
    FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        return false;
    const bool wrote = size == 0 || std::fwrite(data, 1, size, file) == size;
    return std::fclose(file) == 0 && wrote;
}

bool send_all(int fd, const void *data, std::size_t size)
{
    const char *p = static_cast<const char *>(data);
    while (size > 0)
    {
        const ssize_t sent = send(fd, p, size, 0);
        if (sent <= 0)
            return false;
        p += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool send_text(int fd, const std::string &text)
{
    return send_all(fd, text.data(), text.size());
}

// One line (without "\n"), at most `limit` bytes.
bool recv_line(int fd, std::string &line, std::size_t limit = 256)
{
    line.clear();
    char c;
    while (line.size() < limit)
    {
        if (recv(fd, &c, 1, 0) != 1)
            return false;
        if (c == '\n')
            return true;
        line += c;
    }
    return false;
}

bool recv_exact(int fd, std::string &out, std::size_t size)
{
    out.resize(size);
    std::size_t have = 0;
    while (have < size)
    {
        const ssize_t got = recv(fd, out.data() + have, size - have, 0);
        if (got <= 0)
            return false;
        have += static_cast<std::size_t>(got);
    }
    return true;
}

int open_listener(int port, bool loopback_only)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in address{};
    address.sin_len = sizeof address;
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0 || listen(fd, 16) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

// A 16-byte Spotify id as its base62 URI part (as the app's Session::queue).
std::string base62(std::vector<std::uint8_t> number)
{
    static const char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string out;
    for (int i = 0; i < 22; ++i)
    {
        int remainder = 0;
        for (std::uint8_t &byte : number)
        {
            const int value = remainder * 256 + byte;
            byte = static_cast<std::uint8_t>(value / 62);
            remainder = value % 62;
        }
        out.insert(out.begin(), kDigits[remainder]);
    }
    return out;
}

// ---------------------------------------------------------------- what plays

// What the app hands over (HANDOFF) and gets back (TAKEBACK).
struct Handoff
{
    std::string credentials; // the app's saved reusable login (JSON)
    std::string client_id;
    std::string device_name;
    int quality = 2;            // AudioFormat_OGG_VORBIS_96/160/320
    int level = 10;             // the console's own volume step, 0..10
    bool follow_phone = true;   // the phone's volume applies too
    int phone_volume = 65535;   // spirc volume, 0..65535
    std::string context_uri;
    std::vector<std::string> tracks; // in play order, all of them
    int index = 0;              // the one playing
    std::uint32_t position_ms = 0;
    bool paused = false;
    bool shuffle = false;
};

nlohmann::json state_json(const Handoff &h)
{
    return {{"context_uri", h.context_uri}, {"tracks", h.tracks},  {"index", h.index},
            {"position_ms", h.position_ms}, {"paused", h.paused},   {"shuffle", h.shuffle},
            {"phone_volume", h.phone_volume}};
}

Handoff parse_handoff(const std::string &text)
{
    const auto j = nlohmann::json::parse(text);
    Handoff h;
    h.credentials = j.at("credentials").get<std::string>();
    h.client_id = j.value("client_id", std::string());
    h.device_name = j.value("device_name", std::string("PS5"));
    h.quality = std::clamp(j.value("quality", 2), 0, 2);
    h.level = std::clamp(j.value("level", 10), 0, 10);
    h.follow_phone = j.value("follow_phone", true);
    h.phone_volume = std::clamp(j.value("phone_volume", 65535), 0, 65535);
    h.context_uri = j.value("context_uri", std::string());
    h.tracks = j.value("tracks", std::vector<std::string>());
    h.index = std::max(0, j.value("index", 0));
    h.position_ms = j.value("position_ms", 0u);
    h.paused = j.value("paused", false);
    h.shuffle = j.value("shuffle", false);
    return h;
}

struct Status
{
    std::mutex mutex;
    std::string state = "starting";
    std::string user;
    std::string track;
    std::string artist;
    std::string album;
    std::string cover; // https URL (i.scdn.co)
    std::uint32_t duration = 0;
    bool paused = true;
    unsigned version = 0; // counts changes, so the page knows when to read again
} g_status;

template <typename F> void update_status(F change)
{
    std::lock_guard<std::mutex> lock(g_status.mutex);
    change(g_status);
    g_status.version++;
}

// Counts user actions (pause, play, skip, seek). The page polls /state.png, whose
// width is this count, and reconnects to the stream when it changes: that drops
// the music core's buffer, so the action is heard sooner.
std::atomic<int> g_actions{0};

// ---------------------------------------------------------------- live sink

// Takes cspot's PCM like the app's Player (same track rules), but a clock thread
// moves it at real-time speed into a byte ring that HTTP listeners read. While
// paused or starved it sends silence, so the live "file" never stops.
class LiveSink final
{
  public:
    LiveSink() : pcm_(kRingFrames * 2), out_(kOutRing)
    {
    }

    void start()
    {
        running_ = true;
        std::thread(&LiveSink::run, this).detach();
    }

    // TrackPlayer's data callback. Returns 0 when full ("retry later").
    std::size_t write(const std::uint8_t *data, std::size_t bytes, std::string_view track)
    {
        std::lock_guard<std::mutex> lock(pcm_mutex_);
        if (stream_track_ != track)
        {
            // As the app's Player: one track playing and the next streaming;
            // refuse a third (stale data after a seek or skip).
            if (play_track_ != track && play_track_ != stream_track_)
                return 0;
            stream_track_ = std::string(track);
            markers_.push_back({write_total_, stream_track_});
        }
        const std::size_t count = std::min(bytes / kBytesPerFrame, kRingFrames - frames_);
        if (count == 0)
            return 0;
        std::size_t at = (read_frame_ + frames_) % kRingFrames;
        const auto *samples = reinterpret_cast<const std::int16_t *>(data);
        for (std::size_t i = 0; i < count; i++)
        {
            pcm_[2 * at] = samples[2 * i];
            pcm_[2 * at + 1] = samples[2 * i + 1];
            at = (at + 1) % kRingFrames;
        }
        frames_ += count;
        write_total_ += count;
        return count * kBytesPerFrame;
    }

    // Drops buffered audio (seek, skip, a new context).
    void clear(bool forget_tracks)
    {
        std::lock_guard<std::mutex> lock(pcm_mutex_);
        read_frame_ = 0;
        frames_ = 0;
        read_total_ = write_total_;
        markers_.clear();
        if (forget_tracks)
        {
            stream_track_.clear();
            play_track_.clear();
        }
    }

    void set_paused(bool paused)
    {
        paused_ = paused;
    }
    void set_draining(bool draining)
    {
        draining_ = draining;
    }
    void set_gain(float gain)
    {
        gain_q15_ = static_cast<int>(std::clamp(gain, 0.0f, 1.0f) * 32768.0f);
    }

    // The track whose first audio went into the stream since the last call.
    std::string take_reached()
    {
        std::lock_guard<std::mutex> lock(pcm_mutex_);
        return std::exchange(reached_, std::string());
    }

    bool empty()
    {
        std::lock_guard<std::mutex> lock(pcm_mutex_);
        return frames_ == 0;
    }

    // Position of what is heard: where the track started (or was sought to) plus
    // the audio streamed since, minus what the music core still holds.
    std::uint32_t position_ms(std::uint32_t duration)
    {
        std::lock_guard<std::mutex> lock(pcm_mutex_);
        long long ms = static_cast<long long>(base_ms_) +
                       static_cast<long long>(played_ * 1000 / kRate) - kHeardDelayMs;
        if (duration > 0)
            ms = std::min<long long>(ms, duration);
        return static_cast<std::uint32_t>(std::max<long long>(ms, 0));
    }
    // A seek: the clock restarts there.
    void set_position(std::uint32_t ms)
    {
        std::lock_guard<std::mutex> lock(pcm_mutex_);
        base_ms_ = ms + kHeardDelayMs; // heard after the music core's delay
        played_ = 0;
    }
    // PLAYBACK_START: where the next track starts once it reaches the stream.
    void set_next_start(std::uint32_t ms)
    {
        std::lock_guard<std::mutex> lock(pcm_mutex_);
        next_start_ms_ = ms;
    }

    std::uint64_t underruns() const
    {
        return underruns_.load();
    }
    int listeners() const
    {
        return listeners_.load();
    }

    // Streams the live PCM to one HTTP listener until it disconnects or `limit`
    // bytes are sent. Starts on a chunk boundary `backlog_ms` behind the live point.
    void serve(int fd, long long limit, int backlog_ms)
    {
        const std::uint64_t backlog = static_cast<std::uint64_t>(kBytesPerSecond * backlog_ms / 1000 /
                                                                 kBytesPerFrame * kBytesPerFrame);
        // Only the newest listener is served: the music core keeps reading an old
        // connection for ~10 s after the page opens a new one, and only then starts
        // the new one; closing the old one at once avoids that wait.
        const std::uint64_t mine = ++generation_;
        produced_cv_.notify_all();
        listeners_++;
        std::uint64_t position;
        {
            std::lock_guard<std::mutex> lock(out_mutex_);
            position = produced_;
            for (auto it = chunk_starts_.rbegin(); it != chunk_starts_.rend(); ++it)
            {
                position = *it;
                if (produced_ - *it >= backlog)
                    break;
            }
        }
        long long sent = 0;
        std::vector<std::uint8_t> out(64 * 1024);
        while (sent < limit && running_ && !g_quit && mine == generation_)
        {
            std::size_t count;
            {
                std::unique_lock<std::mutex> lock(out_mutex_);
                produced_cv_.wait_for(lock, std::chrono::seconds(2),
                                      [&] { return produced_ > position || mine != generation_; });
                if (produced_ <= position || mine != generation_)
                    continue;
                if (produced_ - position > kOutRing) // fell too far behind: skip ahead
                    position = produced_ - backlog;
                count = static_cast<std::size_t>(std::min<std::uint64_t>(
                    {produced_ - position, out.size(), static_cast<std::uint64_t>(limit - sent)}));
                for (std::size_t i = 0; i < count; i++)
                    out[i] = out_[(position + i) % kOutRing];
            }
            if (!send_all(fd, out.data(), count))
                break;
            position += count;
            sent += static_cast<long long>(count);
        }
        listeners_--;
        std::printf("[live] listener %llu left after %lld bytes\n", static_cast<unsigned long long>(mine), sent);
    }

  private:
    static constexpr std::size_t kRingFrames = kRate * 4; // 4 s, as the app's Player
    static constexpr std::size_t kOutRing = 4 * 1024 * 1024;

    struct Marker
    {
        std::uint64_t frame; // write_total_ where the track begins
        std::string track;
    };

    // pcm_mutex_ held.
    bool read_frame(std::int16_t frame[2])
    {
        if (frames_ == 0)
            return false;
        while (!markers_.empty() && markers_.front().frame <= read_total_)
        {
            if (markers_.front().track != play_track_)
            {
                play_track_ = markers_.front().track;
                reached_ = play_track_;
                base_ms_ = next_start_ms_; // this track's clock starts here
                next_start_ms_ = 0;
                played_ = 0;
            }
            markers_.pop_front();
        }
        frame[0] = pcm_[2 * read_frame_];
        frame[1] = pcm_[2 * read_frame_ + 1];
        read_frame_ = (read_frame_ + 1) % kRingFrames;
        frames_--;
        read_total_++;
        played_++;
        return true;
    }

    void append(const std::uint8_t *data, std::size_t size)
    {
        {
            std::lock_guard<std::mutex> lock(out_mutex_);
            chunk_starts_.push_back(produced_);
            while (chunk_starts_.size() > 200)
                chunk_starts_.pop_front();
            for (std::size_t i = 0; i < size; i++)
                out_[(produced_ + i) % kOutRing] = data[i];
            produced_ += size;
        }
        produced_cv_.notify_all();
    }

    void run()
    {
        std::vector<std::int16_t> chunk(kChunkFrames * 2);
        auto next = std::chrono::steady_clock::now();
        while (running_ && !g_quit)
        {
            bool silent = true;
            if (!paused_)
            {
                std::lock_guard<std::mutex> lock(pcm_mutex_);
                if (frames_ >= static_cast<std::size_t>(kChunkFrames) || (draining_ && frames_ > 0))
                {
                    silent = false;
                    const int gain = gain_q15_.load();
                    for (int i = 0; i < kChunkFrames; i++)
                    {
                        std::int16_t frame[2] = {0, 0};
                        read_frame(frame);
                        chunk[2 * i] = static_cast<std::int16_t>((frame[0] * gain) >> 15);
                        chunk[2 * i + 1] = static_cast<std::int16_t>((frame[1] * gain) >> 15);
                    }
                }
                else
                {
                    underruns_++;
                }
            }
            if (silent)
                std::fill(chunk.begin(), chunk.end(), 0);
            append(reinterpret_cast<const std::uint8_t *>(chunk.data()), kChunkFrames * kBytesPerFrame);
            // Real-time pace: one chunk per chunk duration; after a stall, no burst.
            const auto now = std::chrono::steady_clock::now();
            next += std::chrono::milliseconds(1000 * kChunkFrames / kRate);
            if (next < now - std::chrono::seconds(1))
                next = now;
            std::this_thread::sleep_until(next);
        }
    }

    std::mutex pcm_mutex_;
    std::vector<std::int16_t> pcm_;
    std::size_t read_frame_ = 0;
    std::size_t frames_ = 0;
    std::uint64_t read_total_ = 0;
    std::uint64_t write_total_ = 0;
    std::deque<Marker> markers_;
    std::string stream_track_, play_track_, reached_;
    std::uint32_t base_ms_ = 0, next_start_ms_ = 0;
    std::uint64_t played_ = 0; // frames of real audio since base_ms_

    std::mutex out_mutex_;
    std::condition_variable produced_cv_;
    std::vector<std::uint8_t> out_;
    std::uint64_t produced_ = 0;
    std::deque<std::uint64_t> chunk_starts_;

    std::atomic<bool> paused_{true};
    std::atomic<bool> draining_{false};
    std::atomic<int> gain_q15_{32768};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<int> listeners_{0};
    std::atomic<std::uint64_t> generation_{0};
};

LiveSink g_sink;

// ---------------------------------------------------------------- song toast

// A system toast with cover, title and artist when a new song is heard (only the
// last one when songs are skipped quickly). The toast takes a file path, so the
// cover is downloaded to kCoverDir; the last few are kept.
class Toaster final
{
  public:
    void start()
    {
        if (DIR *dir = opendir(kCoverDir))
        {
            while (dirent *entry = readdir(dir))
                if (entry->d_name[0] != '.')
                    unlink((std::string(kCoverDir) + "/" + entry->d_name).c_str());
            closedir(dir);
        }
        mkdir(kCoverDir, 0755);
        std::thread(&Toaster::run, this).detach();
    }

    void song_started(const cspot::TrackInfo &track)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = {track.trackId, track.name, track.artist, track.imageUrl};
            due_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(kHeardDelayMs);
            waiting_ = true;
        }
        cv_.notify_all();
    }

  private:
    struct Song
    {
        std::string id, name, artist, cover;
    };

    void run()
    {
        std::string last;
        while (!g_quit)
        {
            Song song;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::seconds(1), [&] { return waiting_; });
                if (!waiting_)
                    continue;
                if (std::chrono::steady_clock::now() < due_)
                {
                    cv_.wait_until(lock, due_); // a newer song may replace this one meanwhile
                    continue;
                }
                song = pending_;
                waiting_ = false;
            }
            if (song.id == last || song.name.empty())
                continue;
            last = song.id;
            const std::string icon = cover_file(song.cover);
            if (icon.empty() || !rich_toast(icon, song.name, song.artist))
                notify("%s - %s", song.name.c_str(), song.artist.c_str());
        }
    }

    std::string cover_file(const std::string &url)
    {
        const std::size_t slash = url.rfind('/');
        if (url.empty() || slash == std::string::npos)
            return "";
        const std::string path = std::string(kCoverDir) + "/" + url.substr(slash + 1) + ".jpg";
        struct stat info;
        if (stat(path.c_str(), &info) == 0 && info.st_size > 0)
            return path;
        try
        {
            const std::vector<std::uint8_t> bytes = bell::HTTPClient::get(url)->bytes();
            if (bytes.size() < 100 || !write_file(path, bytes.data(), bytes.size()))
                return "";
        }
        catch (const std::exception &error)
        {
            std::printf("[toast] cover download failed: %s\n", error.what());
            return "";
        }
        covers_.push_back(path);
        while (covers_.size() > 8)
        {
            unlink(covers_.front().c_str());
            covers_.pop_front();
        }
        return path;
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    Song pending_;
    std::chrono::steady_clock::time_point due_;
    bool waiting_ = false;
    std::deque<std::string> covers_;
};

Toaster g_toaster;

// ---------------------------------------------------------------- engine

// Runs cspot on one thread (as the app's controller does). Other threads hand it
// work through post(), which it runs between packets.
class Engine final
{
  public:
    // HANDOFF: plays `handoff` here. Waits until it plays or failed; returns ""
    // or why it failed.
    std::string take_over(Handoff handoff)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (have_handoff_)
            return "already playing";
        handoff_ = std::move(handoff);
        have_handoff_ = true;
        result_.reset();
        cv_.notify_all();
        cv_.wait_for(lock, std::chrono::seconds(45), [&] { return result_.has_value(); });
        return result_.value_or("timed out signing in");
    }

    // TAKEBACK: what plays now (the session thread fills it in), or what played
    // last while it reconnects. Nothing before a handoff.
    std::optional<Handoff> snapshot()
    {
        auto done = std::make_shared<std::promise<Handoff>>();
        std::future<Handoff> future = done->get_future();
        if (post([this, done] { done->set_value(current()); }) &&
            future.wait_for(std::chrono::seconds(5)) == std::future_status::ready)
            return future.get();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!have_handoff_)
            return std::nullopt;
        return last_;
    }

    // The page's buttons; false if no session runs.
    bool control(const std::string &action)
    {
        return post([this, action] {
            bool paused;
            {
                std::lock_guard<std::mutex> lock(g_status.mutex);
                paused = g_status.paused;
            }
            if (action == "toggle")
                handler_->setPause(!paused);
            else if (action == "next")
                handler_->nextSong();
            else if (action == "prev")
                handler_->previousSong();
        });
    }

    // Runs `work` on the session thread. False if no session runs.
    bool post(std::function<void()> work)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_)
            return false;
        work_.push_back(std::move(work));
        return true;
    }

    void run()
    {
        bell::setDefaultLogger();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            update_status([](Status &s) { s.state = "waiting for Spot5"; });
            cv_.wait(lock, [&] { return have_handoff_ || g_quit.load(); });
        }
        Handoff wanted = handoff_;
        bool first = true;
        while (!g_quit)
        {
            try
            {
                session(wanted, first);
            }
            catch (const std::exception &error)
            {
                std::printf("[engine] session error: %s\n", error.what());
                if (first && !answered())
                {
                    finish(std::string("Spotify: ") + error.what());
                    g_quit = true; // nothing plays here: leave the app in charge
                    return;
                }
                update_status([&](Status &s) { s.state = std::string("error: ") + error.what(); });
            }
            first = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = false;
                work_.clear();
                wanted = last_; // reconnect and carry on where it stopped
            }
            if (!g_quit)
                std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }

  private:
    // Answers the waiting HANDOFF.
    void finish(const std::string &result)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = result;
        cv_.notify_all();
    }

    // The first session played: later errors reconnect instead of giving up.
    bool answered()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return result_.has_value() && result_->empty();
    }

    // What plays now, as the app would hand it over (session thread).
    Handoff current()
    {
        Handoff h = wanted_;
        int index = -1;
        std::vector<std::string> uris;
        for (const cspot::TrackReference &track : handler_->getTrackQueue()->snapshot(index))
        {
            if (!track.uri.empty())
                uris.push_back(track.uri);
            else if (track.gid.size() == 16)
                uris.push_back((track.type == cspot::TrackReference::Type::EPISODE ? "spotify:episode:"
                                                                                    : "spotify:track:") +
                               base62(track.gid));
            else
                uris.emplace_back();
        }
        h.context_uri = handler_->contextUri();
        if (!rest_.empty() && h.context_uri == rest_context_)
            uris.insert(uris.end(), rest_.begin(), rest_.end());
        // An entry without a name would shift the rest: hand over no list (as the app).
        if (std::find(uris.begin(), uris.end(), std::string()) != uris.end())
            uris.clear();
        h.tracks = std::move(uris);
        h.index = std::max(index, 0);
        std::uint32_t duration;
        {
            std::lock_guard<std::mutex> lock(g_status.mutex);
            h.paused = g_status.paused;
            duration = g_status.duration;
        }
        h.position_ms = g_sink.position_ms(duration);
        h.phone_volume = phone_volume_.load();
        return h;
    }

    void apply_gain()
    {
        const float step = static_cast<float>(wanted_.level) / 10.0f;
        const float phone = wanted_.follow_phone ? static_cast<float>(phone_volume_.load()) / 65535.0f : 1.0f;
        g_sink.set_gain(phone * step * step); // as the app's Player
    }

    void session(const Handoff &wanted, bool first)
    {
        wanted_ = wanted;
        phone_volume_ = wanted.phone_volume;
        auto blob = std::make_shared<cspot::LoginBlob>(wanted.device_name);
        blob->loadJson(wanted.credentials);
        auto ctx = cspot::Context::createFromBlob(blob);
        if (!wanted.client_id.empty())
            ctx->config.clientId = wanted.client_id;
        ctx->config.audioFormat = static_cast<AudioFormat>(wanted.quality);
        ctx->config.volume = wanted.phone_volume;
        apply_gain();

        update_status([](Status &s) { s.state = "connecting"; });
        ctx->session->connectWithRandomAp();
        ctx->config.authData = ctx->session->authenticate(blob);
        if (ctx->config.authData.empty())
            throw std::runtime_error("the login was rejected");
        update_status([&](Status &s) {
            s.state = "ready";
            s.user = ctx->config.username;
        });

        handler_ = std::make_shared<cspot::SpircHandler>(ctx);
        ctx->session->startTask();
        handler_->getTrackPlayer()->setDataCallback(
            [](std::uint8_t *data, std::size_t bytes, std::string_view track) {
                return g_sink.write(data, bytes, track);
            });
        std::atomic<bool> depleted{false};
        std::atomic<bool> track_heard{false};
        handler_->setEventHandler([&](std::unique_ptr<cspot::SpircHandler::Event> event) {
            on_event(*event, depleted, track_heard);
        });

        // Start what the app played, at its position (seek once the track is in).
        g_sink.clear(true);
        rest_.clear();
        const std::size_t count = wanted.tracks.size();
        const std::size_t start = std::min<std::size_t>(static_cast<std::size_t>(wanted.index),
                                                        count == 0 ? 0 : count - 1);
        const std::size_t begin = start > 20 ? start - 20 : 0; // Previous still works
        const std::size_t end = std::min(count, begin + kQueueWindow);
        bool loaded = false;
        if (count > 0)
        {
            const std::vector<std::string> window(wanted.tracks.begin() + static_cast<std::ptrdiff_t>(begin),
                                                  wanted.tracks.begin() + static_cast<std::ptrdiff_t>(end));
            rest_.assign(wanted.tracks.begin() + static_cast<std::ptrdiff_t>(end), wanted.tracks.end());
            rest_context_ = wanted.context_uri;
            loaded = handler_->loadContext(wanted.context_uri, window, static_cast<std::uint32_t>(start - begin),
                                           wanted.shuffle);
        }
        std::uint32_t seek_to = loaded ? wanted.position_ms : 0;
        bool pause_after_load = loaded && wanted.paused;
        std::printf("[engine] signed in as %s; %s %zu tracks of %s at %u ms\n", ctx->config.username.c_str(),
                    loaded ? "playing" : "nothing to play,", count, wanted.context_uri.c_str(), seek_to);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = true;
            last_ = wanted;
        }
        if (first)
            finish("");

        // Tells Spotify when audio reaches the stream or runs out.
        std::atomic<bool> running{true};
        std::thread notifier([&] {
            while (running)
            {
                const std::string reached = g_sink.take_reached();
                if (!reached.empty())
                    handler_->notifyAudioReachedPlayback(reached);
                if (depleted && g_sink.empty())
                {
                    depleted = false;
                    g_sink.set_draining(false);
                    handler_->notifyAudioEnded();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

        auto stop = [&] {
            running = false;
            notifier.join();
            handler_->disconnect();
            handler_.reset();
        };
        auto remembered = std::chrono::steady_clock::now();
        try
        {
            while (!g_quit)
            {
                ctx->session->handlePacket(); // waits up to ~200 ms
                if (track_heard.exchange(false))
                {
                    if (seek_to > 0)
                        handler_->seekMs(std::exchange(seek_to, 0));
                    if (pause_after_load)
                    {
                        pause_after_load = false;
                        handler_->setPause(true);
                    }
                }
                std::deque<std::function<void()>> work;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    work.swap(work_);
                }
                for (auto &job : work)
                    job();
                if (!rest_.empty() && handler_->getTrackQueue()->tracksRemaining() <= kQueueLowWater)
                {
                    if (handler_->contextUri() != rest_context_)
                        rest_.clear(); // the phone started something else
                    else
                    {
                        const std::size_t n = std::min(rest_.size(), kQueueWindow);
                        handler_->appendToContext({rest_.begin(), rest_.begin() + static_cast<std::ptrdiff_t>(n)});
                        rest_.erase(rest_.begin(), rest_.begin() + static_cast<std::ptrdiff_t>(n));
                    }
                }
                // Kept for a reconnect, and for TAKEBACK while one runs.
                if (const auto at = std::chrono::steady_clock::now(); at - remembered >= std::chrono::seconds(1))
                {
                    remembered = at;
                    Handoff now = current();
                    std::lock_guard<std::mutex> lock(mutex_);
                    last_ = std::move(now);
                }
            }
        }
        catch (...)
        {
            stop();
            throw;
        }
        stop();
    }

    void on_event(const cspot::SpircHandler::Event &event, std::atomic<bool> &depleted,
                  std::atomic<bool> &track_heard)
    {
        using Type = cspot::SpircHandler::EventType;
        switch (event.eventType)
        {
        case Type::PLAY_PAUSE: {
            const bool paused = std::get<bool>(event.data);
            g_sink.set_paused(paused);
            bool changed = false;
            update_status([&](Status &s) {
                changed = s.paused != paused;
                s.paused = paused;
                s.state = paused ? "paused" : "playing";
            });
            // cspot also sends "playing" after every track load; only a real change
            // may interrupt a seamless track change.
            if (changed)
                g_actions++;
            break;
        }
        case Type::NEXT:
        case Type::PREV:
        case Type::FLUSH:
            g_sink.clear(false);
            g_actions++;
            break;
        case Type::DISC:
            g_sink.clear(false);
            g_actions++;
            update_status([](Status &s) { s.state = "disconnected"; });
            break;
        case Type::SEEK:
            g_sink.clear(false);
            g_sink.set_position(static_cast<std::uint32_t>(std::get<int>(event.data)));
            g_actions++;
            update_status([](Status &) {});
            break;
        case Type::PLAYBACK_START:
            g_sink.set_next_start(static_cast<std::uint32_t>(std::get<int>(event.data)));
            g_sink.set_paused(true); // until the PLAY_PAUSE that follows
            g_sink.set_draining(false);
            depleted = false;
            g_sink.clear(true);
            break;
        case Type::DEPLETED:
            depleted = true;
            g_sink.set_draining(true);
            break;
        case Type::VOLUME:
            phone_volume_ = std::get<int>(event.data);
            apply_gain();
            break;
        case Type::TRACK_INFO: {
            const auto &track = std::get<cspot::TrackInfo>(event.data);
            update_status([&](Status &s) {
                s.track = track.name;
                s.artist = track.artist;
                s.album = track.album;
                s.cover = track.imageUrl;
                s.duration = track.duration;
            });
            g_toaster.song_started(track);
            track_heard = true;
            std::printf("[engine] track: %s - %s\n", track.artist.c_str(), track.name.c_str());
            break;
        }
        default:
            break;
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    Handoff handoff_;
    bool have_handoff_ = false;
    std::optional<std::string> result_;
    bool running_ = false;
    std::deque<std::function<void()>> work_;
    Handoff last_; // what played at the last check (mutex_)

    // Session thread only.
    Handoff wanted_;
    std::shared_ptr<cspot::SpircHandler> handler_;
    std::vector<std::string> rest_; // tracks not queued yet
    std::string rest_context_;
    std::atomic<int> phone_volume_{65535};
};

Engine g_engine;

// ---------------------------------------------------------------- tile

// The page address: kDefaultPageUrl, or an https address in kUrlFile (a fork's
// GitHub Pages, a test server).
std::string page_url()
{
    std::string url = read_file(kUrlFile);
    url.erase(std::find_if(url.begin(), url.end(), [](char c) { return c == '\r' || c == '\n' || c == ' '; }),
              url.end());
    return url.rfind("https://", 0) == 0 ? url : kDefaultPageUrl;
}

std::string tile_param_json(const std::string &url)
{
    return nlohmann::json{{"titleId", kTileId},
                          {"localizedParameters",
                           {{"defaultLanguage", "en-US"}, {"en-US", {{"titleName", kTileName}}}}},
                          {"applicationCategoryType", 66048}, // web app (0x10200)
                          {"downloadDataSize", 256},
                          {"webAppUri", url}}
        .dump(2);
}

int remount_system_ex()
{
    auto entry = [](const char *s) { return iovec{const_cast<char *>(s), s ? std::strlen(s) + 1 : 0}; };
    iovec iov[] = {
        entry("from"),     entry("/dev/ssd0.system_ex"), entry("fspath"),    entry("/system_ex"),
        entry("fstype"),   entry("exfatfs"),             entry("large"),     entry("yes"),
        entry("timezone"), entry("static"),              entry("async"),     entry(nullptr),
        entry("ignoreacl"), entry(nullptr),
    };
    return nmount(iov, sizeof iov / sizeof iov[0], MNT_UPDATE);
}

// Installs the tile the way ps5-payload-dev/svtplay does (and ps5-spotify after
// it): an empty eboot.bin under /system_ex/app, param.json and icon under
// /user/app, then AppInstUtil. Skipped when it is installed with this address.
void ensure_tile()
{
    const std::string url = page_url();
    const std::string param = tile_param_json(url);
    const std::string user_dir = std::string("/user/app/") + kTileId;
    if (read_file((user_dir + "/sce_sys/param.json").c_str()) == param)
        return;
    std::printf("[tile] installing %s -> %s\n", kTileId, url.c_str());

    using Init = int (*)(int);
    using Simple = int (*)();
    using UnInstall = int (*)(const char *);
    auto user_init = system_function<Init>("libSceUserService.sprx", "sceUserServiceInitialize");
    auto inst_init = system_function<Simple>("libSceAppInstUtil.sprx", "sceAppInstUtilInitialize");
    auto uninstall = system_function<UnInstall>("libSceAppInstUtil.sprx", "sceAppInstUtilAppUnInstall");
    auto install_all = system_function<int (*)(void *)>("libSceAppInstUtil.sprx", "sceAppInstUtilAppInstallAll");
    if (inst_init == nullptr)
    {
        notify("Spot5: cannot install the background tile");
        return;
    }
    if (user_init != nullptr)
        user_init(0);
    // websrv and BFplayer install with this authid; ours comes back afterwards.
    const std::uint64_t authid = kernel_get_ucred_authid(-1);
    kernel_set_ucred_authid(-1, 0x4801000000000013L);
    int result = inst_init();
    if (result == 0)
    {
        if (uninstall != nullptr)
            uninstall(kTileId);
        std::printf("[tile] remount /system_ex -> %d\n", remount_system_ex());
        const std::string system_dir = std::string("/system_ex/app/") + kTileId;
        const std::uint8_t *icon = kTileIcon;
        const std::size_t icon_size = static_cast<std::size_t>(kTileIcon_end - kTileIcon);
        mkdir(system_dir.c_str(), 0755);
        mkdir((system_dir + "/sce_sys").c_str(), 0755);
        mkdir(user_dir.c_str(), 0755);
        mkdir((user_dir + "/sce_sys").c_str(), 0755);
        const bool written = write_file(system_dir + "/eboot.bin", nullptr, 0) &&
                             write_file(system_dir + "/sce_sys/param.json", param.data(), param.size()) &&
                             write_file(user_dir + "/sce_sys/icon0.png", icon, icon_size) &&
                             write_file(user_dir + "/sce_sys/param.json", param.data(), param.size());
        if (!written)
            result = -1;
        else
        {
            using InstallDir = int (*)(const char *, const char *, void *);
            InstallDir install_dir = nullptr;
            std::uint32_t handle;
            if (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) == 0)
                install_dir = reinterpret_cast<InstallDir>(kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE"));
            result = install_dir != nullptr ? install_dir(kTileId, "/user/app/", nullptr)
                     : install_all != nullptr ? install_all(nullptr)
                                              : -1;
        }
    }
    kernel_set_ucred_authid(-1, authid);
    std::printf("[tile] install -> 0x%08x\n", static_cast<unsigned>(result));
    if (result != 0)
    {
        unlink((user_dir + "/sce_sys/param.json").c_str()); // try again next time
        notify("Spot5: the background tile could not be installed (0x%08X)", static_cast<unsigned>(result));
    }
}

struct LaunchContext
{
    std::uint32_t size;
    std::uint32_t user_id;
    std::uint32_t app_opt;
    std::uint64_t crash_report;
    std::uint32_t check_flag;
};

// Opens the tile once the app has closed (as websrv's sys_launch_title). If that
// fails the user opens it from the home screen.
void open_tile()
{
    using Foreground = int (*)(std::uint32_t *);
    using RunningBigApp = int (*)();
    using KillApp = int (*)(int, int, int, int);
    using LaunchApp = int (*)(const char *, char **, LaunchContext *);
    auto foreground = system_function<Foreground>("libSceUserService.sprx", "sceUserServiceGetForegroundUser");
    auto big_app =
        system_function<RunningBigApp>("libSceSystemService.sprx", "sceSystemServiceGetAppIdOfRunningBigApp");
    auto kill = system_function<KillApp>("libSceSystemService.sprx", "sceSystemServiceKillApp");
    auto launch = system_function<LaunchApp>("libSceSystemService.sprx", "sceSystemServiceLaunchApp");
    LaunchContext context{};
    context.size = sizeof context;
    if (foreground == nullptr || launch == nullptr || foreground(&context.user_id) != 0)
    {
        notify("Spot5 plays in the background: open \"%s\" to hear it", kTileName);
        return;
    }
    // The app quits by itself after our OK; give it a moment, then make sure.
    for (int i = 0; i < 25 && big_app != nullptr && big_app() > 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (big_app != nullptr && kill != nullptr)
        if (const int app = big_app(); app > 0)
            kill(app, -1, 0, 0);
    char *argv[] = {nullptr};
    const int result = launch(kTileId, argv, &context);
    std::printf("[tile] launch -> 0x%08x\n", static_cast<unsigned>(result));
    if (result < 0)
        notify("Spot5 plays in the background: open \"%s\" to hear it", kTileName);
}

// ---------------------------------------------------------------- live HTTP server

std::uint32_t crc32(const unsigned char *data, std::size_t size)
{
    std::uint32_t crc = ~0u;
    for (std::size_t i = 0; i < size; i++)
    {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// A one-row grey PNG whose pixels are the given bytes (zlib "stored" blocks). The
// https page may load images, not data, from the console: it reads the bytes back
// through a canvas, or only the width.
std::string grey_png(const std::string &pixels)
{
    const std::string raw = std::string(1, '\0') + pixels; // filter byte: none
    std::uint32_t a = 1, b = 0;
    for (unsigned char c : raw)
    {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    std::string z = "\x78\x01";
    for (std::size_t at = 0; at < raw.size(); at += 65535)
    {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        z += static_cast<char>(at + n == raw.size() ? 1 : 0); // final block?
        z += static_cast<char>(n & 0xff);
        z += static_cast<char>(n >> 8);
        z += static_cast<char>(~n & 0xff);
        z += static_cast<char>((~n >> 8) & 0xff);
        z.append(raw, at, n);
    }
    for (int shift = 24; shift >= 0; shift -= 8)
        z += static_cast<char>((((b << 16) | a) >> shift) & 0xff);
    auto be32 = [](std::uint32_t v) {
        std::string out(4, '\0');
        for (int i = 0; i < 4; i++)
            out[static_cast<std::size_t>(i)] = static_cast<char>(v >> (24 - 8 * i));
        return out;
    };
    auto chunk = [&](const char *type, const std::string &data) {
        const std::string body = std::string(type, 4) + data;
        return be32(static_cast<std::uint32_t>(data.size())) + body +
               be32(crc32(reinterpret_cast<const unsigned char *>(body.data()), body.size()));
    };
    std::string header = be32(static_cast<std::uint32_t>(pixels.size())) + be32(1);
    header += std::string("\x08\x00\x00\x00\x00", 5); // 8-bit grey, no interlace
    return std::string("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", header) + chunk("IDAT", z) + chunk("IEND", "");
}

std::string png_of_width(int width)
{
    return grey_png(std::string(static_cast<std::size_t>(std::max(1, width)), '\x80'));
}

// `live` adds the fields that change all the time.
std::string status_json(bool live = true)
{
    nlohmann::json j;
    std::uint32_t duration;
    {
        std::lock_guard<std::mutex> lock(g_status.mutex);
        j["v"] = g_status.version;
        j["state"] = g_status.state;
        j["user"] = g_status.user;
        j["track"] = g_status.track;
        j["artist"] = g_status.artist;
        j["album"] = g_status.album;
        j["cover"] = g_status.cover;
        j["duration"] = duration = g_status.duration;
        j["paused"] = g_status.paused;
    }
    if (live)
    {
        j["position"] = g_sink.position_ms(duration);
        j["listeners"] = g_sink.listeners();
        j["underruns"] = g_sink.underruns();
    }
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

//  /meta.png          all of status_json() as pixels (read through a canvas)
//  /meta.png?stable   the same without the live fields; with &b=<k>, an image whose
//                     width is 1 + byte k (for a page that cannot read pixels)
std::string meta_png(const char *path)
{
    if (std::strstr(path, "stable") == nullptr)
        return grey_png(status_json());
    const std::string text = status_json(false);
    if (const char *b = std::strstr(path, "b="))
    {
        const std::size_t k = static_cast<std::size_t>(std::max(0, std::atoi(b + 2)));
        return png_of_width(1 + (k < text.size() ? static_cast<unsigned char>(text[k]) : 0));
    }
    return grey_png(text);
}

// The page's controller buttons: /control.png?a=toggle|next|prev.
bool control(const char *path)
{
    const char *a = std::strstr(path, "a=");
    if (a == nullptr)
        return false;
    return g_engine.control(std::string(a + 2, std::strcspn(a + 2, "&")));
}

void serve_live(int fd, const char *request, const char *path)
{
    long long start = 0, end = kLiveLength - 1;
    bool ranged = false;
    if (const char *range = strcasestr(request, "\nRange: bytes="))
    {
        ranged = true;
        range += std::strlen("\nRange: bytes=");
        char *dash = nullptr;
        if (*range == '-')
            start = kLiveLength - std::strtoll(range + 1, nullptr, 10);
        else
        {
            start = std::strtoll(range, &dash, 10);
            if (dash != nullptr && *dash == '-' && dash[1] >= '0' && dash[1] <= '9')
                end = std::min(std::strtoll(dash + 1, nullptr, 10), kLiveLength - 1);
        }
        start = std::clamp(start, 0LL, kLiveLength - 1);
    }
    char head[512];
    if (ranged)
        std::snprintf(head, sizeof head,
                      "HTTP/1.1 206 Partial Content\r\nContent-Type: audio/wav\r\nAccept-Ranges: bytes\r\n"
                      "Content-Range: bytes %lld-%lld/%lld\r\nContent-Length: %lld\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                      start, end, kLiveLength, end - start + 1);
    else
        std::snprintf(head, sizeof head,
                      "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nAccept-Ranges: bytes\r\n"
                      "Content-Length: %lld\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                      kLiveLength);
    if (!send_all(fd, head, std::strlen(head)))
        return;
    if (start > 1000000)
    {
        // A peek far into the file (players look for tags at the end): filler.
        static const char zeros[4096] = {};
        for (long long left = end - start + 1; left > 0;)
        {
            const std::size_t n = static_cast<std::size_t>(std::min<long long>(left, sizeof zeros));
            if (!send_all(fd, zeros, n))
                break;
            left -= static_cast<long long>(n);
        }
        return;
    }
    int backlog_ms = kDefaultBacklogMs;
    if (const char *b = std::strstr(path, "backlog="))
        backlog_ms = std::clamp(std::atoi(b + 8), 0, 15000);
    long long limit = end - start + 1;
    if (start < kWavHeaderSize)
    {
        unsigned char header[kWavHeaderSize];
        auto u32 = [&](int at, std::uint32_t v) {
            for (int i = 0; i < 4; i++)
                header[at + i] = static_cast<unsigned char>(v >> (8 * i));
        };
        auto u16 = [&](int at, std::uint16_t v) {
            header[at] = static_cast<unsigned char>(v);
            header[at + 1] = static_cast<unsigned char>(v >> 8);
        };
        std::memcpy(header, "RIFF", 4);
        u32(4, static_cast<std::uint32_t>(36 + kWavDataSize));
        std::memcpy(header + 8, "WAVEfmt ", 8);
        u32(16, 16);
        u16(20, 1); // PCM
        u16(22, 2);
        u32(24, kRate);
        u32(28, static_cast<std::uint32_t>(kBytesPerSecond));
        u16(32, kBytesPerFrame);
        u16(34, 16);
        std::memcpy(header + 36, "data", 4);
        u32(40, static_cast<std::uint32_t>(kWavDataSize));
        const long long n = std::min<long long>(kWavHeaderSize - start, limit);
        if (!send_all(fd, header + start, static_cast<std::size_t>(n)))
            return;
        limit -= n;
    }
    g_sink.serve(fd, limit, backlog_ms);
}

void handle_live_client(int fd)
{
    char request[4096];
    std::size_t length = 0;
    while (length < sizeof request - 1)
    {
        const ssize_t got = recv(fd, request + length, sizeof request - 1 - length, 0);
        if (got <= 0)
            break;
        length += static_cast<std::size_t>(got);
        request[length] = '\0';
        if (std::strstr(request, "\r\n\r\n") != nullptr)
            break;
    }
    request[length] = '\0';
    char method[8] = "", path[256] = "";
    std::sscanf(request, "%7s %255s", method, path);

    if (std::strncmp(path, "/live.wav", 9) == 0)
    {
        serve_live(fd, request, path);
        return;
    }
    std::string body;
    const char *type = "image/png";
    if (std::strncmp(path, "/state.png", 10) == 0)
        body = png_of_width(1 + g_actions.load() % 4000);
    else if (std::strncmp(path, "/meta.png", 9) == 0)
        body = meta_png(path);
    else if (std::strncmp(path, "/ver.png", 8) == 0 || std::strncmp(path, "/pos.png", 8) == 0)
    {
        unsigned version;
        std::uint32_t duration;
        {
            std::lock_guard<std::mutex> lock(g_status.mutex);
            version = g_status.version;
            duration = g_status.duration;
        }
        body = png_of_width(1 + static_cast<int>(path[1] == 'v' ? version % 30000
                                                                : std::min<std::uint32_t>(
                                                                      g_sink.position_ms(duration) / 1000, 30000)));
    }
    else if (std::strncmp(path, "/control.png", 12) == 0)
        body = png_of_width(control(path) ? 2 : 1);
    else if (std::strncmp(path, "/status", 7) == 0)
    {
        body = status_json();
        type = "application/json";
    }
    else if (std::strncmp(path, "/quit", 5) == 0)
    {
        body = "{\"quit\":true}";
        type = "application/json";
        g_quit = true;
    }
    else
    {
        const char *missing = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, missing, std::strlen(missing));
        return;
    }
    char head[256];
    // Allow-Origin lets the page read /meta.png's pixels (a crossOrigin image).
    std::snprintf(head, sizeof head,
                  "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nAccess-Control-Allow-Origin: *\r\n"
                  "Cache-Control: no-store\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                  type, body.size());
    send_all(fd, head, std::strlen(head));
    send_all(fd, body.data(), body.size());
}

void live_server(int listener)
{
    while (!g_quit)
    {
        const int fd = accept(listener, nullptr, nullptr);
        if (fd < 0)
            continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        std::thread([fd] {
            handle_live_client(fd);
            close(fd);
        }).detach();
    }
}

// ---------------------------------------------------------------- control link

void handle_control_client(int fd)
{
    timeval timeout{10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    if (!send_text(fd, "READY\n"))
        return;
    std::string line;
    if (!recv_line(fd, line))
        return;
    if (line.rfind("HANDOFF ", 0) == 0)
    {
        const std::size_t size = static_cast<std::size_t>(std::strtoul(line.c_str() + 8, nullptr, 10));
        std::string text;
        if (size == 0 || size > (4u << 20) || !recv_exact(fd, text, size))
        {
            send_text(fd, "ERR bad request\n");
            return;
        }
        Handoff handoff;
        try
        {
            handoff = parse_handoff(text);
        }
        catch (const std::exception &error)
        {
            send_text(fd, std::string("ERR unreadable handoff (") + error.what() + ")\n");
            return;
        }
        const std::string error = g_engine.take_over(std::move(handoff));
        if (!error.empty())
        {
            send_text(fd, "ERR " + error + "\n");
            return;
        }
        send_text(fd, "OK\n");
        std::thread(open_tile).detach();
        return;
    }
    if (line == "TAKEBACK")
    {
        std::optional<Handoff> state = g_engine.snapshot();
        const std::string json = state ? state_json(*state).dump() : "{}";
        send_text(fd, "STATE " + std::to_string(json.size()) + "\n" + json);
        std::printf("[control] the app took the music back\n");
        g_quit = true; // the app plays from here on
        return;
    }
    send_text(fd, "ERR unknown request\n");
}

void control_server(int listener)
{
    while (!g_quit)
    {
        const int fd = accept(listener, nullptr, nullptr);
        if (fd < 0)
            continue;
        std::thread([fd] {
            handle_control_client(fd);
            close(fd);
        }).detach();
    }
}

// Asks an older copy of this payload to quit, so this one can take the ports.
void stop_previous_instance()
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_len = sizeof address;
    address.sin_family = AF_INET;
    address.sin_port = htons(kLivePort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd >= 0 && connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0)
    {
        send_text(fd, "GET /quit HTTP/1.0\r\n\r\n");
        char buffer[256];
        while (recv(fd, buffer, sizeof buffer, 0) > 0)
        {
        }
        std::printf("[main] asked the previous instance to quit\n");
        close(fd);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return;
    }
    if (fd >= 0)
        close(fd);
}

} // namespace

int main()
{
    signal(SIGPIPE, SIG_IGN);
    mkdir(kDataDir, 0755);
    rename(kLogPath, kLogPrevious);
    if (std::freopen(kLogPath, "w", stdout) != nullptr)
    {
        setvbuf(stdout, nullptr, _IOLBF, 0);
        dup2(fileno(stdout), fileno(stderr));
    }
    std::printf("[main] Spot5 background player starting\n");

    try
    {
        bell::X509Bundle::init(kCertBundle, static_cast<std::size_t>(kCertBundle_end - kCertBundle));
    }
    catch (const std::exception &error)
    {
        std::printf("[main] certificate bundle unusable: %s\n", error.what());
        return 1;
    }

    stop_previous_instance();
    const int live = open_listener(kLivePort, false);
    const int control = open_listener(kControlPort, true);
    if (live < 0 || control < 0)
    {
        notify("Spot5 background: port %d busy (errno %d)", live < 0 ? kLivePort : kControlPort, errno);
        return 1;
    }

    g_notification_send = system_function<NotificationSend>("libSceNotification.sprx", "sceNotificationSend");
    g_sink.start();
    g_toaster.start();
    std::thread(live_server, live).detach();
    std::thread(control_server, control).detach();
    std::thread([] { g_engine.run(); }).detach();
    std::thread(ensure_tile).detach();

    // Nobody handed anything over within a minute: an accidental start.
    const auto started = std::chrono::steady_clock::now();
    while (!g_quit)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::lock_guard<std::mutex> lock(g_status.mutex);
        if (g_status.state == "waiting for Spot5" &&
            std::chrono::steady_clock::now() - started > std::chrono::minutes(1))
            g_quit = true;
    }
    std::printf("[main] quitting\n");
    close(live);
    close(control);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    _exit(0);
}
