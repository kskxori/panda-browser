#include "browser_ui.hpp"
#include "content_blocker.hpp"
#include "profile_store.hpp"

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace panda {

namespace {

constexpr const char* kAppVersion = "2.00.00";
constexpr const char* kPandaHomeUri = "panda://home";
constexpr const char* kPandaHistoryUri = "panda://history";
constexpr const char* kPandaBookmarksUri = "panda://bookmarks";
constexpr const char* kPandaPasswordsUri = "panda://passwords";

constexpr const char* kPasswordHelperJs = R"JS(
(() => {
  if (window.__pandaPassHooked) return;
  window.__pandaPassHooked = true;
  const getOrigin = () => {
    try { return location.origin || ''; } catch (_) { return ''; }
  };
  const findUsername = (root, pwEl) => {
    const inputs = Array.from((root || document).querySelectorAll('input'));
    const pwIdx = inputs.indexOf(pwEl);
    let best = '';
    for (let i = (pwIdx >= 0 ? pwIdx - 1 : inputs.length - 1); i >= 0; --i) {
      const el = inputs[i];
      if (el.offsetParent === null && el.type !== 'hidden') continue;
      const t = (el.type || 'text').toLowerCase();
      if (t === 'email' || t === 'text' || t === 'tel') {
        const v = (el.value || '').trim();
        if (v) { best = v; break; }
      }
    }
    if (!best) {
      try { best = sessionStorage.getItem('__panda_last_user') || ''; } catch (_) {}
    }
    return best;
  };
  document.addEventListener('input', (e) => {
    const el = e.target;
    if (el && el.tagName === 'INPUT') {
      const t = (el.type || 'text').toLowerCase();
      if ((t === 'email' || t === 'text' || t === 'tel') && el.value && el.value.trim().length > 0 && el.value.length < 200) {
        try { sessionStorage.setItem('__panda_last_user', el.value.trim()); } catch (_) {}
      }
    }
  }, true);
  const tryCapture = (scope) => {
    const pws = Array.from((scope || document).querySelectorAll('input[type="password"]'));
    for (const pw of pws) {
      const pass = pw.value || '';
      if (!pass) continue;
      const origin = getOrigin();
      if (!origin.startsWith('http://') && !origin.startsWith('https://')) continue;
      const user = findUsername(pw.form || scope || document, pw);
      try {
        if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.pandaPass) {
          window.webkit.messageHandlers.pandaPass.postMessage(JSON.stringify({
            type: 'capture',
            origin: origin,
            username: user,
            password: pass
          }));
          return;
        }
      } catch (_) {}
    }
  };
  document.addEventListener('submit', (e) => tryCapture(e.target), true);
  document.addEventListener('click', (e) => {
    const btn = e.target && e.target.closest ? e.target.closest('button, input[type="submit"], [role="button"]') : null;
    if (btn) tryCapture(btn.closest('form') || document);
  }, true);
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && e.target && e.target.tagName === 'INPUT' && (e.target.type || '').toLowerCase() === 'password') {
      tryCapture(e.target.closest('form') || document);
    }
  }, true);
})();
)JS";

int g_open_windows = 0;

struct BrowserWindow;

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
    std::vector<std::string> closed_tabs;
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
    GtkWidget* ram_label = nullptr;

    guint mem_timer_id = 0;
    guint hibernate_timer_id = 0;
};

// Forward declarations
GtkWidget* open_tab(BrowserWindow* win, const char* url, WebKitWebView* related_view, bool switch_to_tab, bool pinned = false, bool start_suspended = false, const char* suspended_title = nullptr);
void open_browser_window(const char* initial_url, uint32_t max_pages, bool is_private);
void load_user_input(TabData* td, const char* raw);
void sync_toolbar(BrowserWindow* win);
void rebuild_bookmarks_bar(BrowserWindow* win);
void refresh_url_completions(BrowserWindow* win);
void update_ram_indicator(BrowserWindow* win);
void show_preferences_dialog(BrowserWindow* win);
void show_command_palette(BrowserWindow* win);
WebKitWebView* ensure_web_view(TabData* td, WebKitWebView* related_view);
void show_internal_view(TabData* td, std::string_view uri);

gint64 monotonic_seconds() noexcept {
    return g_get_monotonic_time() / G_USEC_PER_SEC;
}

bool ascii_iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
        char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
        if (ca != cb) return false;
    }
    return true;
}

bool starts_with_ci(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && ascii_iequals(s.substr(0, prefix.size()), prefix);
}

TabData* get_tab_data_from_page(GtkWidget* page) {
    return page ? static_cast<TabData*>(g_object_get_data(G_OBJECT(page), "panda-tab")) : nullptr;
}

TabData* get_tab_data(WebKitWebView* view) {
    return view ? static_cast<TabData*>(g_object_get_data(G_OBJECT(view), "panda-tab")) : nullptr;
}

TabData* active_tab(BrowserWindow* win) {
    if (!win || !win->notebook) return nullptr;
    const gint page = gtk_notebook_get_current_page(GTK_NOTEBOOK(win->notebook));
    if (page < 0) return nullptr;
    return get_tab_data_from_page(gtk_notebook_get_nth_page(GTK_NOTEBOOK(win->notebook), page));
}

WebKitWebView* active_web_view(BrowserWindow* win) {
    TabData* td = active_tab(win);
    if (!td || !td->internal_uri.empty() || !td->web_view) {
        return nullptr;
    }
    return WEBKIT_WEB_VIEW(td->web_view);
}

void set_status_message(BrowserWindow* win, const char* msg) {
    if (!win || !win->status_label) return;
    gtk_label_set_text(GTK_LABEL(win->status_label), (msg && msg[0] != '\0') ? msg : "Ready");
}

gchar* escape_html(const char* input) {
    return input ? g_markup_escape_text(input, -1) : g_strdup("");
}

std::string extract_origin(const char* uri) {
    if (!uri) return {};
    std::string_view sv(uri);
    const size_t scheme_end = sv.find("://");
    if (scheme_end == std::string_view::npos) return {};
    const std::string_view scheme = sv.substr(0, scheme_end);
    if (!ascii_iequals(scheme, "http") && !ascii_iequals(scheme, "https")) return {};

    size_t host_start = scheme_end + 3;
    size_t path_start = sv.find('/', host_start);
    if (path_start == std::string_view::npos) path_start = sv.size();
    std::string_view authority = sv.substr(host_start, path_start - host_start);
    if (authority.empty()) return {};

    std::string out;
    out.reserve(scheme.size() + 3 + authority.size());
    for (char c : scheme) {
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
    }
    out += "://";
    for (char c : authority) {
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
    }
    return out;
}

std::string js_quote(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() + 8);
    out.push_back('"');
    for (unsigned char c : sv) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '<':  out += "\\u003C"; break;
            case '>':  out += "\\u003E"; break;
            case '&':  out += "\\u0026"; break;
            default:
                if (c < 0x20) {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "\\u%04x", c);
                    out += hex;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

bool json_extract_string(std::string_view json, std::string_view key, std::string& out) {
    std::string needle = "\"";
    needle.append(key);
    needle.append("\":\"");

    const size_t pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return false;
    }

    size_t i = pos + needle.size();
    out.clear();
    while (i < json.size()) {
        const char ch = json[i++];
        if (ch == '"') {
            return true;
        }
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }
        if (i >= json.size()) return false;
        const char esc = json[i++];
        switch (esc) {
            case '"':  out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/'); break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u':
                if (i + 4 <= json.size()) {
                    unsigned int cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = json[i + k];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned int>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned int>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned int>(h - 'A' + 10);
                    }
                    i += 4;
                    if (cp < 0x80) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
                    } else {
                        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
                    }
                }
                break;
            default:
                out.push_back(esc);
                break;
        }
    }
    return false;
}

std::string format_timestamp(int64_t ts) {
    if (ts <= 0) return "";
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm_val{};
    if (!localtime_r(&t, &tm_val)) return "";
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_val);
    return buf;
}

void clear_container_children(GtkWidget* container) {
    if (!container) return;
    GList* children = gtk_container_get_children(GTK_CONTAINER(container));
    for (GList* l = children; l != nullptr; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);
}

void update_tab_label_text(TabData* td, const char* title) {
    if (!td || !td->tab_label) return;
    std::string text;
    if (td->pinned) text += "[*] ";
    if (td->suspended) text += "[zzz] ";
    text += (title && title[0] != '\0') ? title : "New Tab";
    gtk_label_set_text(GTK_LABEL(td->tab_label), text.c_str());
}

void update_tab_audio_icon(TabData* td, WebKitWebView* view) {
    if (!td || !td->audio_icon || !view) return;
    const gboolean muted = webkit_web_view_get_is_muted(view);
    const gboolean playing = webkit_web_view_is_playing_audio(view);
    if (muted) {
        gtk_image_set_from_icon_name(GTK_IMAGE(td->audio_icon), "audio-volume-muted-symbolic", GTK_ICON_SIZE_MENU);
        gtk_widget_show(td->audio_icon);
    } else if (playing) {
        gtk_image_set_from_icon_name(GTK_IMAGE(td->audio_icon), "audio-volume-high-symbolic", GTK_ICON_SIZE_MENU);
        gtk_widget_show(td->audio_icon);
    } else {
        gtk_widget_hide(td->audio_icon);
    }
}

void refresh_home_view(TabData* td) {
    if (!td || !td->win || !td->home_tiles_grid) return;
    BrowserWindow* win = td->win;

    clear_container_children(td->home_tiles_grid);
    const size_t cap = std::min<size_t>(win->bookmarks.size(), 8);
    for (size_t i = 0; i < cap; ++i) {
        GtkWidget* btn = gtk_button_new();
        gtk_widget_set_size_request(btn, 165, 62);
        gtk_widget_set_tooltip_text(btn, win->bookmarks[i].url.c_str());

        GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget* t_lbl = gtk_label_new(win->bookmarks[i].title.c_str());
        gtk_label_set_xalign(GTK_LABEL(t_lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(t_lbl), PANGO_ELLIPSIZE_END);
        PangoAttrList* attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_SEMIBOLD));
        gtk_label_set_attributes(GTK_LABEL(t_lbl), attrs);
        pango_attr_list_unref(attrs);

        GtkWidget* u_lbl = gtk_label_new(win->bookmarks[i].url.c_str());
        gtk_label_set_xalign(GTK_LABEL(u_lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(u_lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_opacity(u_lbl, 0.7);

        gtk_box_pack_start(GTK_BOX(vbox), t_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), u_lbl, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(btn), vbox);

        g_object_set_data_full(G_OBJECT(btn), "tile-url", g_strdup(win->bookmarks[i].url.c_str()), g_free);
        g_object_set_data(G_OBJECT(btn), "tile-tab", td);
        g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer) {
            const char* url = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "tile-url"));
            auto* tab = static_cast<TabData*>(g_object_get_data(G_OBJECT(b), "tile-tab"));
            if (tab && url) load_user_input(tab, url);
        }), nullptr);

        gtk_grid_attach(GTK_GRID(td->home_tiles_grid), btn, static_cast<gint>(i % 4), static_cast<gint>(i / 4), 1, 1);
    }
    gtk_widget_show_all(td->home_tiles_grid);

    if (td->home_stats_lbl) {
        const double total_mb = static_cast<double>(win->cached_mem.total_rss_kb) / 1024.0;
        const gint tab_count = win->notebook ? gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook)) : 1;
        char buf[160];
        std::snprintf(
            buf,
            sizeof(buf),
            "RAM: %.1f MB · Open Tabs: %d · Saved Logins: %zu · Shield: %s · Ctrl+Shift+P Palette",
            total_mb,
            tab_count,
            win->passwords.size(),
            win->config.shield_enabled ? "ON" : "OFF"
        );
        gtk_label_set_text(GTK_LABEL(td->home_stats_lbl), buf);
    }
}

void build_home_view(TabData* td) {
    if (td->home_view) return;
    BrowserWindow* win = td->win;

    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    GtkWidget* center_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_halign(center_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(center_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(center_box, 24);
    gtk_widget_set_margin_end(center_box, 24);
    gtk_widget_set_margin_top(center_box, 28);
    gtk_widget_set_margin_bottom(center_box, 28);

    GtkWidget* title_lbl = gtk_label_new("Panda Browser");
    PangoAttrList* title_attrs = pango_attr_list_new();
    pango_attr_list_insert(title_attrs, pango_attr_scale_new(2.0));
    pango_attr_list_insert(title_attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(title_lbl), title_attrs);
    pango_attr_list_unref(title_attrs);

    const char* badge = win->is_private
        ? "Private Session · Zero Disk History or Passwords"
        : "v2.00.00 · Instant Native Home · Low-RAM WebKitGTK";
    GtkWidget* sub_lbl = gtk_label_new(badge);
    gtk_widget_set_opacity(sub_lbl, 0.72);

    GtkWidget* search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_size_request(search_row, 620, -1);
    td->home_search_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(td->home_search_entry), "Search the web or enter a URL...");
    GtkWidget* go_btn = gtk_button_new_with_label("Search");
    gtk_style_context_add_class(gtk_widget_get_style_context(go_btn), "suggested-action");
    gtk_box_pack_start(GTK_BOX(search_row), td->home_search_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(search_row), go_btn, FALSE, FALSE, 0);

    g_signal_connect(td->home_search_entry, "activate", G_CALLBACK(+[](GtkEntry* e, gpointer d) {
        load_user_input(static_cast<TabData*>(d), gtk_entry_get_text(e));
    }), td);
    g_signal_connect(go_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        auto* tab = static_cast<TabData*>(d);
        load_user_input(tab, gtk_entry_get_text(GTK_ENTRY(tab->home_search_entry)));
    }), td);

    td->home_tiles_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(td->home_tiles_grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(td->home_tiles_grid), 10);
    gtk_widget_set_halign(td->home_tiles_grid, GTK_ALIGN_CENTER);

    GtkWidget* quick_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(quick_bar, GTK_ALIGN_CENTER);

    auto add_quick_btn = [&](const char* label, const char* target) {
        GtkWidget* b = gtk_button_new_with_label(label);
        g_object_set_data(G_OBJECT(b), "q-target", const_cast<char*>(target));
        g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton* btn, gpointer d) {
            const char* t = static_cast<const char*>(g_object_get_data(G_OBJECT(btn), "q-target"));
            show_internal_view(static_cast<TabData*>(d), t);
        }), td);
        gtk_box_pack_start(GTK_BOX(quick_bar), b, FALSE, FALSE, 0);
    };
    add_quick_btn("History (Ctrl+H)", kPandaHistoryUri);
    add_quick_btn("Bookmarks (Ctrl+Shift+O)", kPandaBookmarksUri);
    add_quick_btn("Passwords (Ctrl+Shift+K)", kPandaPasswordsUri);

    GtkWidget* prefs_btn = gtk_button_new_with_label("Preferences (Ctrl+,)");
    g_signal_connect(prefs_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        show_preferences_dialog(static_cast<TabData*>(d)->win);
    }), td);
    gtk_box_pack_start(GTK_BOX(quick_bar), prefs_btn, FALSE, FALSE, 0);

    td->home_stats_lbl = gtk_label_new("");
    gtk_widget_set_opacity(td->home_stats_lbl, 0.65);

    gtk_box_pack_start(GTK_BOX(center_box), title_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(center_box), sub_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(center_box), search_row, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(center_box), td->home_tiles_grid, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(center_box), quick_bar, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(center_box), td->home_stats_lbl, FALSE, FALSE, 4);

    gtk_container_add(GTK_CONTAINER(scroll), center_box);
    gtk_widget_show_all(scroll);
    td->home_view = scroll;
    gtk_stack_add_named(GTK_STACK(td->stack), td->home_view, "home");
}

