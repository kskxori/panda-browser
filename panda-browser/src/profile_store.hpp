#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace panda {

enum class SearchEngine : int {
    DuckDuckGo = 0,
    Startpage = 1,
    Brave = 2,
    Google = 3,
    Wikipedia = 4,
};

enum class CacheProfile : int {
    UltraLowRam = 0, // WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER
    Balanced = 1,    // WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER
    Fast = 2,        // WEBKIT_CACHE_MODEL_WEB_BROWSER
};

enum class GpuPolicy : int {
    OnDemand = 0,     // Near-0 GPU idle; enables WebGL/video on demand
    Always = 1,       // Always hardware-accelerated
    BatterySaver = 2, // Software rendering (0 GPU VRAM)
};

enum class UserAgentPreset : int {
    SafariWebKit = 0,
    FirefoxLinux = 1,
    ChromeLinux = 2,
};

struct BrowserConfig {
    SearchEngine search_engine = SearchEngine::DuckDuckGo;
    CacheProfile cache_profile = CacheProfile::UltraLowRam;
    GpuPolicy gpu_policy = GpuPolicy::OnDemand;
    UserAgentPreset ua_preset = UserAgentPreset::SafariWebKit;
    bool shield_enabled = true;
    bool force_dark = false;
    bool show_bookmarks_bar = true;
    bool restore_session = true;
    int hibernate_minutes = 5; // 0 = disabled
    std::string home_url = "panda://home";
};

struct BookmarkItem {
    std::string title;
    std::string url;
    bool pinned = false;
};

struct HistoryItem {
    std::string title;
    std::string url;
    int64_t timestamp = 0;
};

struct SessionTab {
    std::string title;
    std::string url;
    bool pinned = false;
};

struct MemoryStats {
    uint64_t ui_rss_kb = 0;
    uint64_t total_rss_kb = 0;
    uint32_t web_process_count = 0;
};

class ProfileStore {
public:
    ProfileStore();
    ~ProfileStore() = default;

    const std::string& config_dir() const noexcept { return config_dir_; }
    const std::string& data_dir() const noexcept { return data_dir_; }
    const std::string& cache_dir() const noexcept { return cache_dir_; }

    BrowserConfig load_config() const;
    void save_config(const BrowserConfig& cfg) const;

    std::vector<BookmarkItem> load_bookmarks() const;
    void save_bookmarks(const std::vector<BookmarkItem>& items) const;

    std::vector<HistoryItem> load_history() const;
    void append_history(const std::string& title, const std::string& url) const;
    void clear_history() const;

    std::vector<SessionTab> load_session() const;
    void save_session(const std::vector<SessionTab>& tabs) const;

    std::string read_optional_file(const std::string& path) const;
    std::string build_search_url(SearchEngine engine, const std::string& query) const;
    const char* user_agent_string(UserAgentPreset preset) const noexcept;

    MemoryStats sample_memory_stats() const;

private:
    std::string config_dir_;
    std::string data_dir_;
    std::string cache_dir_;
};

} // namespace panda
