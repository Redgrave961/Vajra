#include "presence.h"
#include "rl_achievements.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <windows.h>
#else
#include <cstdlib>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
#endif

namespace {

constexpr const char* k_discord_app_id = "1574571133653491862";
constexpr const char* k_large_image = "vajra";
constexpr const char* k_large_text = "Asura's Wrath";

struct episode_info {
    const char* code;
    const char* details;
    const char* state;
};

constexpr episode_info k_episodes[] = {
    { "BC-S000", "Episode 1: The Coming of a New Dawn", "Part I: Suffering" },
    { "BC-S010", "Episode 2: Betrayal and Vengeance", "Part I: Suffering" },
    { "BC-S020", "Episode 3: Hell on Gaea", "Part I: Suffering" },
    { "BC-S030", "Episode 4: Old Friends, New Enemies", "Part I: Suffering" },
    { "BC-S040", "Episode 5: Hollow Victory", "Part I: Suffering" },
    { "BC-S050", "Episode 6: Confessions of a Mask", "Part I: Suffering" },
    { "BC-S060", "Episode 7: A Father's Duty", "Part II: Rebirth" },
    { "BC-S070", "Episode 8: Willing Sacrifices", "Part II: Rebirth" },
    { "BC-S080", "Episode 9: The Best Laid Plans", "Part II: Rebirth" },
    { "BC-S100", "Episode 10: Words of Wisdom", "Part II: Rebirth" },
    { "BC-S110", "Episode 11: The Final Lesson", "Part II: Rebirth" },
    { "BC-S120", "Episode 11.5: Forging Ahead", "Interlude DLC" },
    { "BC-S130", "Episode 12: Gods of Death", "Part II: Rebirth" },
    { "BC-S140", "Episode 13: Cause and Effect", "Part III: Karma" },
    { "BC-S150", "Episode 14: Gods and Men", "Part III: Karma" },
    { "BC-S160", "Episode 15: A New Cause", "Part III: Karma" },
    { "BC-S170", "Episode 15.5: Defiance", "Interlude DLC" },
    { "BC-S180", "Episode 16: Power Struggle", "Part III: Karma" },
    { "BC-S190", "Episode 17: Feet of Clay", "Part III: Karma" },
    { "BC-S200", "Episode 18: The Breaking Point", "Part III: Karma" },
    { "BC-S205", "Episode 18: A Turn of Events", "Part III: Karma" },
    { "BC-S210", "Episode 19: The One Behind the Curtain", "Part IV: Nirvana" },
    { "BC-S220", "Episode 20: The Key to Victory", "Part IV: Nirvana" },
    { "BC-S230", "Episode 21: A Rude Awakening", "Part IV: Nirvana" },
    { "BC-S250", "Episode 22: A Life Well Lived", "Part IV: Nirvana" },
    { "BC-S260", "Lost Episode 1: At Last, Someone Angrier Than Me", "Lost Episode DLC" },
    { "BC-S270", "Lost Episode 2: The Strongest vs. the Angriest", "Lost Episode DLC" },
};

std::string json_escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back((char)c);
        } else if (c >= 0x20) {
            out.push_back((char)c);
        }
    }
    return out;
}

class discord_ipc {
public:
    ~discord_ipc() { close(); }

    bool connected() const
    {
#ifdef _WIN32
        return pipe != INVALID_HANDLE_VALUE;
#else
        return fd >= 0;
#endif
    }