void refresh_history_view(TabData* td) {
    if (!td || !td->win || !td->history_list) return;
    clear_container_children(td->history_list);

    std::string filter;
    if (td->history_search) {
        const char* q = gtk_entry_get_text(GTK_ENTRY(td->history_search));
        if (q) {
            gchar* low = g_ascii_strdown(q, -1);
            filter = low ? low : "";
            g_free(low);
        }
    }

    const auto& items = td->win->history;
    int shown = 0;
    for (size_t rev = 0; rev < items.size() && shown < 250; ++rev) {
        const size_t idx = items.size() - 1 - rev;
        const HistoryItem& h = items[idx];

        if (!filter.empty()) {
            gchar* t_low = g_ascii_strdown(h.title.c_str(), -1);
            gchar* u_low = g_ascii_strdown(h.url.c_str(), -1);
            const bool match = (t_low && std::strstr(t_low, filter.c_str())) ||
                               (u_low && std::strstr(u_low, filter.c_str()));
            g_free(t_low);
            g_free(u_low);
            if (!match) continue;
        }

        GtkWidget* row = gtk_list_box_row_new();
        GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(hbox, 10);
        gtk_widget_set_margin_end(hbox, 10);
        gtk_widget_set_margin_top(hbox, 6);
        gtk_widget_set_margin_bottom(hbox, 6);

        GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget* t_lbl = gtk_label_new(h.title.c_str());
        gtk_label_set_xalign(GTK_LABEL(t_lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(t_lbl), PANGO_ELLIPSIZE_END);
        GtkWidget* u_lbl = gtk_label_new((format_timestamp(h.timestamp) + "  ·  " + h.url).c_str());
        gtk_label_set_xalign(GTK_LABEL(u_lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(u_lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_opacity(u_lbl, 0.7);
        gtk_box_pack_start(GTK_BOX(vbox), t_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), u_lbl, FALSE, FALSE, 0);

        GtkWidget* open_btn = gtk_button_new_with_label("Open");
        g_object_set_data_full(G_OBJECT(open_btn), "h-url", g_strdup(h.url.c_str()), g_free);
        g_signal_connect(open_btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer d) {
            const char* u = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "h-url"));
            if (u) load_user_input(static_cast<TabData*>(d), u);
        }), td);

        GtkWidget* del_btn = gtk_button_new_from_icon_name("edit-delete-symbolic", GTK_ICON_SIZE_MENU);
        gtk_widget_set_tooltip_text(del_btn, "Remove from history");
        g_object_set_data(G_OBJECT(del_btn), "h-idx", GSIZE_TO_POINTER(idx));
        g_signal_connect(del_btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer d) {
            auto* tab = static_cast<TabData*>(d);
            const size_t remove_idx = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(b), "h-idx"));
            if (remove_idx < tab->win->history.size()) {
                tab->win->history.erase(tab->win->history.begin() + static_cast<ptrdiff_t>(remove_idx));
                tab->win->store.save_history(tab->win->history);
                refresh_url_completions(tab->win);
                refresh_history_view(tab);
            }
        }), td);

        gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), del_btn, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), open_btn, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), hbox);
        g_object_set_data_full(G_OBJECT(row), "h-url", g_strdup(h.url.c_str()), g_free);
        gtk_container_add(GTK_CONTAINER(td->history_list), row);
        ++shown;
    }

    if (shown == 0) {
        GtkWidget* empty = gtk_label_new(filter.empty() ? "No browsing history yet." : "No matching history entries.");
        gtk_widget_set_margin_top(empty, 24);
        gtk_widget_set_margin_bottom(empty, 24);
        gtk_widget_set_opacity(empty, 0.7);
        gtk_container_add(GTK_CONTAINER(td->history_list), empty);
    }
    gtk_widget_show_all(td->history_list);
}

void build_history_view(TabData* td) {
    if (td->history_view) return;

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(root), 18);

    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* title = gtk_label_new("Browsing History");
    PangoAttrList* attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(1.4));
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(title), attrs);
    pango_attr_list_unref(attrs);

    td->history_search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(td->history_search), "Search history...");
    gtk_widget_set_size_request(td->history_search, 260, -1);

    GtkWidget* clear_btn = gtk_button_new_with_label("Clear All History");
    gtk_style_context_add_class(gtk_widget_get_style_context(clear_btn), "destructive-action");

    gtk_box_pack_start(GTK_BOX(header), title, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(header), clear_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(header), td->history_search, FALSE, FALSE, 0);

    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    td->history_list = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scroll), td->history_list);

    g_signal_connect(td->history_search, "search-changed", G_CALLBACK(+[](GtkSearchEntry*, gpointer d) {
        refresh_history_view(static_cast<TabData*>(d));
    }), td);

    g_signal_connect(clear_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        auto* tab = static_cast<TabData*>(d);
        tab->win->history.clear();
        tab->win->store.clear_history();
        refresh_url_completions(tab->win);
        refresh_history_view(tab);
        set_status_message(tab->win, "Browsing history cleared");
    }), td);

    g_signal_connect(td->history_list, "row-activated", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer d) {
        const char* u = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "h-url"));
        if (u) load_user_input(static_cast<TabData*>(d), u);
    }), td);

    gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);
    gtk_widget_show_all(root);
    td->history_view = root;
    gtk_stack_add_named(GTK_STACK(td->stack), td->history_view, "history");
}

void refresh_bookmarks_view(TabData* td) {
    if (!td || !td->win || !td->bookmarks_list) return;
    clear_container_children(td->bookmarks_list);

    for (size_t i = 0; i < td->win->bookmarks.size(); ++i) {
        const auto& bm = td->win->bookmarks[i];
        GtkWidget* row = gtk_list_box_row_new();
        GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(hbox, 10);
        gtk_widget_set_margin_end(hbox, 10);
        gtk_widget_set_margin_top(hbox, 6);
        gtk_widget_set_margin_bottom(hbox, 6);

        GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget* t_lbl = gtk_label_new(bm.title.c_str());
        gtk_label_set_xalign(GTK_LABEL(t_lbl), 0.0f);
        GtkWidget* u_lbl = gtk_label_new(bm.url.c_str());
        gtk_label_set_xalign(GTK_LABEL(u_lbl), 0.0f);
        gtk_widget_set_opacity(u_lbl, 0.7);
        gtk_box_pack_start(GTK_BOX(vbox), t_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), u_lbl, FALSE, FALSE, 0);

        GtkWidget* open_btn = gtk_button_new_with_label("Open");
        g_object_set_data_full(G_OBJECT(open_btn), "bm-u", g_strdup(bm.url.c_str()), g_free);
        g_signal_connect(open_btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer d) {
            const char* u = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "bm-u"));
            if (u) load_user_input(static_cast<TabData*>(d), u);
        }), td);

        GtkWidget* del_btn = gtk_button_new_from_icon_name("edit-delete-symbolic", GTK_ICON_SIZE_MENU);
        g_object_set_data(G_OBJECT(del_btn), "bm-idx", GSIZE_TO_POINTER(i));
        g_signal_connect(del_btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer d) {
            auto* tab = static_cast<TabData*>(d);
            const size_t idx = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(b), "bm-idx"));
            if (idx < tab->win->bookmarks.size()) {
                tab->win->bookmarks.erase(tab->win->bookmarks.begin() + static_cast<ptrdiff_t>(idx));
                tab->win->store.save_bookmarks(tab->win->bookmarks);
                rebuild_bookmarks_bar(tab->win);
                refresh_url_completions(tab->win);
                refresh_bookmarks_view(tab);
            }
        }), td);

        gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), del_btn, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), open_btn, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), hbox);
        g_object_set_data_full(G_OBJECT(row), "bm-u", g_strdup(bm.url.c_str()), g_free);
        gtk_container_add(GTK_CONTAINER(td->bookmarks_list), row);
    }

    if (td->win->bookmarks.empty()) {
        GtkWidget* empty = gtk_label_new("No bookmarks saved. Press Ctrl+D on any page to add one.");
        gtk_widget_set_margin_top(empty, 24);
        gtk_widget_set_opacity(empty, 0.7);
        gtk_container_add(GTK_CONTAINER(td->bookmarks_list), empty);
    }
    gtk_widget_show_all(td->bookmarks_list);
}

void build_bookmarks_view(TabData* td) {
    if (td->bookmarks_view) return;

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(root), 18);

    GtkWidget* title = gtk_label_new("Bookmarks");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    PangoAttrList* attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(1.4));
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(title), attrs);
    pango_attr_list_unref(attrs);

    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    td->bookmarks_list = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scroll), td->bookmarks_list);

    g_signal_connect(td->bookmarks_list, "row-activated", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer d) {
        const char* u = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "bm-u"));
        if (u) load_user_input(static_cast<TabData*>(d), u);
    }), td);

    gtk_box_pack_start(GTK_BOX(root), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);
    gtk_widget_show_all(root);
    td->bookmarks_view = root;
    gtk_stack_add_named(GTK_STACK(td->stack), td->bookmarks_view, "bookmarks");
}

void refresh_passwords_view(TabData* td) {
    if (!td || !td->win || !td->passwords_list) return;
    clear_container_children(td->passwords_list);

    std::string filter;
    if (td->passwords_search) {
        const char* q = gtk_entry_get_text(GTK_ENTRY(td->passwords_search));
        if (q) {
            gchar* low = g_ascii_strdown(q, -1);
            filter = low ? low : "";
            g_free(low);
        }
    }

    int shown = 0;
    const auto& entries = td->win->passwords;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& p = entries[i];
        if (!filter.empty()) {
            gchar* o_low = g_ascii_strdown(p.origin.c_str(), -1);
            gchar* u_low = g_ascii_strdown(p.username.c_str(), -1);
            const bool match = (o_low && std::strstr(o_low, filter.c_str())) ||
                               (u_low && std::strstr(u_low, filter.c_str()));
            g_free(o_low);
            g_free(u_low);
            if (!match) continue;
        }

        GtkWidget* row = gtk_list_box_row_new();
        GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(hbox, 10);
        gtk_widget_set_margin_end(hbox, 10);
        gtk_widget_set_margin_top(hbox, 8);
        gtk_widget_set_margin_bottom(hbox, 8);

        GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        std::string head = p.origin + "  —  " + (p.username.empty() ? "(no username)" : p.username);
        GtkWidget* site_lbl = gtk_label_new(head.c_str());
        gtk_label_set_xalign(GTK_LABEL(site_lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(site_lbl), PANGO_ELLIPSIZE_END);

        GtkWidget* pass_lbl = gtk_label_new("••••••••••••");
        gtk_label_set_xalign(GTK_LABEL(pass_lbl), 0.0f);
        gtk_widget_set_opacity(pass_lbl, 0.78);
        gtk_box_pack_start(GTK_BOX(vbox), site_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), pass_lbl, FALSE, FALSE, 0);

        GtkWidget* show_btn = gtk_toggle_button_new_with_label("Show");
        g_object_set_data(G_OBJECT(show_btn), "pw-lbl", pass_lbl);
        g_object_set_data_full(G_OBJECT(show_btn), "pw-val", g_strdup(p.password.c_str()), g_free);
        g_signal_connect(show_btn, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer) {
            auto* lbl = GTK_LABEL(g_object_get_data(G_OBJECT(b), "pw-lbl"));
            const char* secret = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "pw-val"));
            if (gtk_toggle_button_get_active(b)) {
                gtk_label_set_text(lbl, secret ? secret : "");
                gtk_button_set_label(GTK_BUTTON(b), "Hide");
            } else {
                gtk_label_set_text(lbl, "••••••••••••");
                gtk_button_set_label(GTK_BUTTON(b), "Show");
            }
        }), nullptr);

        GtkWidget* copy_btn = gtk_button_new_with_label("Copy");
        g_object_set_data_full(G_OBJECT(copy_btn), "pw-val", g_strdup(p.password.c_str()), g_free);
        g_signal_connect(copy_btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer d) {
            auto* tab = static_cast<TabData*>(d);
            const char* secret = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "pw-val"));
            if (secret) {
                GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
                gtk_clipboard_set_text(cb, secret, -1);
                set_status_message(tab->win, "Password copied to clipboard");
            }
        }), td);

        GtkWidget* open_btn = gtk_button_new_with_label("Open Site");
        g_object_set_data_full(G_OBJECT(open_btn), "pw-origin", g_strdup(p.origin.c_str()), g_free);
        g_signal_connect(open_btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer d) {
            const char* origin = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "pw-origin"));
            if (origin) load_user_input(static_cast<TabData*>(d), origin);
        }), td);

        GtkWidget* del_btn = gtk_button_new_from_icon_name("edit-delete-symbolic", GTK_ICON_SIZE_MENU);
        g_object_set_data(G_OBJECT(del_btn), "pw-idx", GSIZE_TO_POINTER(i));
        g_signal_connect(del_btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer d) {
            auto* tab = static_cast<TabData*>(d);
            const size_t idx = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(b), "pw-idx"));
            if (idx < tab->win->passwords.size()) {
                tab->win->passwords.erase(tab->win->passwords.begin() + static_cast<ptrdiff_t>(idx));
                tab->win->store.save_passwords(tab->win->passwords);
                refresh_passwords_view(tab);
                set_status_message(tab->win, "Saved login removed");
            }
        }), td);

        gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), del_btn, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), open_btn, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), copy_btn, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), show_btn, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), hbox);
        gtk_container_add(GTK_CONTAINER(td->passwords_list), row);
        ++shown;
    }

    if (shown == 0) {
        GtkWidget* empty = gtk_label_new(
            filter.empty()
                ? "No saved passwords yet. Sign in to any site and click Save when prompted, or add one above."
                : "No matching saved logins."
        );
        gtk_widget_set_margin_top(empty, 24);
        gtk_widget_set_opacity(empty, 0.7);
        gtk_container_add(GTK_CONTAINER(td->passwords_list), empty);
    }
    gtk_widget_show_all(td->passwords_list);
}

