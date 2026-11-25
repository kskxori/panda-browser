#pragma once

#include "content_blocker.hpp"
#include "profile_store.hpp"

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace panda {

inline constexpr const char* kPandaHomeUri = "panda://home";
inline constexpr const char* kPandaHistoryUri = "panda://history";
inline constexpr const char* kPandaBookmarksUri = "panda://bookmarks";
inline constexpr const char* kPandaPasswordsUri = "panda://passwords";

struct BrowserWindow;

struct DownloadRecord {
    uint64_t id = 0;
    std::string filename;
    std::string dest_path;
    std::string error_msg;
    int progress_pct = 0;
    bool finished = false;
    bool failed = false;
};

struct TabData {
    BrowserWindow* win = nullptr;
    GtkWidget* stack = nullptr;          // Notebook page container ("home", "history", "bookmarks", "passwords", "web")
    GtkWidget* web_view = nullptr;       // Created lazily on first real URL navigation
    GtkWidget* home_view = nullptr;
    GtkWidget* home_tiles_grid = nullptr;
    GtkWidget* home_stats_lbl = nullptr;
    GtkWidget* home_search_entry = nullptr;
    GtkWidget* history_view = nullptr;
    GtkWidget* history_list = nullptr;
    GtkWidget* history_search = nullptr;
    GtkWidget* bookmarks_view = nullptr;
    GtkWidget* bookmarks_list = nullptr;
    GtkWidget* passwords_view = nullptr;
    GtkWidget* passwords_list = nullptr;
    GtkWidget* passwords_search = nullptr;

    GtkWidget* tab_label = nullptr;
    GtkWidget* audio_icon = nullptr;
    gchar* suspended_url = nullptr;
    gint64 last_active_sec = 0;
    bool suspended = false;
    bool pinned = false;

    std::string internal_uri;      // Non-empty when displaying a native GTK page
    std::string prev_internal_uri; // Native page before entering "web", for smooth Back button
    bool forward_to_web = false;
    std::string last_applied_host; // Tracks host whose per-site shield/dark/zoom was applied
};

struct BrowserWindow {
    uint32_t max_pages = 64;
    bool is_private = false;

    ProfileStore store;
    BrowserConfig config;
    std::vector<BookmarkItem> bookmarks;
    std::vector<HistoryItem> history;
    std::vector<PasswordEntry> passwords;
    std::vector<std::string> never_save_origins;
    std::vector<SitePref> site_prefs;
    std::vector<std::string> closed_tabs;
    std::vector<DownloadRecord> downloads;
    uint64_t next_download_id = 1;
    ContentBlocker blocker;
    MemoryStats cached_mem;

    WebKitWebContext* web_context = nullptr;
    WebKitUserScript* pass_script = nullptr;

    GtkWidget* window = nullptr;
    GtkWidget* notebook = nullptr;
    GtkWidget* back_btn = nullptr;
    GtkWidget* fwd_btn = nullptr;
    GtkWidget* reload_btn = nullptr;
    GtkWidget* reload_icon = nullptr;
    GtkWidget* home_btn = nullptr;
    GtkWidget* bookmark_btn = nullptr;
    GtkWidget* pass_btn = nullptr;
    GtkWidget* shield_btn = nullptr;
    GtkWidget* dark_btn = nullptr;
    GtkWidget* menu_btn = nullptr;
    GtkWidget* url_entry = nullptr;
    GtkListStore* url_completion_store = nullptr;

    GtkWidget* bookmarks_bar = nullptr;
    GtkWidget* pass_bar = nullptr;
    GtkWidget* pass_bar_label = nullptr;
    std::string pending_origin;
    std::string pending_user;
    std::string pending_secret;

    GtkWidget* find_revealer = nullptr;
    GtkWidget* find_entry = nullptr;
    GtkWidget* status_label = nullptr;
    GtkWidget* downloads_btn = nullptr;
    GtkWidget* downloads_popover = nullptr;
    GtkWidget* downloads_list = nullptr;
    GtkWidget* ram_label = nullptr;

    guint mem_timer_id = 0;
    guint hibernate_timer_id = 0;
};

// Shared utility helpers (browser_ui.cpp)
gint64 monotonic_seconds() noexcept;
bool ascii_iequals(std::string_view a, std::string_view b) noexcept;
bool starts_with_ci(std::string_view s, std::string_view prefix) noexcept;
std::string extract_origin(const char* uri);
std::string extract_host(const char* uri);
std::string format_timestamp(int64_t ts);
void clear_container_children(GtkWidget* container);

// Per-site preference helpers (browser_ui.cpp)
SitePref* find_site_pref(BrowserWindow* win, std::string_view host);
SitePref& upsert_site_pref(BrowserWindow* win, const std::string& host);
bool effective_shield_for_host(const BrowserWindow* win, std::string_view host);
bool effective_dark_for_host(const BrowserWindow* win, std::string_view host);
double effective_zoom_for_host(const BrowserWindow* win, std::string_view host);
void apply_site_prefs_to_tab(TabData* td);

// Core navigation & UI helpers (browser_ui.cpp)
TabData* get_tab_data_from_page(GtkWidget* page);
TabData* get_tab_data(WebKitWebView* view);
TabData* active_tab(BrowserWindow* win);
WebKitWebView* active_web_view(BrowserWindow* win);
void set_status_message(BrowserWindow* win, const char* msg);
void update_tab_label_text(TabData* td, const char* title);
void sync_toolbar(BrowserWindow* win);
void load_user_input(TabData* td, const char* raw);
GtkWidget* open_tab(
    BrowserWindow* win,
    const char* url,
    WebKitWebView* related_view,
    bool switch_to_tab,
    bool pinned = false,
    bool start_suspended = false,
    const char* suspended_title = nullptr
);
void open_browser_window(const char* initial_url, uint32_t max_pages, bool is_private);
void rebuild_bookmarks_bar(BrowserWindow* win);
void refresh_url_completions(BrowserWindow* win);
void sleep_all_background_tabs(BrowserWindow* win);
void apply_settings_to_view(BrowserWindow* win, WebKitWebView* view);
void set_active_tab_zoom(BrowserWindow* win, double zoom);

// Native panda:// pages (browser_pages.cpp)
void build_home_view(TabData* td);
void refresh_home_view(TabData* td);
void build_history_view(TabData* td);
void refresh_history_view(TabData* td);
void build_bookmarks_view(TabData* td);
void refresh_bookmarks_view(TabData* td);
void build_passwords_view(TabData* td);
void refresh_passwords_view(TabData* td);
void show_internal_view(TabData* td, std::string_view uri);
bool handle_internal_uri(TabData* td, std::string_view sv);

// Dialogs, popovers & print (browser_dialogs.cpp)
void print_active_tab(BrowserWindow* win);
void show_site_info_popover(BrowserWindow* win);
void refresh_downloads_popover(BrowserWindow* win);
void show_downloads_popover(BrowserWindow* win);
void show_clear_data_dialog(BrowserWindow* win);
void show_preferences_dialog(BrowserWindow* win);
void show_command_palette(BrowserWindow* win);

} // namespace panda