    bool connect(const char* app_id, const std::atomic<bool>& running)
    {
        if (connected()) return true;
#ifdef _WIN32
        for (int i = 0; i < 10 && running.load(std::memory_order_relaxed); i++) {
            char name[64];
            std::snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
            HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                pipe = h;
                break;
            }
        }
#else
        std::vector<std::string> dirs;
        auto push_unique = [&](std::string d) {
            if (!d.empty() && std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(std::move(d));
        };
        auto add_dir = [&](const char* env) {
            if (const char* v = std::getenv(env); v && *v) {
                push_unique(v);
                push_unique(std::string(v) + "/app/com.discordapp.Discord");
                push_unique(std::string(v) + "/snap.discord");
            }
        };
        add_dir("XDG_RUNTIME_DIR");
        add_dir("TMPDIR");
        add_dir("TMP");
        add_dir("TEMP");
        push_unique("/tmp");

        for (const std::string& d : dirs) {
            for (int i = 0; i < 10 && running.load(std::memory_order_relaxed); i++) {
                std::string path = d + "/discord-ipc-" + std::to_string(i);
                struct sockaddr_un sa {};
                if (path.size() >= sizeof(sa.sun_path) || access(path.c_str(), F_OK) != 0) continue;
                int s = socket(AF_UNIX, SOCK_STREAM, 0);
                if (s < 0) continue;
                sa.sun_family = AF_UNIX;
                std::strcpy(sa.sun_path, path.c_str());
                if (::connect(s, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa) == 0) {
                    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
                    fd = s;
                    break;
                }
                ::close(s);
            }
            if (fd >= 0) break;
        }
#endif
        if (!connected()) return false;
        const std::string hs = std::string("{\"v\":1,\"client_id\":\"") + app_id + "\"}";
        if (!write_frame(0, hs, running)) {
            close();
            return false;
        }
        // Wait briefly for Discord's READY frame before sending SET_ACTIVITY
        for (int tries = 0; tries < 25 && connected() && running.load(std::memory_order_relaxed); tries++) {
            if (drain_once() > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        drain();
        return connected();
    }

    bool send_activity(const std::string& details, const std::string& state, int64_t start_ts, uint32_t seq, const std::atomic<bool>& running)
    {
        if (!connected()) return false;
        drain();
        if (!connected()) return false;
#ifdef _WIN32
        const unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
        const unsigned long pid = (unsigned long)getpid();
#endif
        std::string payload = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) + ",\"activity\":{";
        payload += "\"details\":\"" + json_escape(details) + "\"";
        if (!state.empty()) payload += ",\"state\":\"" + json_escape(state) + "\"";
        payload += ",\"timestamps\":{\"start\":" + std::to_string(start_ts) + "}";
        payload += ",\"assets\":{\"large_image\":\"" + std::string(k_large_image) + "\",\"large_text\":\"" + std::string(k_large_text) + "\"}";
        payload += "}},\"nonce\":\"" + std::to_string(seq) + "\"}";
        if (!write_frame(1, payload, running)) {
            close();
            return false;
        }
        drain();
        return connected();
    }

    void close()
    {
#ifdef _WIN32
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
#else
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
#endif
    }

private:
    bool write_all(const void* data, size_t n, const std::atomic<bool>& running)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
#ifdef _WIN32
        (void)running;
        while (n > 0) {
            DWORD w = 0;
            if (!WriteFile(pipe, p, (DWORD)n, &w, nullptr) || w == 0) return false;
            p += w;
            n -= w;
        }
        return true;
#else
        int retries = 0;
        while (n > 0) {
            ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
            if (w > 0) {
                p += w;
                n -= (size_t)w;
                retries = 0;
                continue;
            }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                if (!running.load(std::memory_order_relaxed) || ++retries > 100) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            return false;
        }
        return true;
#endif
    }

    bool write_frame(uint32_t op, const std::string& json, const std::atomic<bool>& running)
    {
        uint32_t hdr[2] = { op, (uint32_t)json.size() };
        return write_all(hdr, sizeof hdr, running) && write_all(json.data(), json.size(), running);
    }

    int drain_once()
    {
        char buf[4096];
#ifdef _WIN32
        DWORD avail = 0, got = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
            close();
            return -1;
        }
        if (avail == 0) return 0;
        DWORD want = avail > sizeof buf ? (DWORD)sizeof buf : avail;
        if (!ReadFile(pipe, buf, want, &got, nullptr) || got == 0) {
            close();
            return -1;
        }
        return (int)got;
#else
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r > 0) return (int)r;
        if (r == 0) {
            close();
            return -1;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        close();
        return -1;
#endif
    }

    void drain()
    {
        while (connected() && drain_once() > 0) {}
    }

#ifdef _WIN32
    HANDLE pipe = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
};

std::mutex g_lock;
std::condition_variable g_cv;
std::thread g_worker;
std::atomic<bool> g_running{ false };
bool g_dirty = true;
int64_t g_start_ts = 0;
std::string g_details;
std::string g_state;
std::string g_last_arc;