void build_passwords_view(TabData* td) {
    if (td->passwords_view) return;

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(root), 18);

    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* title = gtk_label_new("Saved Passwords (Encrypted Vault)");
    PangoAttrList* attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(1.4));
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(title), attrs);
    pango_attr_list_unref(attrs);

    td->passwords_search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(td->passwords_search), "Search site or username...");
    gtk_widget_set_size_request(td->passwords_search, 250, -1);

    GtkWidget* clear_btn = gtk_button_new_with_label("Clear All");
    gtk_style_context_add_class(gtk_widget_get_style_context(clear_btn), "destructive-action");

    gtk_box_pack_start(GTK_BOX(header), title, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(header), clear_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(header), td->passwords_search, FALSE, FALSE, 0);

    // Quick inline "Add Login" bar
    GtkWidget* add_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* orig_e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(orig_e), "https://example.com");
    GtkWidget* user_e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(user_e), "Username or email");
    GtkWidget* pass_e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(pass_e), "Password");
    gtk_entry_set_visibility(GTK_ENTRY(pass_e), FALSE);
    GtkWidget* add_btn = gtk_button_new_with_label("Add Login");
    gtk_style_context_add_class(gtk_widget_get_style_context(add_btn), "suggested-action");

    gtk_box_pack_start(GTK_BOX(add_box), orig_e, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(add_box), user_e, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(add_box), pass_e, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(add_box), add_btn, FALSE, FALSE, 0);

    g_object_set_data(G_OBJECT(add_btn), "add-orig", orig_e);
    g_object_set_data(G_OBJECT(add_btn), "add-user", user_e);
    g_object_set_data(G_OBJECT(add_btn), "add-pass", pass_e);

    g_signal_connect(add_btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer d) {
        auto* tab = static_cast<TabData*>(d);
        if (tab->win->is_private) {
            set_status_message(tab->win, "Cannot save passwords in Private Mode");
            return;
        }
        auto* oe = GTK_ENTRY(g_object_get_data(G_OBJECT(b), "add-orig"));
        auto* ue = GTK_ENTRY(g_object_get_data(G_OBJECT(b), "add-user"));
        auto* pe = GTK_ENTRY(g_object_get_data(G_OBJECT(b), "add-pass"));

        const char* raw_orig = gtk_entry_get_text(oe);
        const char* raw_user = gtk_entry_get_text(ue);
        const char* raw_pass = gtk_entry_get_text(pe);
        if (!raw_orig || !raw_pass || raw_pass[0] == '\0') return;

        std::string norm = extract_origin(raw_orig);
        if (norm.empty()) {
            std::string with_https = std::string("https://") + raw_orig;
            norm = extract_origin(with_https.c_str());
        }
        if (norm.empty()) {
            set_status_message(tab->win, "Enter a valid site URL (e.g. https://github.com)");
            return;
        }

        const std::string uname = raw_user ? raw_user : "";
        bool updated = false;
        for (auto& e : tab->win->passwords) {
            if (e.origin == norm && e.username == uname) {
                e.password = raw_pass;
                e.updated_at = static_cast<int64_t>(std::time(nullptr));
                updated = true;
                break;
            }
        }
        if (!updated) {
            tab->win->passwords.push_back({norm, uname, raw_pass, static_cast<int64_t>(std::time(nullptr))});
        }
        tab->win->store.save_passwords(tab->win->passwords);
        gtk_entry_set_text(oe, "");
        gtk_entry_set_text(ue, "");
        gtk_entry_set_text(pe, "");
        refresh_passwords_view(tab);
        set_status_message(tab->win, "Password saved to encrypted vault");
    }), td);

    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    td->passwords_list = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scroll), td->passwords_list);

    g_signal_connect(td->passwords_search, "search-changed", G_CALLBACK(+[](GtkSearchEntry*, gpointer d) {
        refresh_passwords_view(static_cast<TabData*>(d));
    }), td);

    g_signal_connect(clear_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        auto* tab = static_cast<TabData*>(d);
        tab->win->passwords.clear();
        tab->win->store.save_passwords(tab->win->passwords);
        refresh_passwords_view(tab);
        set_status_message(tab->win, "All saved passwords cleared");
    }), td);

    gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), add_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);
    gtk_widget_show_all(root);
    td->passwords_view = root;
    gtk_stack_add_named(GTK_STACK(td->stack), td->passwords_view, "passwords");
}

void show_internal_view(TabData* td, std::string_view uri) {
    if (!td || !td->stack) return;
    td->suspended = false;
    td->last_active_sec = monotonic_seconds();
    g_free(td->suspended_url);
    td->suspended_url = nullptr;

    if (ascii_iequals(uri, kPandaHistoryUri)) {
        build_history_view(td);
        refresh_history_view(td);
        td->internal_uri = kPandaHistoryUri;
        gtk_stack_set_visible_child_name(GTK_STACK(td->stack), "history");
        update_tab_label_text(td, "History");
    } else if (ascii_iequals(uri, kPandaBookmarksUri)) {
        build_bookmarks_view(td);
        refresh_bookmarks_view(td);
        td->internal_uri = kPandaBookmarksUri;
        gtk_stack_set_visible_child_name(GTK_STACK(td->stack), "bookmarks");
        update_tab_label_text(td, "Bookmarks");
    } else if (ascii_iequals(uri, kPandaPasswordsUri)) {
        build_passwords_view(td);
        refresh_passwords_view(td);
        td->internal_uri = kPandaPasswordsUri;
        gtk_stack_set_visible_child_name(GTK_STACK(td->stack), "passwords");
        update_tab_label_text(td, "Passwords");
    } else {
        build_home_view(td);
        refresh_home_view(td);
        td->internal_uri = kPandaHomeUri;
        gtk_stack_set_visible_child_name(GTK_STACK(td->stack), "home");
        update_tab_label_text(td, "Panda Home");
    }
    sync_toolbar(td->win);
}

bool handle_internal_uri(TabData* td, std::string_view sv) {
    if (ascii_iequals(sv, kPandaHomeUri) ||
        ascii_iequals(sv, kPandaHistoryUri) ||
        ascii_iequals(sv, kPandaBookmarksUri) ||
        ascii_iequals(sv, kPandaPasswordsUri)) {
        show_internal_view(td, sv);
        return true;
    }
    return false;
}

void sync_toolbar(BrowserWindow* win) {
    if (!win || !win->window) return;
    TabData* td = active_tab(win);
    const char* suffix = win->is_private ? "Panda Browser (Private)" : "Panda Browser";

    if (!td) {
        gtk_window_set_title(GTK_WINDOW(win->window), suffix);
        gtk_entry_set_text(GTK_ENTRY(win->url_entry), "");
        gtk_entry_set_progress_fraction(GTK_ENTRY(win->url_entry), 0.0);
        gtk_widget_set_sensitive(win->back_btn, FALSE);
        gtk_widget_set_sensitive(win->fwd_btn, FALSE);
        gtk_image_set_from_icon_name(GTK_IMAGE(win->reload_icon), "view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
        return;
    }

    if (!td->internal_uri.empty()) {
        if (!gtk_widget_has_focus(win->url_entry)) {
            gtk_entry_set_text(GTK_ENTRY(win->url_entry), td->internal_uri.c_str());
        }
        const char* page_name = "Panda Home";
        if (td->internal_uri == kPandaHistoryUri) page_name = "History";
        else if (td->internal_uri == kPandaBookmarksUri) page_name = "Bookmarks";
        else if (td->internal_uri == kPandaPasswordsUri) page_name = "Saved Passwords";

        gchar* win_title = g_strdup_printf("%s — %s", page_name, suffix);
        gtk_window_set_title(GTK_WINDOW(win->window), win_title);
        g_free(win_title);

        gtk_widget_set_sensitive(win->back_btn, FALSE);
        gtk_widget_set_sensitive(win->fwd_btn, (td->forward_to_web && td->web_view != nullptr) ? TRUE : FALSE);
        gtk_image_set_from_icon_name(GTK_IMAGE(win->reload_icon), "view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_entry_set_progress_fraction(GTK_ENTRY(win->url_entry), 0.0);

        GtkWidget* star_img = gtk_button_get_image(GTK_BUTTON(win->bookmark_btn));
        if (star_img) {
            gtk_image_set_from_icon_name(GTK_IMAGE(star_img), "non-starred-symbolic", GTK_ICON_SIZE_BUTTON);
        }
        return;
    }

    WebKitWebView* view = td->web_view ? WEBKIT_WEB_VIEW(td->web_view) : nullptr;
    if (!view) return;

    const gchar* uri = (td->suspended && td->suspended_url)
        ? td->suspended_url
        : webkit_web_view_get_uri(view);
    if (uri && !gtk_widget_has_focus(win->url_entry)) {
        gtk_entry_set_text(GTK_ENTRY(win->url_entry), uri);
    }

    const gchar* title = webkit_web_view_get_title(view);
    if (title && title[0] != '\0') {
        gchar* win_title = g_strdup_printf("%s — %s", title, suffix);
        gtk_window_set_title(GTK_WINDOW(win->window), win_title);
        g_free(win_title);
    } else {
        gtk_window_set_title(GTK_WINDOW(win->window), suffix);
    }

    const gboolean can_back = webkit_web_view_can_go_back(view) || !td->prev_internal_uri.empty();
    gtk_widget_set_sensitive(win->back_btn, can_back);
    gtk_widget_set_sensitive(win->fwd_btn, webkit_web_view_can_go_forward(view));

    const gboolean loading = webkit_web_view_is_loading(view);
    gtk_image_set_from_icon_name(
        GTK_IMAGE(win->reload_icon),
        loading ? "process-stop-symbolic" : "view-refresh-symbolic",
        GTK_ICON_SIZE_BUTTON
    );
    gtk_widget_set_tooltip_text(win->reload_btn, loading ? "Stop loading" : "Reload page (Ctrl+R)");
    gtk_entry_set_progress_fraction(
        GTK_ENTRY(win->url_entry),
        loading ? webkit_web_view_get_estimated_load_progress(view) : 0.0
    );

    bool bookmarked = false;
    if (uri && uri[0] != '\0') {
        for (const auto& bm : win->bookmarks) {
            if (bm.url == uri) {
                bookmarked = true;
                break;
            }
        }
    }
    GtkWidget* star_img = gtk_button_get_image(GTK_BUTTON(win->bookmark_btn));
    if (star_img) {
        gtk_image_set_from_icon_name(
            GTK_IMAGE(star_img),
            bookmarked ? "starred-symbolic" : "non-starred-symbolic",
            GTK_ICON_SIZE_BUTTON
        );
    }
}

void wake_tab_if_suspended(TabData* td) {
    if (!td) return;
    td->last_active_sec = monotonic_seconds();
    if (!td->suspended) return;

    td->suspended = false;
    gchar* url = td->suspended_url;
    td->suspended_url = nullptr;
    if (url && url[0] != '\0') {
        load_user_input(td, url);
    } else {
        load_user_input(td, td->win->config.home_url.c_str());
    }
    g_free(url);
    set_status_message(td->win, "Tab woken");
}

bool is_local_host_input(std::string_view sv) noexcept {
    return starts_with_ci(sv, "localhost") || starts_with_ci(sv, "127.0.0.1") || starts_with_ci(sv, "[::1]");
}

void load_user_input(TabData* td, const char* raw) {
    if (!td || !raw) return;
    BrowserWindow* win = td->win;

    std::string_view sv(raw);
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\n' || sv.front() == '\r')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\n' || sv.back() == '\r')) {
        sv.remove_suffix(1);
    }
    if (sv.empty()) return;

    td->suspended = false;
    td->last_active_sec = monotonic_seconds();
    g_free(td->suspended_url);
    td->suspended_url = nullptr;

    if (handle_internal_uri(td, sv)) {
        return;
    }

    if (!td->internal_uri.empty()) {
        td->prev_internal_uri = td->internal_uri;
        td->internal_uri.clear();
        td->forward_to_web = false;
    }

    WebKitWebView* view = ensure_web_view(td, nullptr);
    if (!view) return;
    gtk_stack_set_visible_child_name(GTK_STACK(td->stack), "web");

    if (starts_with_ci(sv, "http://") || starts_with_ci(sv, "https://") ||
        starts_with_ci(sv, "file://") || starts_with_ci(sv, "about:")) {
        gchar* exact = g_strndup(sv.data(), sv.size());
        webkit_web_view_load_uri(view, exact);
        g_free(exact);
        sync_toolbar(win);
        return;
    }

    if (sv.front() == '/') {
        gchar* path = g_strndup(sv.data(), sv.size());
        gchar* file_uri = g_filename_to_uri(path, nullptr, nullptr);
        if (file_uri) {
            webkit_web_view_load_uri(view, file_uri);
            g_free(file_uri);
        }
        g_free(path);
        sync_toolbar(win);
        return;
    }

    const bool has_space = (sv.find(' ') != std::string_view::npos);
    const bool looks_like_host = !has_space &&
        (sv.find('.') != std::string_view::npos || is_local_host_input(sv));

    if (looks_like_host) {
        gchar* host = g_strndup(sv.data(), sv.size());
        const char* scheme = is_local_host_input(sv) ? "http" : "https";
        gchar* url = g_strdup_printf("%s://%s", scheme, host);
        webkit_web_view_load_uri(view, url);
        g_free(url);
        g_free(host);
    } else if (win) {
        std::string search_url = win->store.build_search_url(win->config.search_engine, std::string(sv));
        webkit_web_view_load_uri(view, search_url.c_str());
    }
    sync_toolbar(win);
}

void apply_settings_to_view(BrowserWindow* win, WebKitWebView* view) {
    WebKitSettings* settings = webkit_web_view_get_settings(view);

    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_javascript_can_open_windows_automatically(settings, TRUE);
    webkit_settings_set_javascript_can_access_clipboard(settings, TRUE);
    webkit_settings_set_enable_webgl(settings, TRUE);
    webkit_settings_set_enable_webaudio(settings, TRUE);
    webkit_settings_set_enable_mediasource(settings, TRUE);
    webkit_settings_set_enable_media(settings, TRUE);
    webkit_settings_set_enable_encrypted_media(settings, TRUE);
    webkit_settings_set_enable_media_stream(settings, TRUE);
    webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);
    webkit_settings_set_enable_smooth_scrolling(settings, TRUE);
    webkit_settings_set_enable_back_forward_navigation_gestures(settings, TRUE);
    webkit_settings_set_enable_developer_extras(settings, TRUE);

    WebKitHardwareAccelerationPolicy hw = WEBKIT_HARDWARE_ACCELERATION_POLICY_ON_DEMAND;
    if (win->config.gpu_policy == GpuPolicy::Always) {
        hw = WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS;
    } else if (win->config.gpu_policy == GpuPolicy::BatterySaver) {
        hw = WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER;
    }
    webkit_settings_set_hardware_acceleration_policy(settings, hw);
    webkit_settings_set_user_agent(settings, win->store.user_agent_string(win->config.ua_preset));
}

void autofill_view_if_matched(BrowserWindow* win, WebKitWebView* view) {
    if (!win || !view || win->is_private || !win->config.remember_passwords) return;
    const gchar* uri = webkit_web_view_get_uri(view);
    const std::string origin = extract_origin(uri);
    if (origin.empty()) return;

    const PasswordEntry* matched = nullptr;
    for (auto it = win->passwords.rbegin(); it != win->passwords.rend(); ++it) {
        if (it->origin == origin) {
            matched = &(*it);
            break;
        }
    }
    if (!matched) return;

    std::string js =
        "(() => {"
        "  const expectedOrigin = " + js_quote(matched->origin) + ";"
        "  if (location.origin !== expectedOrigin) return;"
        "  const userVal = " + js_quote(matched->username) + ";"
        "  const passVal = " + js_quote(matched->password) + ";"
        "  const setNative = (el, val) => {"
        "    if (!el || el.value === val) return false;"
        "    const d = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value');"
        "    if (d && d.set) d.set.call(el, val); else el.value = val;"
        "    el.dispatchEvent(new Event('input', { bubbles: true }));"
        "    el.dispatchEvent(new Event('change', { bubbles: true }));"
        "    return true;"
        "  };"
        "  const tryFill = () => {"
        "    const pws = Array.from(document.querySelectorAll('input[type=\"password\"]')).filter(el => !el.disabled && !el.readOnly);"
        "    if (pws.length === 0) return false;"
        "    let filled = false;"
        "    for (const pw of pws) {"
        "      const root = pw.form || document;"
        "      const inputs = Array.from(root.querySelectorAll('input'));"
        "      const pwIdx = inputs.indexOf(pw);"
        "      if (userVal) {"
        "        for (let i = (pwIdx >= 0 ? pwIdx - 1 : inputs.length - 1); i >= 0; --i) {"
        "          const u = inputs[i];"
        "          const t = (u.type || 'text').toLowerCase();"
        "          if ((t === 'email' || t === 'text' || t === 'tel') && !u.disabled && !u.readOnly && u.offsetParent !== null) {"
        "            if (!u.value) setNative(u, userVal);"
        "            break;"
        "          }"
        "        }"
        "      }"
        "      if (!pw.value) { setNative(pw, passVal); filled = true; }"
        "    }"
        "    return filled;"
        "  };"
        "  if (!tryFill() && document.documentElement) {"
        "    const obs = new MutationObserver(() => { if (tryFill()) obs.disconnect(); });"
        "    obs.observe(document.documentElement, { childList: true, subtree: true });"
        "    setTimeout(() => obs.disconnect(), 4000);"
        "  }"
        "})();";

#if WEBKIT_CHECK_VERSION(2, 40, 0)
    webkit_web_view_evaluate_javascript(view, js.c_str(), static_cast<gssize>(js.size()), nullptr, nullptr, nullptr, nullptr, nullptr);
#else
    webkit_web_view_run_javascript(view, js.c_str(), nullptr, nullptr, nullptr);
#endif
}

void on_password_script_message(WebKitUserContentManager*, WebKitJavascriptResult* js_result, gpointer data) {
    auto* td = static_cast<TabData*>(data);
    if (!td || !td->win || td->win->is_private || !td->win->config.remember_passwords || !td->web_view) {
        return;
    }
    BrowserWindow* win = td->win;

    JSCValue* val = webkit_javascript_result_get_js_value(js_result);
    if (!val || !jsc_value_is_string(val)) return;

    gchar* raw_json = jsc_value_to_string(val);
    if (!raw_json) return;
    std::string_view json(raw_json);

    std::string msg_type, origin, username, password;
    const bool ok = json_extract_string(json, "type", msg_type) &&
                    msg_type == "capture" &&
                    json_extract_string(json, "origin", origin) &&
                    json_extract_string(json, "password", password);
    json_extract_string(json, "username", username);
    g_free(raw_json);

    if (!ok || origin.empty() || password.empty()) return;

    // Verify JS-reported origin against WebKit's actual top-level URI
    const std::string actual_origin = extract_origin(webkit_web_view_get_uri(WEBKIT_WEB_VIEW(td->web_view)));
    if (actual_origin.empty() || actual_origin != origin) {
        return;
    }

    if (std::find(win->never_save_origins.begin(), win->never_save_origins.end(), origin) != win->never_save_origins.end()) {
        return;
    }

    bool is_update = false;
    for (const auto& e : win->passwords) {
        if (e.origin == origin && e.username == username) {
            if (e.password == password) {
                return; // Already saved identically
            }
            is_update = true;
            break;
        }
    }

    win->pending_origin = origin;
    win->pending_user = username;
    win->pending_secret = password;

    std::string prompt = is_update
        ? ("Update saved password for " + (username.empty() ? "this account" : username) + " on " + origin + "?")
        : ("Save password for " + (username.empty() ? "this account" : username) + " on " + origin + "?");
    gtk_label_set_text(GTK_LABEL(win->pass_bar_label), prompt.c_str());
    gtk_widget_show_all(win->pass_bar);
}

void show_error_page(WebKitWebView* view, const char* heading, const char* uri, const char* details) {
    gchar* esc_h = escape_html(heading);
    gchar* esc_u = escape_html(uri ? uri : "");
    gchar* esc_d = escape_html(details ? details : "Unknown error");

    gchar* html = g_strdup_printf(
        "<!doctype html><html><head><meta charset='utf-8'><title>%s</title><style>"
        "body{font-family:system-ui,sans-serif;background:#111317;color:#e5e7eb;display:flex;"
        "align-items:center;justify-content:center;min-height:100vh;margin:0;}"
        ".card{max-width:560px;padding:28px;border:1px solid #2a2f3a;border-radius:10px;background:#181b22;}"
        "h1{font-size:20px;margin:0 0 10px 0;}.url{font-family:monospace;font-size:12px;color:#9ca3af;"
        "word-break:break-all;margin-bottom:12px;}p{font-size:14px;color:#d1d5db;margin:0 0 18px 0;}"
        "button{background:#2563eb;color:#fff;border:none;border-radius:6px;padding:9px 16px;cursor:pointer;}"
        "</style></head><body><div class='card'><h1>%s</h1><div class='url'>%s</div><p>%s</p>"
        "<button onclick='location.reload()'>Reload Page</button></div></body></html>",
        esc_h, esc_h, esc_u, esc_d
    );
    webkit_web_view_load_alternate_html(view, html, uri ? uri : "about:blank", uri);
    g_free(html);
    g_free(esc_h);
    g_free(esc_u);
    g_free(esc_d);
}

bool is_supported_browser_uri(const char* uri) noexcept {
    if (!uri) return false;
    const std::string_view sv(uri);
    return starts_with_ci(sv, "http://") ||
           starts_with_ci(sv, "https://") ||
           starts_with_ci(sv, "file://") ||
           starts_with_ci(sv, "about:") ||
           starts_with_ci(sv, "data:") ||
           starts_with_ci(sv, "blob:") ||
           starts_with_ci(sv, "panda://") ||
           starts_with_ci(sv, "webkit:");
}

gboolean on_decide_policy(
    WebKitWebView* view,
    WebKitPolicyDecision* decision,
    WebKitPolicyDecisionType decision_type,
    gpointer user_data
) {
    auto* win = static_cast<BrowserWindow*>(user_data);

    switch (decision_type) {
        case WEBKIT_POLICY_DECISION_TYPE_RESPONSE: {
            auto* resp = WEBKIT_RESPONSE_POLICY_DECISION(decision);
            if (!webkit_response_policy_decision_is_mime_type_supported(resp)) {
                webkit_policy_decision_download(decision);
                return TRUE;
            }
            return FALSE;
        }
        case WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION:
        case WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION: {
            auto* nav = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
            WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(nav);
            WebKitURIRequest* req = action ? webkit_navigation_action_get_request(action) : nullptr;
            const gchar* uri = req ? webkit_uri_request_get_uri(req) : nullptr;
            if (!uri) return FALSE;

            if (starts_with_ci(uri, "panda://")) {
                if (decision_type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
                    open_tab(win, uri, nullptr, true);
                } else if (TabData* td = get_tab_data(view)) {
                    handle_internal_uri(td, uri);
                }
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }

            if (!is_supported_browser_uri(uri)) {
                gtk_show_uri_on_window(GTK_WINDOW(win->window), uri, GDK_CURRENT_TIME, nullptr);
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
            return FALSE;
        }
        default:
            return FALSE;
    }
}

gboolean on_permission_request(WebKitWebView*, WebKitPermissionRequest* req, gpointer) {
#if WEBKIT_CHECK_VERSION(2, 40, 0)
    if (WEBKIT_IS_CLIPBOARD_PERMISSION_REQUEST(req)) {
        webkit_permission_request_allow(req);
        return TRUE;
    }
#endif
#if WEBKIT_CHECK_VERSION(2, 32, 0)
    if (WEBKIT_IS_MEDIA_KEY_SYSTEM_PERMISSION_REQUEST(req)) {
        webkit_permission_request_allow(req);
        return TRUE;
    }
#endif
    webkit_permission_request_deny(req);
    return TRUE;
}

gboolean on_load_failed(
    WebKitWebView* view,
    WebKitLoadEvent,
    gchar* failing_uri,
    GError* error,
    gpointer
) {
    if (error && (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED) ||
                  g_error_matches(error, WEBKIT_PLUGIN_ERROR, WEBKIT_PLUGIN_ERROR_WILL_HANDLE_LOAD))) {
        return FALSE;
    }
    show_error_page(view, "Page Failed to Load", failing_uri, error ? error->message : "Connection error.");
    return TRUE;
}

gboolean on_load_failed_tls(
    WebKitWebView* view,
    gchar* failing_uri,
    GTlsCertificate*,
    GTlsCertificateFlags,
    gpointer
) {
    show_error_page(
        view,
        "Untrusted TLS Certificate",
        failing_uri,
        "This site presented an invalid or untrusted HTTPS certificate. Connection blocked."
    );
    return TRUE;
}

void on_web_process_terminated(
    WebKitWebView* view,
    WebKitWebProcessTerminationReason reason,
    gpointer data
) {
    auto* td = static_cast<TabData*>(data);
    if (td && td->suspended) {
        return;
    }
    if (reason == WEBKIT_WEB_PROCESS_TERMINATED_BY_API) {
        return;
    }
    const char* msg = (reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT)
        ? "This tab exceeded its memory limit and was closed to protect system RAM."
        : "The renderer process stopped unexpectedly.";
    show_error_page(view, "Tab Renderer Stopped", webkit_web_view_get_uri(view), msg);
}

gboolean on_download_decide_destination(WebKitDownload* download, gchar* suggested_filename, gpointer data) {
    auto* win = static_cast<BrowserWindow*>(data);
    const char* dl_dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    if (!dl_dir || dl_dir[0] == '\0') {
        dl_dir = g_get_home_dir();
    }
    g_mkdir_with_parents(dl_dir, 0755);

    gchar* base_name = (suggested_filename && suggested_filename[0] != '\0')
        ? g_path_get_basename(suggested_filename)
        : g_strdup("download.bin");

    gchar* dest_path = g_build_filename(dl_dir, base_name, nullptr);
    for (int idx = 1; idx < 1000 && g_file_test(dest_path, G_FILE_TEST_EXISTS); ++idx) {
        g_free(dest_path);
        gchar* numbered = g_strdup_printf("%d_%s", idx, base_name);
        dest_path = g_build_filename(dl_dir, numbered, nullptr);
        g_free(numbered);
    }

    gchar* dest_uri = g_filename_to_uri(dest_path, nullptr, nullptr);
    if (dest_uri) {
        webkit_download_set_destination(download, dest_uri);
        g_object_set_data_full(G_OBJECT(download), "panda-dl-name", g_strdup(base_name), g_free);
        gchar* msg = g_strdup_printf("Downloading: %s (0%%)", base_name);
        set_status_message(win, msg);
        g_free(msg);
        gtk_widget_show(win->downloads_btn);
        g_free(dest_uri);
    }

    g_free(dest_path);
    g_free(base_name);
    return TRUE;
}

void on_download_started(WebKitWebContext*, WebKitDownload* download, gpointer data) {
    auto* win = static_cast<BrowserWindow*>(data);
    g_signal_connect(download, "decide-destination", G_CALLBACK(on_download_decide_destination), win);
    g_signal_connect(
        download,
        "notify::estimated-progress",
        G_CALLBACK(+[](WebKitDownload* dl, GParamSpec*, gpointer d) {
            auto* w = static_cast<BrowserWindow*>(d);
            const char* name = static_cast<const char*>(g_object_get_data(G_OBJECT(dl), "panda-dl-name"));
            const int pct = static_cast<int>(webkit_download_get_estimated_progress(dl) * 100.0);
            gchar* msg = g_strdup_printf("Downloading %s (%d%%)", name ? name : "file", pct);
            set_status_message(w, msg);
            g_free(msg);
        }),
        win
    );
    g_signal_connect(
        download,
        "finished",
        G_CALLBACK(+[](WebKitDownload* dl, gpointer d) {
            auto* w = static_cast<BrowserWindow*>(d);
            const char* name = static_cast<const char*>(g_object_get_data(G_OBJECT(dl), "panda-dl-name"));
            gchar* msg = g_strdup_printf("Saved to Downloads: %s", name ? name : "file");
            set_status_message(w, msg);
            g_free(msg);
            gtk_widget_show(w->downloads_btn);
        }),
        win
    );
    g_signal_connect(
        download,
        "failed",
        G_CALLBACK(+[](WebKitDownload*, GError* err, gpointer d) {
            auto* w = static_cast<BrowserWindow*>(d);
            gchar* msg = g_strdup_printf("Download failed: %s", err ? err->message : "unknown error");
            set_status_message(w, msg);
            g_free(msg);
        }),
        win
    );
}

void close_tab_widget(BrowserWindow* win, GtkWidget* page) {
    if (!win || !win->notebook || !page) return;
    const gint idx = gtk_notebook_page_num(GTK_NOTEBOOK(win->notebook), page);
    if (idx < 0) return;

    TabData* td = get_tab_data_from_page(page);
    if (td && !win->is_private) {
        std::string cur_uri;
        if (!td->internal_uri.empty()) {
            cur_uri = td->internal_uri;
        } else if (td->suspended && td->suspended_url) {
            cur_uri = td->suspended_url;
        } else if (td->web_view) {
            const gchar* u = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(td->web_view));
            if (u) cur_uri = u;
        }
        if (!cur_uri.empty() && !starts_with_ci(cur_uri, "about:")) {
            win->closed_tabs.push_back(std::move(cur_uri));
            if (win->closed_tabs.size() > 20) {
                win->closed_tabs.erase(win->closed_tabs.begin());
            }
        }
    }

    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook)) <= 1) {
        gtk_widget_destroy(win->window);
        return;
    }
    gtk_notebook_remove_page(GTK_NOTEBOOK(win->notebook), idx);
    sync_toolbar(win);
}