void update_rl_presence_locked()
{
    const std::string line = g_state.empty() ? g_details : (g_details + " - " + g_state);
    rl_ach_presence(line.c_str());
}

void set_activity_locked(std::string details, std::string state)
{
    if (details == g_details && state == g_state) return;
    g_details = std::move(details);
    g_state = std::move(state);
    g_dirty = true;
    update_rl_presence_locked();
    g_cv.notify_one();
}

void worker_loop()
{
    discord_ipc ipc;
    uint32_t seq = 1;
    std::unique_lock<std::mutex> lk(g_lock);
    while (g_running.load(std::memory_order_relaxed)) {
        if (!ipc.connected()) {
            lk.unlock();
            const bool ok = ipc.connect(k_discord_app_id, g_running);
            lk.lock();
            if (ok) g_dirty = true;
        }
        if (ipc.connected() && g_dirty) {
            const std::string d = g_details, s = g_state;
            const int64_t ts = g_start_ts;
            g_dirty = false;
            lk.unlock();
            if (!ipc.send_activity(d, s, ts, seq++, g_running)) {
                lk.lock();
                g_dirty = true;
                continue;
            }
            lk.lock();
        }
        g_cv.wait_for(lk, std::chrono::seconds(ipc.connected() ? 5 : 15),
                      [&] { return !g_running.load(std::memory_order_relaxed) || (g_dirty && ipc.connected()); });
    }
    ipc.close();
}

}

void presence_init()
{
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_running.load(std::memory_order_relaxed)) return;
    g_start_ts = (int64_t)std::time(nullptr);
    g_details = "Main Menu";
    g_state.clear();
    g_dirty = true;
    g_running.store(true, std::memory_order_release);
    update_rl_presence_locked();
    g_worker = std::thread(worker_loop);
}

void presence_on_file_open(const char* guest_path)
{
    if (!guest_path) return;
    const char* slash = std::strrchr(guest_path, '/');
    const char* base = slash ? slash + 1 : guest_path;
    const size_t len = std::strlen(base);
    if (len < 8) return;
    if ((base[0] != 'B' && base[0] != 'b') || (base[1] != 'C' && base[1] != 'c') || base[2] != '-') return;
    if (std::tolower((unsigned char)base[len - 4]) != '.' || std::tolower((unsigned char)base[len - 3]) != 'x' ||
        std::tolower((unsigned char)base[len - 2]) != 'x' || std::tolower((unsigned char)base[len - 1]) != 'x')
        return;

    // Extract uppercase package stem before ".XXX"
    char stem[64];
    const size_t stem_len = std::min(len - 4, sizeof(stem) - 1);
    for (size_t i = 0; i < stem_len; i++) stem[i] = (char)std::toupper((unsigned char)base[i]);
    stem[stem_len] = 0;

    // Only react to top-level map packages (no underscore sub-chunks like _LOC_INT or _L01_...)
    if (std::strchr(stem, '_')) return;

    std::lock_guard<std::mutex> lk(g_lock);
    if (!std::strcmp(stem, "BC-FRONTEND") || !std::strcmp(stem, "BC-MENU")) {
        set_activity_locked("Main Menu", {});
        return;
    }
    if (!std::strcmp(stem, "BC-CHAPTERMENU")) {
        set_activity_locked("Episode Select", {});
        return;
    }
    if (!std::strcmp(stem, "BC-COLLECTION")) {
        set_activity_locked("Collection & Extras", {});
        return;
    }
    if (!std::strcmp(stem, "BC-RESULT")) {
        set_activity_locked("Episode Results", g_last_arc);
        return;
    }
    if (!std::strcmp(stem, "BC-ENDCREDIT")) {
        set_activity_locked("End Credits", "Asura's Wrath");
        return;
    }
    for (const episode_info& ep : k_episodes) {
        if (!std::strcmp(stem, ep.code)) {
            g_last_arc = ep.state;
            set_activity_locked(ep.details, ep.state);
            return;
        }
    }
}

void presence_shutdown()
{
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g_running.load(std::memory_order_relaxed)) return;
        g_running.store(false, std::memory_order_release);
        g_cv.notify_all();
    }
    if (g_worker.joinable()) g_worker.join();
}