void suspend_tab(TabData* td) {
    if (!td || !td->internal_uri.empty() || !td->web_view || td->suspended || td->pinned) return;
    WebKitWebView* view = WEBKIT_WEB_VIEW(td->web_view);
    const gchar* uri = webkit_web_view_get_uri(view);
    if (!uri || uri[0] == '\0' || starts_with_ci(uri, "panda://")) {
        return;
    }
    if (webkit_web_view_is_playing_audio(view)) {
        return;
    }

    g_free(td->suspended_url);
    td->suspended_url = g_strdup(uri);
    td->suspended = true;

    const gchar* title = webkit_web_view_get_title(view);
    update_tab_label_text(td, title ? title : uri);

#if WEBKIT_CHECK_VERSION(2, 34, 0)
    webkit_web_view_terminate_web_process(view);
#endif
    if (active_tab(td->win) == td) {
        set_status_message(td->win, "Tab suspended (0 MB RAM) — press Reload to wake");
        sync_toolbar(td->win);
    }
}

void sleep_all_background_tabs(BrowserWindow* win) {
    if (!win || !win->notebook) return;
    TabData* active = active_tab(win);
    const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook));
    int count = 0;
    for (gint i = 0; i < n; ++i) {
        GtkWidget* child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(win->notebook), i);
        TabData* td = get_tab_data_from_page(child);
        if (!td || td == active) continue;
        if (!td->suspended && !td->pinned && td->web_view && td->internal_uri.empty()) {
            suspend_tab(td);
            if (td->suspended) ++count;
        }
    }
    gchar* msg = g_strdup_printf("Hibernated %d background tab(s) to save RAM", count);
    set_status_message(win, msg);
    g_free(msg);
    update_ram_indicator(win);
}

void toggle_pin_tab(TabData* td) {
    if (!td || !td->win || !td->win->notebook) return;
    td->pinned = !td->pinned;
    if (td->pinned) {
        gtk_notebook_reorder_child(GTK_NOTEBOOK(td->win->notebook), td->stack, 0);
    }
    const char* title = !td->internal_uri.empty()
        ? gtk_label_get_text(GTK_LABEL(td->tab_label))
        : (td->web_view ? webkit_web_view_get_title(WEBKIT_WEB_VIEW(td->web_view)) : "New Tab");
    update_tab_label_text(td, title);
}

void toggle_mute_tab(TabData* td) {
    if (!td || !td->web_view) return;
    WebKitWebView* view = WEBKIT_WEB_VIEW(td->web_view);
    const gboolean next = !webkit_web_view_get_is_muted(view);
    webkit_web_view_set_is_muted(view, next);
    update_tab_audio_icon(td, view);
}

void toggle_current_bookmark(BrowserWindow* win) {
    WebKitWebView* view = active_web_view(win);
    if (!view) return;
    const gchar* uri = webkit_web_view_get_uri(view);
    if (!uri || uri[0] == '\0' || starts_with_ci(uri, "panda://")) {
        return;
    }
    const gchar* title = webkit_web_view_get_title(view);

    for (size_t i = 0; i < win->bookmarks.size(); ++i) {
        if (win->bookmarks[i].url == uri) {
            win->bookmarks.erase(win->bookmarks.begin() + static_cast<ptrdiff_t>(i));
            win->store.save_bookmarks(win->bookmarks);
            rebuild_bookmarks_bar(win);
            refresh_url_completions(win);
            sync_toolbar(win);
            set_status_message(win, "Bookmark removed");
            return;
        }
    }

    win->bookmarks.push_back({(title && title[0] != '\0') ? title : uri, uri, false});
    win->store.save_bookmarks(win->bookmarks);
    rebuild_bookmarks_bar(win);
    refresh_url_completions(win);
    sync_toolbar(win);
    set_status_message(win, "Bookmark saved");
}

void rebuild_bookmarks_bar(BrowserWindow* win) {
    if (!win || !win->bookmarks_bar) return;
    clear_container_children(win->bookmarks_bar);

    for (size_t i = 0; i < win->bookmarks.size(); ++i) {
        GtkWidget* btn = gtk_button_new_with_label(win->bookmarks[i].title.c_str());
        gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(btn, win->bookmarks[i].url.c_str());
        g_object_set_data_full(G_OBJECT(btn), "bm-url", g_strdup(win->bookmarks[i].url.c_str()), g_free);
        g_signal_connect(
            btn,
            "clicked",
            G_CALLBACK(+[](GtkButton* b, gpointer d) {
                auto* w = static_cast<BrowserWindow*>(d);
                const char* u = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "bm-url"));
                if (TabData* tab = active_tab(w)) {
                    if (u) load_user_input(tab, u);
                }
            }),
            win
        );
        gtk_box_pack_start(GTK_BOX(win->bookmarks_bar), btn, FALSE, FALSE, 2);
    }
    gtk_widget_show_all(win->bookmarks_bar);
    if (!win->config.show_bookmarks_bar) {
        gtk_widget_hide(win->bookmarks_bar);
    }
}

void refresh_url_completions(BrowserWindow* win) {
    if (!win || !win->url_completion_store) return;
    gtk_list_store_clear(win->url_completion_store);

    auto add_row = [&](const std::string& label, const std::string& url) {
        GtkTreeIter iter;
        gtk_list_store_append(win->url_completion_store, &iter);
        gtk_list_store_set(win->url_completion_store, &iter, 0, url.c_str(), 1, label.c_str(), -1);
    };

    for (const auto& bm : win->bookmarks) {
        add_row(bm.title + " — " + bm.url, bm.url);
    }

    size_t added = 0;
    for (auto it = win->history.rbegin(); it != win->history.rend() && added < 120; ++it, ++added) {
        add_row(it->title + " — " + it->url, it->url);
    }
}

void prepend_url_completion(BrowserWindow* win, const std::string& title, const std::string& url) {
    if (!win || !win->url_completion_store || url.empty()) return;
    std::string label = (title.empty() ? url : title) + " — " + url;
    GtkTreeIter iter;
    gtk_list_store_prepend(win->url_completion_store, &iter);
    gtk_list_store_set(win->url_completion_store, &iter, 0, url.c_str(), 1, label.c_str(), -1);
}

void update_ram_indicator(BrowserWindow* win) {
    if (!win || !win->ram_label) return;
    win->cached_mem = win->store.sample_memory_stats();
    const double total_mb = static_cast<double>(win->cached_mem.total_rss_kb) / 1024.0;
    const double ui_mb = static_cast<double>(win->cached_mem.ui_rss_kb) / 1024.0;

    char buf[128];
    std::snprintf(
        buf,
        sizeof(buf),
        "RAM: %.1f MB (UI %.1f MB, %u proc)",
        total_mb,
        ui_mb,
        win->cached_mem.web_process_count
    );
    gtk_label_set_text(GTK_LABEL(win->ram_label), buf);

    TabData* cur = active_tab(win);
    if (cur && cur->internal_uri == kPandaHomeUri && cur->home_stats_lbl) {
        const gint tab_count = win->notebook ? gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook)) : 1;
        char hbuf[160];
        std::snprintf(
            hbuf,
            sizeof(hbuf),
            "RAM: %.1f MB · Open Tabs: %d · Saved Logins: %zu · Shield: %s · Ctrl+Shift+P Palette",
            total_mb,
            tab_count,
            win->passwords.size(),
            win->config.shield_enabled ? "ON" : "OFF"
        );
        gtk_label_set_text(GTK_LABEL(cur->home_stats_lbl), hbuf);
    }
}

void go_back_active_tab(BrowserWindow* win) {
    TabData* td = active_tab(win);
    if (!td || !td->internal_uri.empty() || !td->web_view) return;
    WebKitWebView* v = WEBKIT_WEB_VIEW(td->web_view);
    if (webkit_web_view_can_go_back(v)) {
        webkit_web_view_go_back(v);
    } else if (!td->prev_internal_uri.empty()) {
        td->forward_to_web = true;
        show_internal_view(td, td->prev_internal_uri);
    }
}

void go_forward_active_tab(BrowserWindow* win) {
    TabData* td = active_tab(win);
    if (!td) return;
    if (!td->internal_uri.empty()) {
        if (td->forward_to_web && td->web_view) {
            td->prev_internal_uri = td->internal_uri;
            td->internal_uri.clear();
            td->forward_to_web = false;
            gtk_stack_set_visible_child_name(GTK_STACK(td->stack), "web");
            const gchar* t = webkit_web_view_get_title(WEBKIT_WEB_VIEW(td->web_view));
            update_tab_label_text(td, (t && t[0]) ? t : "Web");
            sync_toolbar(win);
        }
        return;
    }
    if (td->web_view) {
        webkit_web_view_go_forward(WEBKIT_WEB_VIEW(td->web_view));
    }
}

void reload_or_wake_active_tab(BrowserWindow* win) {
    TabData* td = active_tab(win);
    if (!td) return;
    if (td->suspended) {
        wake_tab_if_suspended(td);
        return;
    }
    if (!td->internal_uri.empty()) {
        show_internal_view(td, td->internal_uri);
        return;
    }
    if (!td->web_view) return;
    WebKitWebView* v = WEBKIT_WEB_VIEW(td->web_view);
    if (webkit_web_view_is_loading(v)) {
        webkit_web_view_stop_loading(v);
    } else {
        webkit_web_view_reload(v);
    }
}

void toggle_find_bar(BrowserWindow* win, bool show) {
    if (!win || !win->find_revealer) return;
    gtk_revealer_set_reveal_child(GTK_REVEALER(win->find_revealer), show ? TRUE : FALSE);
    if (show) {
        gtk_widget_grab_focus(win->find_entry);
        gtk_editable_select_region(GTK_EDITABLE(win->find_entry), 0, -1);
    } else if (WebKitWebView* v = active_web_view(win)) {
        webkit_find_controller_search_finish(webkit_web_view_get_find_controller(v));
        gtk_widget_grab_focus(GTK_WIDGET(v));
    }
}

void record_history_for_view(TabData* td, WebKitWebView* v) {
    if (!td || !td->win || td->win->is_private || !td->win->config.remember_history || td->suspended) {
        return;
    }
    const gchar* u = webkit_web_view_get_uri(v);
    const gchar* ti = webkit_web_view_get_title(v);
    if (!u || u[0] == '\0') return;
    const size_t before_sz = td->win->history.size();
    td->win->store.upsert_history(td->win->history, ti ? ti : u, u);
    if (td->win->history.size() > before_sz) {
        prepend_url_completion(td->win, ti ? ti : u, u);
    }
}

WebKitWebView* ensure_web_view(TabData* td, WebKitWebView* related_view) {
    if (!td || !td->win || !td->stack) return nullptr;
    if (td->web_view) {
        return WEBKIT_WEB_VIEW(td->web_view);
    }

    BrowserWindow* win = td->win;
    GtkWidget* view_widget = related_view
        ? webkit_web_view_new_with_related_view(related_view)
        : webkit_web_view_new_with_context(win->web_context);
    WebKitWebView* view = WEBKIT_WEB_VIEW(view_widget);

    apply_settings_to_view(win, view);
    win->blocker.apply_to_view(view, win->config.shield_enabled, win->config.force_dark);

    if (WebKitUserContentManager* ucm = webkit_web_view_get_user_content_manager(view)) {
        g_signal_connect(ucm, "script-message-received::pandaPass", G_CALLBACK(on_password_script_message), td);
        webkit_user_content_manager_register_script_message_handler(ucm, "pandaPass");
        if (win->pass_script) {
            webkit_user_content_manager_add_script(ucm, win->pass_script);
        }
    }

    td->web_view = view_widget;
    g_object_set_data(G_OBJECT(view_widget), "panda-tab", td);
    g_object_set_data(G_OBJECT(td->stack), "panda-webview", view_widget);

    g_signal_connect(view, "notify::title", G_CALLBACK(+[](WebKitWebView* v, GParamSpec*, gpointer d) {
        auto* t = static_cast<TabData*>(d);
        if (t->suspended || !t->internal_uri.empty()) return;
        const gchar* title = webkit_web_view_get_title(v);
        update_tab_label_text(t, title);
        if (!webkit_web_view_is_loading(v)) {
            record_history_for_view(t, v);
        }
        if (active_tab(t->win) == t) {
            sync_toolbar(t->win);
        }
    }), td);

    g_signal_connect(view, "notify::uri", G_CALLBACK(+[](WebKitWebView*, GParamSpec*, gpointer d) {
        auto* t = static_cast<TabData*>(d);
        if (active_tab(t->win) == t) {
            sync_toolbar(t->win);
        }
    }), td);

    g_signal_connect(view, "load-changed", G_CALLBACK(+[](WebKitWebView* v, WebKitLoadEvent ev, gpointer d) {
        auto* t = static_cast<TabData*>(d);
        if (ev == WEBKIT_LOAD_COMMITTED) {
            record_history_for_view(t, v);
        } else if (ev == WEBKIT_LOAD_FINISHED) {
            record_history_for_view(t, v);
            autofill_view_if_matched(t->win, v);
        }
    }), td);

    g_signal_connect(view, "notify::is-loading", G_CALLBACK(+[](WebKitWebView*, GParamSpec*, gpointer d) {
        auto* t = static_cast<TabData*>(d);
        if (active_tab(t->win) == t) {
            sync_toolbar(t->win);
        }
    }), td);

    g_signal_connect(view, "notify::estimated-load-progress", G_CALLBACK(+[](WebKitWebView*, GParamSpec*, gpointer d) {
        auto* t = static_cast<TabData*>(d);
        if (active_tab(t->win) == t) {
            sync_toolbar(t->win);
        }
    }), td);

    g_signal_connect(view, "notify::is-playing-audio", G_CALLBACK(+[](WebKitWebView* v, GParamSpec*, gpointer d) {
        update_tab_audio_icon(static_cast<TabData*>(d), v);
    }), td);

    g_signal_connect(view, "decide-policy", G_CALLBACK(on_decide_policy), win);
    g_signal_connect(view, "permission-request", G_CALLBACK(on_permission_request), win);
    g_signal_connect(view, "load-failed", G_CALLBACK(on_load_failed), win);
    g_signal_connect(view, "load-failed-with-tls-errors", G_CALLBACK(on_load_failed_tls), win);
    g_signal_connect(view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), td);

    g_signal_connect(view, "create", G_CALLBACK(+[](WebKitWebView* parent, WebKitNavigationAction*, gpointer d) -> GtkWidget* {
        GtkWidget* page = open_tab(static_cast<BrowserWindow*>(d), nullptr, parent, false);
        TabData* new_td = get_tab_data_from_page(page);
        return new_td ? new_td->web_view : nullptr;
    }), win);

    g_signal_connect(view, "ready-to-show", G_CALLBACK(+[](WebKitWebView* v, gpointer d) {
        auto* t = static_cast<TabData*>(d);
        const gint page_idx = gtk_notebook_page_num(GTK_NOTEBOOK(t->win->notebook), t->stack);
        if (page_idx >= 0) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(t->win->notebook), page_idx);
            gtk_widget_grab_focus(GTK_WIDGET(v));
            sync_toolbar(t->win);
        }
    }), td);

    g_signal_connect(view, "close", G_CALLBACK(+[](WebKitWebView*, gpointer d) {
        auto* t = static_cast<TabData*>(d);
        if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(t->win->notebook)) <= 1) {
            load_user_input(t, t->win->config.home_url.c_str());
        } else {
            close_tab_widget(t->win, t->stack);
        }
    }), td);

    gtk_widget_show(view_widget);
    gtk_stack_add_named(GTK_STACK(td->stack), view_widget, "web");
    return view;
}

GtkWidget* open_tab(
    BrowserWindow* win,
    const char* url,
    WebKitWebView* related_view,
    bool switch_to_tab,
    bool pinned,
    bool start_suspended,
    const char* suspended_title
) {
    if (!win || !win->notebook) return nullptr;
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook)) >= static_cast<gint>(win->max_pages)) {
        set_status_message(win, "Tab limit reached (max-pages)");
        return nullptr;
    }

    GtkWidget* stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_widget_show(stack);

    auto* td = new TabData();
    td->win = win;
    td->stack = stack;
    td->last_active_sec = monotonic_seconds();
    td->pinned = pinned;
    td->suspended = start_suspended;
    if (start_suspended && url) {
        td->suspended_url = g_strdup(url);
    }
    g_object_set_data(G_OBJECT(stack), "panda-tab", td);

    GtkWidget* tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    td->audio_icon = gtk_image_new_from_icon_name("audio-volume-high-symbolic", GTK_ICON_SIZE_MENU);
    td->tab_label = gtk_label_new("New Tab");
    gtk_label_set_ellipsize(GTK_LABEL(td->tab_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(td->tab_label), pinned ? 10 : 15);
    update_tab_label_text(td, suspended_title ? suspended_title : (url ? url : "New Tab"));

    GtkWidget* close_btn = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close_btn), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(close_btn, FALSE);

    gtk_box_pack_start(GTK_BOX(tab_box), td->audio_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tab_box), td->tab_label, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(tab_box), close_btn, FALSE, FALSE, 0);

    GtkWidget* tab_event_box = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(tab_event_box), FALSE);
    gtk_container_add(GTK_CONTAINER(tab_event_box), tab_box);
    gtk_widget_show_all(tab_event_box);
    gtk_widget_hide(td->audio_icon);

    g_signal_connect(
        tab_event_box,
        "button-press-event",
        G_CALLBACK(+[](GtkWidget*, GdkEventButton* ev, gpointer data) -> gboolean {
            auto* t = static_cast<TabData*>(data);
            if (ev->button == 2) {
                close_tab_widget(t->win, t->stack);
                return TRUE;
            }
            if (ev->button == 3) {
                GtkWidget* menu = gtk_menu_new();
                GtkWidget* pin_item = gtk_menu_item_new_with_label(t->pinned ? "Unpin Tab" : "Pin Tab");
                const bool muted = t->web_view && webkit_web_view_get_is_muted(WEBKIT_WEB_VIEW(t->web_view));
                GtkWidget* mute_item = gtk_menu_item_new_with_label(muted ? "Unmute Tab" : "Mute Tab");
                GtkWidget* suspend_item = gtk_menu_item_new_with_label("Suspend Tab (Free RAM)");
                GtkWidget* dup_item = gtk_menu_item_new_with_label("Duplicate Tab");
                GtkWidget* close_item = gtk_menu_item_new_with_label("Close Tab");

                gtk_widget_set_sensitive(mute_item, t->web_view != nullptr);
                gtk_widget_set_sensitive(suspend_item, !t->suspended && !t->pinned && t->web_view != nullptr && t->internal_uri.empty());

                g_signal_connect(pin_item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer d) {
                    toggle_pin_tab(static_cast<TabData*>(d));
                }), t);
                g_signal_connect(mute_item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer d) {
                    toggle_mute_tab(static_cast<TabData*>(d));
                }), t);
                g_signal_connect(suspend_item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer d) {
                    suspend_tab(static_cast<TabData*>(d));
                }), t);
                g_signal_connect(dup_item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer d) {
                    auto* tab = static_cast<TabData*>(d);
                    const gchar* cur = !tab->internal_uri.empty()
                        ? tab->internal_uri.c_str()
                        : ((tab->suspended && tab->suspended_url)
                            ? tab->suspended_url
                            : (tab->web_view ? webkit_web_view_get_uri(WEBKIT_WEB_VIEW(tab->web_view)) : nullptr));
                    open_tab(tab->win, cur ? cur : tab->win->config.home_url.c_str(), nullptr, true);
                }), t);
                g_signal_connect(close_item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer d) {
                    auto* tab = static_cast<TabData*>(d);
                    close_tab_widget(tab->win, tab->stack);
                }), t);

                gtk_menu_shell_append(GTK_MENU_SHELL(menu), pin_item);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), mute_item);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), suspend_item);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), dup_item);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), close_item);
                gtk_widget_show_all(menu);

                g_signal_connect(menu, "deactivate", G_CALLBACK(+[](GtkMenuShell* m, gpointer) {
                    g_idle_add(+[](gpointer ptr) -> gboolean {
                        gtk_widget_destroy(GTK_WIDGET(ptr));
                        return G_SOURCE_REMOVE;
                    }, m);
                }), nullptr);
                gtk_menu_popup_at_pointer(GTK_MENU(menu), reinterpret_cast<const GdkEvent*>(ev));
                return TRUE;
            }
            return FALSE;
        }),
        td
    );

    g_signal_connect(close_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        auto* t = static_cast<TabData*>(d);
        close_tab_widget(t->win, t->stack);
    }), td);

    g_signal_connect_after(stack, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer d) {
        auto* t = static_cast<TabData*>(d);
        g_free(t->suspended_url);
        delete t;
    }), td);

    const gint idx = gtk_notebook_append_page(GTK_NOTEBOOK(win->notebook), stack, tab_event_box);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(win->notebook), stack, TRUE);
    if (switch_to_tab) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(win->notebook), idx);
    }

    if (related_view) {
        ensure_web_view(td, related_view);
        gtk_stack_set_visible_child_name(GTK_STACK(td->stack), "web");
    } else if (!start_suspended && url && url[0] != '\0') {
        load_user_input(td, url);
    } else if (start_suspended && url && starts_with_ci(url, "panda://")) {
        td->suspended = false;
        g_free(td->suspended_url);
        td->suspended_url = nullptr;
        show_internal_view(td, url);
    }

    sync_toolbar(win);
    return stack;
}

void show_preferences_dialog(BrowserWindow* win) {
    GtkWidget* dlg = gtk_dialog_new_with_buttons(
        "Preferences — Panda Browser v2.00.00",
        GTK_WINDOW(win->window),
        GTK_DIALOG_MODAL,
        "Save",
        GTK_RESPONSE_ACCEPT,
        "Cancel",
        GTK_RESPONSE_CANCEL,
        nullptr
    );
    gtk_window_set_default_size(GTK_WINDOW(dlg), 460, 400);

    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_container_add(GTK_CONTAINER(area), grid);

    int row = 0;
    auto add_row = [&](const char* label, GtkWidget* widget) {
        GtkWidget* lbl = gtk_label_new(label);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);
        gtk_widget_set_hexpand(widget, TRUE);
        gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 1, 1);
        ++row;
    };

    GtkWidget* search_cb = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(search_cb), "DuckDuckGo");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(search_cb), "Startpage");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(search_cb), "Brave Search");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(search_cb), "Google");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(search_cb), "Wikipedia");
    gtk_combo_box_set_active(GTK_COMBO_BOX(search_cb), static_cast<gint>(win->config.search_engine));
    add_row("Search Engine", search_cb);

    GtkWidget* cache_cb = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cache_cb), "Ultra-Low RAM (Document Viewer)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cache_cb), "Balanced (Document Browser)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cache_cb), "Fast (Web Browser)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(cache_cb), static_cast<gint>(win->config.cache_profile));
    add_row("Memory Cache Profile", cache_cb);

    GtkWidget* gpu_cb = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(gpu_cb), "On-Demand (Near-0 GPU Idle)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(gpu_cb), "Always Hardware Accelerated");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(gpu_cb), "Battery Saver (0 GPU VRAM)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(gpu_cb), static_cast<gint>(win->config.gpu_policy));
    add_row("GPU Acceleration", gpu_cb);

    GtkWidget* ua_cb = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ua_cb), "Safari / WebKit (Recommended)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ua_cb), "Firefox Linux");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ua_cb), "Chrome Linux");
    gtk_combo_box_set_active(GTK_COMBO_BOX(ua_cb), static_cast<gint>(win->config.ua_preset));
    add_row("User-Agent Preset", ua_cb);

    GtkWidget* hib_spin = gtk_spin_button_new_with_range(0.0, 120.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(hib_spin), static_cast<gdouble>(win->config.hibernate_minutes));
    add_row("Auto-Sleep Tabs (min, 0=off)", hib_spin);

    GtkWidget* home_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(home_entry), win->config.home_url.c_str());
    add_row("Home Page", home_entry);

    GtkWidget* restore_chk = gtk_check_button_new_with_label("Restore tabs suspended on startup");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(restore_chk), win->config.restore_session ? TRUE : FALSE);
    gtk_grid_attach(GTK_GRID(grid), restore_chk, 0, row++, 2, 1);

    GtkWidget* hist_chk = gtk_check_button_new_with_label("Remember browsing history");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hist_chk), win->config.remember_history ? TRUE : FALSE);
    gtk_grid_attach(GTK_GRID(grid), hist_chk, 0, row++, 2, 1);

    GtkWidget* pass_chk = gtk_check_button_new_with_label("Offer to save & autofill passwords");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pass_chk), win->config.remember_passwords ? TRUE : FALSE);
    gtk_grid_attach(GTK_GRID(grid), pass_chk, 0, row++, 2, 1);

    GtkWidget* bm_chk = gtk_check_button_new_with_label("Show Bookmarks Bar");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bm_chk), win->config.show_bookmarks_bar ? TRUE : FALSE);
    gtk_grid_attach(GTK_GRID(grid), bm_chk, 0, row++, 2, 1);

    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        win->config.search_engine = static_cast<SearchEngine>(std::max(0, gtk_combo_box_get_active(GTK_COMBO_BOX(search_cb))));
        win->config.cache_profile = static_cast<CacheProfile>(std::max(0, gtk_combo_box_get_active(GTK_COMBO_BOX(cache_cb))));
        win->config.gpu_policy = static_cast<GpuPolicy>(std::max(0, gtk_combo_box_get_active(GTK_COMBO_BOX(gpu_cb))));
        win->config.ua_preset = static_cast<UserAgentPreset>(std::max(0, gtk_combo_box_get_active(GTK_COMBO_BOX(ua_cb))));
        win->config.hibernate_minutes = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(hib_spin));
        const char* home_txt = gtk_entry_get_text(GTK_ENTRY(home_entry));
        if (home_txt && home_txt[0] != '\0') {
            win->config.home_url = home_txt;
        }
        win->config.restore_session = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(restore_chk)) == TRUE;
        win->config.remember_history = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hist_chk)) == TRUE;
        win->config.remember_passwords = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(pass_chk)) == TRUE;
        win->config.show_bookmarks_bar = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bm_chk)) == TRUE;

        win->store.save_config(win->config);

        WebKitCacheModel cm = WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER;
        if (win->config.cache_profile == CacheProfile::Balanced) cm = WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER;
        else if (win->config.cache_profile == CacheProfile::Fast) cm = WEBKIT_CACHE_MODEL_WEB_BROWSER;
        webkit_web_context_set_cache_model(win->web_context, cm);

        const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook));
        for (gint i = 0; i < n; ++i) {
            GtkWidget* child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(win->notebook), i);
            TabData* td = get_tab_data_from_page(child);
            if (td && td->web_view) apply_settings_to_view(win, WEBKIT_WEB_VIEW(td->web_view));
        }
        rebuild_bookmarks_bar(win);
        set_status_message(win, "Preferences saved");
    }
    gtk_widget_destroy(dlg);
}

void show_command_palette(BrowserWindow* win) {
    GtkWidget* dlg = gtk_dialog_new_with_buttons(
        "Command Palette",
        GTK_WINDOW(win->window),
        GTK_DIALOG_MODAL,
        "Close",
        GTK_RESPONSE_CANCEL,
        nullptr
    );
    gtk_window_set_default_size(GTK_WINDOW(dlg), 500, 360);

    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(area), vbox);

    GtkWidget* search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Type command, tab, or bookmark...");
    gtk_box_pack_start(GTK_BOX(vbox), search, FALSE, FALSE, 0);

    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_vexpand(scroll, TRUE);
    GtkWidget* list = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scroll), list);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    struct PaletteEntry {
        std::string label;
        std::string payload;
    };
    std::vector<PaletteEntry> entries = {
        {"Open Saved Passwords (panda://passwords)", "url:panda://passwords"},
        {"Open History (panda://history)", "url:panda://history"},
        {"Open Bookmarks (panda://bookmarks)", "url:panda://bookmarks"},
        {"Open Home (panda://home)", "url:panda://home"},
        {"Sleep All Background Tabs (Free RAM)", "cmd:sleep_all"},
        {"New Private Window (Zero Disk Storage)", "cmd:private"},
        {"Toggle Reader Mode", "cmd:reader"},
        {"Toggle Force Dark Mode", "cmd:dark"},
        {"Toggle Ad & Tracker Shield", "cmd:shield"},
        {"Toggle Bookmarks Bar", "cmd:bmbar"},
        {"Open Preferences", "cmd:prefs"},
        {"Clear Browsing History", "cmd:clear_history"},
    };

    const gint tab_n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook));
    for (gint i = 0; i < tab_n; ++i) {
        GtkWidget* child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(win->notebook), i);
        TabData* td = get_tab_data_from_page(child);
        if (!td) continue;
        const char* t = td->tab_label ? gtk_label_get_text(GTK_LABEL(td->tab_label)) : "Tab";
        entries.push_back({std::string("Switch Tab: ") + (t && t[0] ? t : "Tab"), "tab:" + std::to_string(i)});
    }
    for (const auto& bm : win->bookmarks) {
        entries.push_back({"Bookmark: " + bm.title, "url:" + bm.url});
    }

    for (const auto& e : entries) {
        GtkWidget* row = gtk_list_box_row_new();
        GtkWidget* lbl = gtk_label_new(e.label.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_widget_set_margin_start(lbl, 8);
        gtk_widget_set_margin_end(lbl, 8);
        gtk_widget_set_margin_top(lbl, 6);
        gtk_widget_set_margin_bottom(lbl, 6);
        gtk_container_add(GTK_CONTAINER(row), lbl);
        g_object_set_data_full(G_OBJECT(row), "pal-payload", g_strdup(e.payload.c_str()), g_free);
        g_object_set_data_full(G_OBJECT(row), "pal-label", g_strdup(e.label.c_str()), g_free);
        gtk_container_add(GTK_CONTAINER(list), row);
    }

    gtk_list_box_set_filter_func(GTK_LIST_BOX(list), +[](GtkListBoxRow* row, gpointer d) -> gboolean {
        const gchar* q = gtk_entry_get_text(GTK_ENTRY(d));
        if (!q || q[0] == '\0') return TRUE;
        const char* lbl = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "pal-label"));
        if (!lbl) return FALSE;
        gchar* l_low = g_ascii_strdown(lbl, -1);
        gchar* q_low = g_ascii_strdown(q, -1);
        const gboolean hit = (l_low && q_low && std::strstr(l_low, q_low) != nullptr);
        g_free(l_low);
        g_free(q_low);
        return hit;
    }, search, nullptr);

    g_signal_connect(search, "search-changed", G_CALLBACK(+[](GtkSearchEntry*, gpointer d) {
        gtk_list_box_invalidate_filter(GTK_LIST_BOX(d));
    }), list);

    struct ActivateCtx {
        BrowserWindow* win;
        GtkWidget* dlg;
    } ctx{win, dlg};

    auto run_payload = +[](BrowserWindow* w, const char* p) {
        if (!p) return;
        std::string_view sv(p);
        if (sv == "cmd:sleep_all") sleep_all_background_tabs(w);
        else if (sv == "cmd:private") open_browser_window(w->config.home_url.c_str(), w->max_pages, true);
        else if (sv == "cmd:reader") ContentBlocker::trigger_reader_mode(active_web_view(w));
        else if (sv == "cmd:dark") gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->dark_btn), !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->dark_btn)));
        else if (sv == "cmd:shield") gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->shield_btn), !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->shield_btn)));
        else if (sv == "cmd:bmbar") {
            w->config.show_bookmarks_bar = !w->config.show_bookmarks_bar;
            w->store.save_config(w->config);
            rebuild_bookmarks_bar(w);
        } else if (sv == "cmd:prefs") show_preferences_dialog(w);
        else if (sv == "cmd:clear_history") {
            w->history.clear();
            w->store.clear_history();
            refresh_url_completions(w);
            if (TabData* cur = active_tab(w)) {
                if (cur->internal_uri == kPandaHistoryUri) refresh_history_view(cur);
            }
            set_status_message(w, "History cleared");
        } else if (sv.rfind("tab:", 0) == 0) {
            const int idx = std::atoi(p + 4);
            gtk_notebook_set_current_page(GTK_NOTEBOOK(w->notebook), idx);
        } else if (sv.rfind("url:", 0) == 0) {
            if (TabData* tab = active_tab(w)) load_user_input(tab, p + 4);
        }
    };

    g_object_set_data(G_OBJECT(list), "pal-ctx", &ctx);
    g_object_set_data(G_OBJECT(list), "pal-runner", reinterpret_cast<gpointer>(run_payload));

    g_signal_connect(list, "row-activated", G_CALLBACK(+[](GtkListBox* lb, GtkListBoxRow* row, gpointer) {
        auto* c = static_cast<ActivateCtx*>(g_object_get_data(G_OBJECT(lb), "pal-ctx"));
        auto runner = reinterpret_cast<void (*)(BrowserWindow*, const char*)>(g_object_get_data(G_OBJECT(lb), "pal-runner"));
        const char* p = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "pal-payload"));
        gtk_dialog_response(GTK_DIALOG(c->dlg), GTK_RESPONSE_ACCEPT);
        if (runner && p) runner(c->win, p);
    }), nullptr);

    gtk_widget_show_all(dlg);
    gtk_widget_grab_focus(search);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

gboolean on_window_key_press(GtkWidget*, GdkEventKey* ev, gpointer data) {
    auto* win = static_cast<BrowserWindow*>(data);
    const guint mods = ev->state & gtk_accelerator_get_default_mod_mask();

    if (mods == GDK_CONTROL_MASK) {
        if (ev->keyval == GDK_KEY_t || ev->keyval == GDK_KEY_T) {
            open_tab(win, win->config.home_url.c_str(), nullptr, true);
            gtk_widget_grab_focus(win->url_entry);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_w || ev->keyval == GDK_KEY_W) {
            if (TabData* t = active_tab(win)) {
                close_tab_widget(win, t->stack);
            }
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_l || ev->keyval == GDK_KEY_L || ev->keyval == GDK_KEY_k || ev->keyval == GDK_KEY_K) {
            gtk_widget_grab_focus(win->url_entry);
            gtk_editable_select_region(GTK_EDITABLE(win->url_entry), 0, -1);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_r || ev->keyval == GDK_KEY_R) {
            reload_or_wake_active_tab(win);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_f || ev->keyval == GDK_KEY_F) {
            toggle_find_bar(win, true);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_d || ev->keyval == GDK_KEY_D) {
            toggle_current_bookmark(win);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_h || ev->keyval == GDK_KEY_H) {
            open_tab(win, kPandaHistoryUri, nullptr, true);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_b || ev->keyval == GDK_KEY_B) {
            win->config.show_bookmarks_bar = !win->config.show_bookmarks_bar;
            win->store.save_config(win->config);
            rebuild_bookmarks_bar(win);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_m || ev->keyval == GDK_KEY_M) {
            toggle_mute_tab(active_tab(win));
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_comma) {
            show_preferences_dialog(win);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_equal || ev->keyval == GDK_KEY_plus || ev->keyval == GDK_KEY_KP_Add) {
            if (WebKitWebView* v = active_web_view(win)) {
                webkit_web_view_set_zoom_level(v, std::min(3.0, webkit_web_view_get_zoom_level(v) + 0.1));
            }
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_minus || ev->keyval == GDK_KEY_KP_Subtract) {
            if (WebKitWebView* v = active_web_view(win)) {
                webkit_web_view_set_zoom_level(v, std::max(0.3, webkit_web_view_get_zoom_level(v) - 0.1));
            }
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_0 || ev->keyval == GDK_KEY_KP_0) {
            if (WebKitWebView* v = active_web_view(win)) {
                webkit_web_view_set_zoom_level(v, 1.0);
            }
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_Tab || ev->keyval == GDK_KEY_Page_Down) {
            const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook));
            if (n > 1) {
                gtk_notebook_set_current_page(GTK_NOTEBOOK(win->notebook), (gtk_notebook_get_current_page(GTK_NOTEBOOK(win->notebook)) + 1) % n);
            }
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_Page_Up) {
            const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook));
            if (n > 1) {
                gtk_notebook_set_current_page(GTK_NOTEBOOK(win->notebook), (gtk_notebook_get_current_page(GTK_NOTEBOOK(win->notebook)) - 1 + n) % n);
            }
            return TRUE;
        }
    } else if (mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        if (ev->keyval == GDK_KEY_ISO_Left_Tab || ev->keyval == GDK_KEY_Tab) {
            const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook));
            if (n > 1) {
                gtk_notebook_set_current_page(GTK_NOTEBOOK(win->notebook), (gtk_notebook_get_current_page(GTK_NOTEBOOK(win->notebook)) - 1 + n) % n);
            }
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_T || ev->keyval == GDK_KEY_t) {
            if (!win->closed_tabs.empty()) {
                std::string u = std::move(win->closed_tabs.back());
                win->closed_tabs.pop_back();
                open_tab(win, u.c_str(), nullptr, true);
            }
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_N || ev->keyval == GDK_KEY_n) {
            open_browser_window(win->config.home_url.c_str(), win->max_pages, true);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_S || ev->keyval == GDK_KEY_s) {
            sleep_all_background_tabs(win);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_P || ev->keyval == GDK_KEY_p) {
            show_command_palette(win);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_B || ev->keyval == GDK_KEY_b) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->shield_btn), !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(win->shield_btn)));
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_O || ev->keyval == GDK_KEY_o) {
            open_tab(win, kPandaBookmarksUri, nullptr, true);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_K || ev->keyval == GDK_KEY_k) {
            open_tab(win, kPandaPasswordsUri, nullptr, true);
            return TRUE;
        }
    } else if (mods == (GDK_CONTROL_MASK | GDK_MOD1_MASK)) {
        if (ev->keyval == GDK_KEY_r || ev->keyval == GDK_KEY_R) {
            ContentBlocker::trigger_reader_mode(active_web_view(win));
            return TRUE;
        }
    } else if (ev->keyval == GDK_KEY_F5) {
        reload_or_wake_active_tab(win);
        return TRUE;
    } else if (ev->keyval == GDK_KEY_F12) {
        if (WebKitWebView* v = active_web_view(win)) {
            webkit_web_inspector_show(webkit_web_view_get_inspector(v));
        }
        return TRUE;
    } else if (ev->keyval == GDK_KEY_Escape) {
        if (gtk_revealer_get_reveal_child(GTK_REVEALER(win->find_revealer))) {
            toggle_find_bar(win, false);
            return TRUE;
        }
        if (WebKitWebView* v = active_web_view(win)) {
            if (webkit_web_view_is_loading(v)) {
                webkit_web_view_stop_loading(v);
                return TRUE;
            }
        }
    } else if (mods == GDK_MOD1_MASK) {
        if (ev->keyval == GDK_KEY_Left) {
            go_back_active_tab(win);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_Right) {
            go_forward_active_tab(win);
            return TRUE;
        }
    }
    return FALSE;
}

void init_web_context(BrowserWindow* win) {
    if (win->is_private) {
        win->web_context = webkit_web_context_new_ephemeral();
    } else {
        WebKitWebsiteDataManager* data_mgr = webkit_website_data_manager_new(
            "base-data-directory", win->store.data_dir().c_str(),
            "base-cache-directory", win->store.cache_dir().c_str(),
            nullptr
        );
        webkit_website_data_manager_set_itp_enabled(data_mgr, FALSE);
        win->web_context = webkit_web_context_new_with_website_data_manager(data_mgr);
        g_object_unref(data_mgr);

        WebKitCookieManager* cm = webkit_web_context_get_cookie_manager(win->web_context);
        std::string cookie_path = win->store.data_dir() + "/cookies.sqlite";
        webkit_cookie_manager_set_persistent_storage(cm, cookie_path.c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
        webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
    }

    WebKitCacheModel cm_model = WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER;
    if (win->config.cache_profile == CacheProfile::Balanced) cm_model = WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER;
    else if (win->config.cache_profile == CacheProfile::Fast) cm_model = WEBKIT_CACHE_MODEL_WEB_BROWSER;
    webkit_web_context_set_cache_model(win->web_context, cm_model);

    webkit_web_context_register_uri_scheme(
        win->web_context,
        "panda",
        +[](WebKitURISchemeRequest* req, gpointer) {
            const char* body = "<!doctype html><html><body style='background:#0f1218;color:#e5e7eb'></body></html>";
            GInputStream* stream = g_memory_input_stream_new_from_data(body, static_cast<gssize>(std::strlen(body)), nullptr);
            webkit_uri_scheme_request_finish(req, stream, static_cast<gint64>(std::strlen(body)), "text/html");
            g_object_unref(stream);
        },
        nullptr,
        nullptr
    );

    g_signal_connect(win->web_context, "download-started", G_CALLBACK(on_download_started), win);
}

void save_open_session(BrowserWindow* win) {
    if (!win || win->is_private || !win->config.restore_session || !win->notebook) return;
    std::vector<SessionTab> tabs;
    const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook));
    for (gint i = 0; i < n; ++i) {
        GtkWidget* child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(win->notebook), i);
        TabData* td = get_tab_data_from_page(child);
        if (!td) continue;
        std::string u;
        std::string t;
        if (!td->internal_uri.empty()) {
            u = td->internal_uri;
            t = td->tab_label ? gtk_label_get_text(GTK_LABEL(td->tab_label)) : u;
        } else if (td->suspended && td->suspended_url) {
            u = td->suspended_url;
            t = td->tab_label ? gtk_label_get_text(GTK_LABEL(td->tab_label)) : u;
        } else if (td->web_view) {
            WebKitWebView* v = WEBKIT_WEB_VIEW(td->web_view);
            const gchar* wu = webkit_web_view_get_uri(v);
            const gchar* wt = webkit_web_view_get_title(v);
            if (wu) u = wu;
            if (wt) t = wt;
        }
        if (!u.empty()) {
            tabs.push_back({t.empty() ? u : t, u, td->pinned});
        }
    }
    win->store.save_session(tabs);
}

void open_browser_window(const char* initial_url, uint32_t max_pages, bool is_private) {
    auto* win = new BrowserWindow();
    win->max_pages = max_pages;
    win->is_private = is_private;
    win->config = win->store.load_config();
    win->bookmarks = win->store.load_bookmarks();
    if (!is_private) {
        win->history = win->store.load_history();
        win->passwords = win->store.load_passwords();
        win->never_save_origins = win->store.load_never_save_origins();
    }
    ++g_open_windows;

    if (GtkSettings* gs = gtk_settings_get_default()) {
        g_object_set(gs, "gtk-application-prefer-dark-theme", TRUE, nullptr);
    }

    init_web_context(win);
    win->pass_script = webkit_user_script_new(
        kPasswordHelperJs,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
        nullptr,
        nullptr
    );

    win->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win->window), 1280, 800);
    gtk_window_set_title(GTK_WINDOW(win->window), is_private ? "Panda Browser (Private)" : "Panda Browser");
    gtk_window_set_icon_name(GTK_WINDOW(win->window), "panda-browser");

    GtkWidget* root_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win->window), root_vbox);

    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 4);
    gtk_box_pack_start(GTK_BOX(root_vbox), toolbar, FALSE, FALSE, 0);

    win->back_btn = gtk_button_new_from_icon_name("go-previous-symbolic", GTK_ICON_SIZE_BUTTON);
    win->fwd_btn = gtk_button_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_BUTTON);
    win->reload_btn = gtk_button_new();
    win->reload_icon = gtk_image_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(win->reload_btn), win->reload_icon);

    win->home_btn = gtk_button_new_from_icon_name("go-home-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(win->home_btn, "Home (panda://home)");

    win->url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(win->url_entry), "Search, enter URL, or panda://home...");

    win->url_completion_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    GtkEntryCompletion* comp = gtk_entry_completion_new();
    gtk_entry_completion_set_model(comp, GTK_TREE_MODEL(win->url_completion_store));
    gtk_entry_completion_set_text_column(comp, 1);
    gtk_entry_completion_set_inline_completion(comp, FALSE);
    gtk_entry_completion_set_minimum_key_length(comp, 2);
    g_signal_connect(comp, "match-selected", G_CALLBACK(+[](GtkEntryCompletion*, GtkTreeModel* model, GtkTreeIter* iter, gpointer d) -> gboolean {
        auto* w = static_cast<BrowserWindow*>(d);
        gchar* url = nullptr;
        gtk_tree_model_get(model, iter, 0, &url, -1);
        if (url) {
            if (TabData* tab = active_tab(w)) {
                load_user_input(tab, url);
                if (tab->web_view && tab->internal_uri.empty()) {
                    gtk_widget_grab_focus(tab->web_view);
                }
            }
            g_free(url);
        }
        return TRUE;
    }), win);
    gtk_entry_set_completion(GTK_ENTRY(win->url_entry), comp);
    g_object_unref(comp);
    refresh_url_completions(win);

    win->bookmark_btn = gtk_button_new_from_icon_name("non-starred-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(win->bookmark_btn, "Bookmark Page (Ctrl+D)");

    win->pass_btn = gtk_button_new_from_icon_name("dialog-password-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(win->pass_btn, "Saved Passwords (Ctrl+Shift+K)");

    win->shield_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(win->shield_btn), gtk_image_new_from_icon_name("security-high-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->shield_btn), win->config.shield_enabled ? TRUE : FALSE);
    gtk_widget_set_tooltip_text(win->shield_btn, "Ad & Tracker Shield (Ctrl+Shift+B)");

    win->dark_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(win->dark_btn), gtk_image_new_from_icon_name("weather-clear-night-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(win->dark_btn), win->config.force_dark ? TRUE : FALSE);
    gtk_widget_set_tooltip_text(win->dark_btn, "Force Dark Pages");

    GtkWidget* new_tab_btn = gtk_button_new_from_icon_name("tab-new-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(new_tab_btn, "New Tab (Ctrl+T)");

    win->menu_btn = gtk_button_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(win->menu_btn, "Menu & Command Palette (Ctrl+Shift+P)");

    gtk_box_pack_start(GTK_BOX(toolbar), win->back_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), win->fwd_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), win->reload_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), win->home_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), win->url_entry, TRUE, TRUE, 4);
    gtk_box_pack_end(GTK_BOX(toolbar), win->menu_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(toolbar), new_tab_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(toolbar), win->dark_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(toolbar), win->shield_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(toolbar), win->pass_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(toolbar), win->bookmark_btn, FALSE, FALSE, 0);

    win->bookmarks_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(win->bookmarks_bar), 2);
    gtk_box_pack_start(GTK_BOX(root_vbox), win->bookmarks_bar, FALSE, FALSE, 0);

    // Non-intrusive "Save Password?" InfoBar
    win->pass_bar = gtk_info_bar_new();
    gtk_info_bar_set_message_type(GTK_INFO_BAR(win->pass_bar), GTK_MESSAGE_QUESTION);
    win->pass_bar_label = gtk_label_new("Save password?");
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(win->pass_bar))), win->pass_bar_label);
    gtk_info_bar_add_button(GTK_INFO_BAR(win->pass_bar), "Save", GTK_RESPONSE_ACCEPT);
    gtk_info_bar_add_button(GTK_INFO_BAR(win->pass_bar), "Never for this site", GTK_RESPONSE_REJECT);
    gtk_info_bar_add_button(GTK_INFO_BAR(win->pass_bar), "Not now", GTK_RESPONSE_CANCEL);
    gtk_box_pack_start(GTK_BOX(root_vbox), win->pass_bar, FALSE, FALSE, 0);

    g_signal_connect(win->pass_bar, "response", G_CALLBACK(+[](GtkInfoBar* bar, gint resp, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        if (resp == GTK_RESPONSE_ACCEPT && !w->is_private && !w->pending_origin.empty() && !w->pending_secret.empty()) {
            bool updated = false;
            for (auto& e : w->passwords) {
                if (e.origin == w->pending_origin && e.username == w->pending_user) {
                    e.password = w->pending_secret;
                    e.updated_at = static_cast<int64_t>(std::time(nullptr));
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                w->passwords.push_back({w->pending_origin, w->pending_user, w->pending_secret, static_cast<int64_t>(std::time(nullptr))});
            }
            w->store.save_passwords(w->passwords);
            set_status_message(w, updated ? "Password updated" : "Password saved to encrypted vault");
        } else if (resp == GTK_RESPONSE_REJECT && !w->pending_origin.empty()) {
            w->never_save_origins.push_back(w->pending_origin);
            w->store.add_never_save_origin(w->pending_origin);
            set_status_message(w, "Won't ask to save passwords for this site");
        }
        w->pending_origin.clear();
        w->pending_user.clear();
        w->pending_secret.clear();
        gtk_widget_hide(GTK_WIDGET(bar));
    }), win);

    win->find_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(win->find_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    GtkWidget* find_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(find_bar), 4);
    win->find_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(win->find_entry), "Find in page...");
    GtkWidget* find_prev_btn = gtk_button_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget* find_next_btn = gtk_button_new_from_icon_name("go-down-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget* find_close_btn = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(GTK_BOX(find_bar), win->find_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(find_bar), find_prev_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(find_bar), find_next_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(find_bar), find_close_btn, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(win->find_revealer), find_bar);
    gtk_box_pack_start(GTK_BOX(root_vbox), win->find_revealer, FALSE, FALSE, 0);

    win->notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(win->notebook), TRUE);
    gtk_box_pack_start(GTK_BOX(root_vbox), win->notebook, TRUE, TRUE, 0);

    GtkWidget* status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(status_bar, 8);
    gtk_widget_set_margin_end(status_bar, 8);
    gtk_widget_set_margin_top(status_bar, 2);
    gtk_widget_set_margin_bottom(status_bar, 2);
    win->status_label = gtk_label_new(is_private ? "Private Mode" : "Ready");
    gtk_label_set_xalign(GTK_LABEL(win->status_label), 0.0f);
    win->downloads_btn = gtk_button_new_with_label("Open Downloads");
    gtk_button_set_relief(GTK_BUTTON(win->downloads_btn), GTK_RELIEF_NONE);
    win->ram_label = gtk_label_new("RAM: -- MB");
    gtk_box_pack_start(GTK_BOX(status_bar), win->status_label, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(status_bar), win->ram_label, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(status_bar), win->downloads_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(root_vbox), status_bar, FALSE, FALSE, 0);

    const std::string user_css = win->store.read_optional_file(win->store.config_dir() + "/user.css");
    const std::string user_js = win->store.read_optional_file(win->store.config_dir() + "/user.js");
    win->blocker.init(win->store.cache_dir(), user_css, user_js, win->notebook);

    g_signal_connect(win->window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer d) -> gboolean {
        save_open_session(static_cast<BrowserWindow*>(d));
        return FALSE;
    }), win);

    g_signal_connect_after(win->window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        if (w->mem_timer_id > 0) g_source_remove(w->mem_timer_id);
        if (w->hibernate_timer_id > 0) g_source_remove(w->hibernate_timer_id);
        w->blocker.shutdown();
        if (w->pass_script) webkit_user_script_unref(w->pass_script);
        if (w->url_completion_store) g_object_unref(w->url_completion_store);
        if (w->web_context) g_object_unref(w->web_context);
        delete w;
        if (--g_open_windows <= 0) {
            gtk_main_quit();
        }
    }), win);

    g_signal_connect(win->window, "key-press-event", G_CALLBACK(on_window_key_press), win);

    g_signal_connect(win->back_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        go_back_active_tab(static_cast<BrowserWindow*>(d));
    }), win);
    g_signal_connect(win->fwd_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        go_forward_active_tab(static_cast<BrowserWindow*>(d));
    }), win);
    g_signal_connect(win->reload_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        reload_or_wake_active_tab(static_cast<BrowserWindow*>(d));
    }), win);
    g_signal_connect(win->home_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        if (TabData* tab = active_tab(w)) load_user_input(tab, w->config.home_url.c_str());
    }), win);
    g_signal_connect(win->bookmark_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        toggle_current_bookmark(static_cast<BrowserWindow*>(d));
    }), win);
    g_signal_connect(win->pass_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        if (TabData* tab = active_tab(w)) show_internal_view(tab, kPandaPasswordsUri);
    }), win);
    g_signal_connect(win->shield_btn, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        w->config.shield_enabled = gtk_toggle_button_get_active(b) == TRUE;
        w->store.save_config(w->config);
        w->blocker.apply_to_all_tabs(w->config.shield_enabled, w->config.force_dark);
        set_status_message(w, w->config.shield_enabled ? "Shield ON: blocking trackers" : "Shield OFF");
    }), win);
    g_signal_connect(win->dark_btn, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        w->config.force_dark = gtk_toggle_button_get_active(b) == TRUE;
        w->store.save_config(w->config);
        w->blocker.apply_to_all_tabs(w->config.shield_enabled, w->config.force_dark);
    }), win);
    g_signal_connect(new_tab_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        open_tab(w, w->config.home_url.c_str(), nullptr, true);
        gtk_widget_grab_focus(w->url_entry);
    }), win);
    g_signal_connect(win->menu_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        show_command_palette(static_cast<BrowserWindow*>(d));
    }), win);
    g_signal_connect(win->downloads_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        const char* dl_dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
        if (!dl_dir || dl_dir[0] == '\0') dl_dir = g_get_home_dir();
        gchar* uri = g_filename_to_uri(dl_dir, nullptr, nullptr);
        if (uri) {
            gtk_show_uri_on_window(GTK_WINDOW(w->window), uri, GDK_CURRENT_TIME, nullptr);
            g_free(uri);
        }
    }), win);

    g_signal_connect(win->url_entry, "activate", G_CALLBACK(+[](GtkEntry* entry, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        if (TabData* tab = active_tab(w)) {
            load_user_input(tab, gtk_entry_get_text(entry));
            if (tab->web_view && tab->internal_uri.empty()) {
                gtk_widget_grab_focus(tab->web_view);
            }
        }
    }), win);

    g_signal_connect_after(win->notebook, "switch-page", G_CALLBACK(+[](GtkNotebook*, GtkWidget* page, guint, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        TabData* td = get_tab_data_from_page(page);
        if (td) {
            wake_tab_if_suspended(td);
            if (!td->internal_uri.empty()) {
                if (td->internal_uri == kPandaHistoryUri) refresh_history_view(td);
                else if (td->internal_uri == kPandaBookmarksUri) refresh_bookmarks_view(td);
                else if (td->internal_uri == kPandaPasswordsUri) refresh_passwords_view(td);
                else refresh_home_view(td);
            }
        }
        sync_toolbar(w);
    }), win);

    g_signal_connect(win->find_entry, "search-changed", G_CALLBACK(+[](GtkSearchEntry* e, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        if (WebKitWebView* v = active_web_view(w)) {
            WebKitFindController* fc = webkit_web_view_get_find_controller(v);
            const gchar* txt = gtk_entry_get_text(GTK_ENTRY(e));
            if (!txt || txt[0] == '\0') webkit_find_controller_search_finish(fc);
            else webkit_find_controller_search(fc, txt, WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND, 1000);
        }
    }), win);
    g_signal_connect(win->find_entry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer d) {
        if (WebKitWebView* v = active_web_view(static_cast<BrowserWindow*>(d))) {
            webkit_find_controller_search_next(webkit_web_view_get_find_controller(v));
        }
    }), win);
    g_signal_connect(find_next_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        if (WebKitWebView* v = active_web_view(static_cast<BrowserWindow*>(d))) {
            webkit_find_controller_search_next(webkit_web_view_get_find_controller(v));
        }
    }), win);
    g_signal_connect(find_prev_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        if (WebKitWebView* v = active_web_view(static_cast<BrowserWindow*>(d))) {
            webkit_find_controller_search_previous(webkit_web_view_get_find_controller(v));
        }
    }), win);
    g_signal_connect(find_close_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        toggle_find_bar(static_cast<BrowserWindow*>(d), false);
    }), win);

    // Populate initial tabs (URL arg OR suspended session restore OR instant native panda://home)
    if (initial_url && initial_url[0] != '\0') {
        open_tab(win, initial_url, nullptr, true);
    } else if (!is_private && win->config.restore_session) {
        auto session = win->store.load_session();
        if (session.empty()) {
            open_tab(win, win->config.home_url.c_str(), nullptr, true);
        } else {
            for (size_t i = 0; i < session.size(); ++i) {
                const bool is_first = (i == 0);
                open_tab(
                    win,
                    session[i].url.c_str(),
                    nullptr,
                    is_first,
                    session[i].pinned,
                    !is_first,
                    session[i].title.c_str()
                );
            }
        }
    } else {
        open_tab(win, win->config.home_url.c_str(), nullptr, true);
    }

    gtk_widget_show_all(win->window);
    gtk_widget_hide(win->pass_bar);
    gtk_widget_hide(win->downloads_btn);
    rebuild_bookmarks_bar(win);

    // Sample initial memory stats on idle so window opens instantaneously
    g_idle_add(+[](gpointer d) -> gboolean {
        update_ram_indicator(static_cast<BrowserWindow*>(d));
        return G_SOURCE_REMOVE;
    }, win);

    win->mem_timer_id = g_timeout_add_seconds(4, +[](gpointer d) -> gboolean {
        update_ram_indicator(static_cast<BrowserWindow*>(d));
        return G_SOURCE_CONTINUE;
    }, win);

    win->hibernate_timer_id = g_timeout_add_seconds(60, +[](gpointer d) -> gboolean {
        auto* w = static_cast<BrowserWindow*>(d);
        if (w->config.hibernate_minutes <= 0 || !w->notebook) return G_SOURCE_CONTINUE;
        const gint64 limit_sec = static_cast<gint64>(w->config.hibernate_minutes) * 60;
        const gint64 now = monotonic_seconds();
        TabData* active = active_tab(w);
        const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->notebook));
        for (gint i = 0; i < n; ++i) {
            GtkWidget* child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(w->notebook), i);
            TabData* td = get_tab_data_from_page(child);
            if (td && td != active && !td->suspended && !td->pinned && td->web_view && (now - td->last_active_sec) >= limit_sec) {
                suspend_tab(td);
            }
        }
        return G_SOURCE_CONTINUE;
    }, win);
}

} // namespace

int32_t launch_browser_gui(const char* initial_url, uint32_t max_pages) {
    // Prevent WebKitGTK DMABUF white-screen/GPU crashes on Linux while respecting user overrides
    g_setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", FALSE);

    g_set_prgname("panda-browser");
    g_set_application_name("Panda Browser");

    if (!gtk_init_check(nullptr, nullptr)) {
        return -1;
    }

    open_browser_window(initial_url, max_pages, false);
    gtk_main();
    return 0;
}

} // namespace panda
