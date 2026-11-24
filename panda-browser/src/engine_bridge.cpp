// ============================================================================
// FILE: panda-browser/src/engine_bridge.cpp
// Standalone C++20 DOM scanner, JS bridge, and GTK3 + WebKitGTK browser UI
// ============================================================================
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#if defined(USE_GUI_WEBKIT)
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#endif

extern "C" {

// Shared C-ABI struct layout matching `src/main.zig`
typedef struct {
    size_t text_len;
    size_t title_len;
    size_t link_count;
    size_t script_count;
    uint32_t parse_us;
} PandaParseStats;

int32_t cpp_engine_init(bool enable_simd, uint32_t max_pages);
PandaParseStats cpp_parse_document(
    const uint8_t* html_ptr,
    size_t html_len,
    uint8_t* title_buf,
    size_t title_cap,
    uint8_t* text_buf,
    size_t text_cap
);
size_t cpp_eval_script(
    const uint8_t* script_ptr,
    size_t script_len,
    uint8_t* out_buf,
    size_t out_cap
);
int32_t cpp_launch_gui(const char* initial_url);
void cpp_engine_shutdown(void);

} // extern "C"

namespace {

bool g_simd_enabled = false;
uint32_t g_max_pages = 64;

constexpr char to_lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

constexpr bool is_ascii_ws(char c) noexcept {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f';
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (to_lower_ascii(a[i]) != to_lower_ascii(b[i])) {
            return false;
        }
    }
    return true;
}

bool starts_with_ci(std::string_view s, std::string_view prefix) noexcept {
    if (s.size() < prefix.size()) {
        return false;
    }
    return iequals(s.substr(0, prefix.size()), prefix);
}

// Fast byte scan; uses glibc SIMD memchr when USE_SIMD_PARSER=1 and enabled at init
size_t find_char(std::string_view sv, size_t start, char needle) noexcept {
    if (start >= sv.size()) {
        return std::string_view::npos;
    }
#if defined(USE_SIMD_PARSER)
    if (g_simd_enabled) {
        const void* hit = std::memchr(sv.data() + start, static_cast<unsigned char>(needle), sv.size() - start);
        if (!hit) {
            return std::string_view::npos;
        }
        return static_cast<size_t>(static_cast<const char*>(hit) - sv.data());
    }
#endif
    const size_t n = sv.size();
    const char* data = sv.data();
    for (size_t i = start; i < n; ++i) {
        if (data[i] == needle) {
            return i;
        }
    }
    return std::string_view::npos;
}

// Finds closing '>' while respecting single/double quoted attributes
size_t find_tag_end(std::string_view doc, size_t start) noexcept {
    char quote = '\0';
    for (size_t i = start; i < doc.size(); ++i) {
        const char ch = doc[i];
        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
            }
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
        } else if (ch == '>') {
            return i;
        }
    }
    return std::string_view::npos;
}

// Finds matching closing tag (e.g., "</script>" or "</style>") case-insensitively
size_t find_closing_tag(std::string_view doc, size_t start, std::string_view tag_name) noexcept {
    size_t pos = start;
    while (pos < doc.size()) {
        const size_t lt = find_char(doc, pos, '<');
        if (lt == std::string_view::npos) {
            return std::string_view::npos;
        }
        if (lt + 2 + tag_name.size() <= doc.size() && doc[lt + 1] == '/') {
            const std::string_view candidate = doc.substr(lt + 2, tag_name.size());
            if (iequals(candidate, tag_name)) {
                size_t after = lt + 2 + tag_name.size();
                while (after < doc.size() && is_ascii_ws(doc[after])) {
                    ++after;
                }
                if (after < doc.size() && doc[after] == '>') {
                    return after + 1;
                }
            }
        }
        pos = lt + 1;
    }
    return std::string_view::npos;
}

bool is_block_tag(std::string_view tag) noexcept {
    return iequals(tag, "p") || iequals(tag, "div") || iequals(tag, "br") ||
           iequals(tag, "li") || iequals(tag, "tr") || iequals(tag, "td") ||
           iequals(tag, "th") || iequals(tag, "hr") || iequals(tag, "h1") ||
           iequals(tag, "h2") || iequals(tag, "h3") || iequals(tag, "h4") ||
           iequals(tag, "h5") || iequals(tag, "h6") || iequals(tag, "section") ||
           iequals(tag, "article") || iequals(tag, "header") || iequals(tag, "footer") ||
           iequals(tag, "nav") || iequals(tag, "main") || iequals(tag, "blockquote") ||
           iequals(tag, "pre");
}

void push_char(uint8_t* buf, size_t cap, size_t& pos, char ch, bool is_ws) noexcept {
    if (!buf || cap <= 1) {
        return;
    }
    if (is_ws) {
        // Trim leading whitespace and collapse consecutive spaces
        if (pos == 0 || buf[pos - 1] == ' ') {
            return;
        }
        if (pos + 1 < cap) {
            buf[pos++] = ' ';
        }
        return;
    }
    if (pos + 1 < cap) {
        buf[pos++] = static_cast<uint8_t>(ch);
    }
}

void emit_text_chunk(
    std::string_view chunk,
    bool in_title,
    uint8_t* title_buf,
    size_t title_cap,
    size_t& title_pos,
    uint8_t* text_buf,
    size_t text_cap,
    size_t& text_pos
) noexcept {
    for (size_t i = 0; i < chunk.size(); ++i) {
        char ch = chunk[i];
        bool is_ws = is_ascii_ws(ch);

        // Zero-allocation decode for common HTML character entities
        if (ch == '&') {
            const std::string_view rem = chunk.substr(i);
            if (rem.starts_with("&amp;")) {
                ch = '&';
                i += 4;
            } else if (rem.starts_with("&lt;")) {
                ch = '<';
                i += 3;
            } else if (rem.starts_with("&gt;")) {
                ch = '>';
                i += 3;
            } else if (rem.starts_with("&quot;")) {
                ch = '"';
                i += 5;
            } else if (rem.starts_with("&#39;") || rem.starts_with("&apos;")) {
                ch = '\'';
                i += rem.starts_with("&#39;") ? 4 : 5;
            } else if (rem.starts_with("&nbsp;")) {
                ch = ' ';
                is_ws = true;
                i += 5;
            }
        }

        if (in_title) {
            push_char(title_buf, title_cap, title_pos, ch, is_ws);
        } else {
            push_char(text_buf, text_cap, text_pos, ch, is_ws);
        }
    }
}

void trim_trailing_space(uint8_t* buf, size_t& pos) noexcept {
    while (pos > 0 && buf && buf[pos - 1] == ' ') {
        --pos;
    }
}

#if defined(USE_GUI_WEBKIT)

constexpr const char* kHomeUrl = "https://duckduckgo.com";
constexpr const char* kCompatibleUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15";

// Lightweight native WebKit content-blocker rules (third-party trackers & ad scripts only)
constexpr const char* kAdblockRulesJson = R"JSON([
  {
    "trigger": {
      "url-filter": "^https?://([^/]+\\.)?(doubleclick\\.net|googlesyndication\\.com|googleadservices\\.com|adservice\\.google\\.com|google-analytics\\.com|googletagmanager\\.com|scorecardresearch\\.com|taboola\\.com|outbrain\\.com|facebook\\.net|ads-twitter\\.com)/",
      "load-type": ["third-party"]
    },
    "action": { "type": "block" }
  }
])JSON";

struct PandaBrowserUI {
    GtkWidget* window = nullptr;
    GtkWidget* notebook = nullptr;
    GtkWidget* back_btn = nullptr;
    GtkWidget* fwd_btn = nullptr;
    GtkWidget* reload_btn = nullptr;
    GtkWidget* reload_icon = nullptr;
    GtkWidget* shield_btn = nullptr;
    GtkWidget* url_entry = nullptr;
    GtkWidget* find_revealer = nullptr;
    GtkWidget* find_entry = nullptr;
    GtkWidget* status_label = nullptr;

    WebKitWebContext* web_context = nullptr;
    WebKitUserContentFilterStore* filter_store = nullptr;
    WebKitUserContentFilter* adblock_filter = nullptr;
    GCancellable* filter_cancellable = nullptr;
    bool adblock_enabled = true;
};

struct TabWidgets {
    PandaBrowserUI* ui = nullptr;
    GtkWidget* web_view = nullptr;
    GtkWidget* tab_label = nullptr;
    gchar* suspended_url = nullptr;
    bool suspended = false;
};

TabWidgets* get_tab_widgets(WebKitWebView* view) {
    if (!view) {
        return nullptr;
    }
    return static_cast<TabWidgets*>(g_object_get_data(G_OBJECT(view), "panda-tab"));
}

WebKitWebView* active_web_view(PandaBrowserUI* ui) {
    if (!ui || !ui->notebook) {
        return nullptr;
    }
    const gint page = gtk_notebook_get_current_page(GTK_NOTEBOOK(ui->notebook));
    if (page < 0) {
        return nullptr;
    }
    GtkWidget* child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(ui->notebook), page);
    return child ? WEBKIT_WEB_VIEW(child) : nullptr;
}

void set_status_message(PandaBrowserUI* ui, const char* msg) {
    if (!ui || !ui->status_label) {
        return;
    }
    if (!msg || msg[0] == '\0') {
        gtk_widget_hide(ui->status_label);
        gtk_label_set_text(GTK_LABEL(ui->status_label), "");
    } else {
        gtk_label_set_text(GTK_LABEL(ui->status_label), msg);
        gtk_widget_show(ui->status_label);
    }
}

void sync_toolbar(PandaBrowserUI* ui) {
    if (!ui || !ui->window) {
        return;
    }
    WebKitWebView* view = active_web_view(ui);
    if (!view) {
        gtk_window_set_title(GTK_WINDOW(ui->window), "Panda Browser");
        gtk_entry_set_text(GTK_ENTRY(ui->url_entry), "");
        gtk_entry_set_progress_fraction(GTK_ENTRY(ui->url_entry), 0.0);
        gtk_widget_set_sensitive(ui->back_btn, FALSE);
        gtk_widget_set_sensitive(ui->fwd_btn, FALSE);
        gtk_image_set_from_icon_name(GTK_IMAGE(ui->reload_icon), "view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
        return;
    }

    TabWidgets* tw = get_tab_widgets(view);
    const gchar* uri = (tw && tw->suspended && tw->suspended_url)
        ? tw->suspended_url
        : webkit_web_view_get_uri(view);
    if (uri && !gtk_widget_has_focus(ui->url_entry)) {
        gtk_entry_set_text(GTK_ENTRY(ui->url_entry), uri);
    }

    const gchar* title = webkit_web_view_get_title(view);
    if (title && title[0] != '\0') {
        gchar* win_title = g_strdup_printf("%s — Panda Browser", title);
        gtk_window_set_title(GTK_WINDOW(ui->window), win_title);
        g_free(win_title);
    } else {
        gtk_window_set_title(GTK_WINDOW(ui->window), "Panda Browser");
    }

    gtk_widget_set_sensitive(ui->back_btn, webkit_web_view_can_go_back(view));
    gtk_widget_set_sensitive(ui->fwd_btn, webkit_web_view_can_go_forward(view));

    const gboolean loading = webkit_web_view_is_loading(view);
    gtk_image_set_from_icon_name(
        GTK_IMAGE(ui->reload_icon),
        loading ? "process-stop-symbolic" : "view-refresh-symbolic",
        GTK_ICON_SIZE_BUTTON
    );
    gtk_widget_set_tooltip_text(ui->reload_btn, loading ? "Stop loading" : "Reload page (Ctrl+R)");

    const gdouble progress = loading ? webkit_web_view_get_estimated_load_progress(view) : 0.0;
    gtk_entry_set_progress_fraction(GTK_ENTRY(ui->url_entry), progress);
}

bool is_local_host_input(std::string_view sv) noexcept {
    if (starts_with_ci(sv, "localhost") || starts_with_ci(sv, "127.0.0.1") || starts_with_ci(sv, "[::1]")) {
        return true;
    }
    return false;
}

void wake_tab_if_suspended(WebKitWebView* view) {
    TabWidgets* tw = get_tab_widgets(view);
    if (!tw || !tw->suspended) {
        return;
    }
    tw->suspended = false;
    gchar* url = tw->suspended_url;
    tw->suspended_url = nullptr;
    if (url && url[0] != '\0') {
        webkit_web_view_load_uri(view, url);
    } else {
        webkit_web_view_load_uri(view, kHomeUrl);
    }
    g_free(url);
    set_status_message(tw->ui, nullptr);
}

void load_user_input(WebKitWebView* view, const char* raw) {
    if (!view || !raw) {
        return;
    }
    std::string_view sv(raw);
    while (!sv.empty() && is_ascii_ws(sv.front())) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && is_ascii_ws(sv.back())) {
        sv.remove_suffix(1);
    }
    if (sv.empty()) {
        return;
    }

    if (TabWidgets* tw = get_tab_widgets(view)) {
        tw->suspended = false;
        g_free(tw->suspended_url);
        tw->suspended_url = nullptr;
        set_status_message(tw->ui, nullptr);
    }

    if (starts_with_ci(sv, "http://") || starts_with_ci(sv, "https://") ||
        starts_with_ci(sv, "file://") || starts_with_ci(sv, "about:")) {
        gchar* exact = g_strndup(sv.data(), sv.size());
        webkit_web_view_load_uri(view, exact);
        g_free(exact);
        return;
    }

    // Local filesystem paths (e.g. opened via desktop MIME handler) -> file:// URI
    if (sv.front() == '/') {
        gchar* path = g_strndup(sv.data(), sv.size());
        gchar* file_uri = g_filename_to_uri(path, nullptr, nullptr);
        if (file_uri) {
            webkit_web_view_load_uri(view, file_uri);
            g_free(file_uri);
        }
        g_free(path);
        return;
    }

    // Domain heuristic: no whitespace and contains '.' or local loopback address
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
    } else {
        gchar* query = g_strndup(sv.data(), sv.size());
        gchar* escaped = g_uri_escape_string(query, nullptr, TRUE);
        gchar* search_url = g_strdup_printf("https://duckduckgo.com/?q=%s", escaped);
        webkit_web_view_load_uri(view, search_url);
        g_free(search_url);
        g_free(escaped);
        g_free(query);
    }
}

void apply_filter_to_view(PandaBrowserUI* ui, WebKitWebView* view) {
    if (!ui || !view || !ui->adblock_filter) {
        return;
    }
    WebKitUserContentManager* ucm = webkit_web_view_get_user_content_manager(view);
    if (!ucm) {
        return;
    }
    if (ui->adblock_enabled) {
        webkit_user_content_manager_add_filter(ucm, ui->adblock_filter);
    } else {
        webkit_user_content_manager_remove_filter(ucm, ui->adblock_filter);
    }
}

void apply_filter_to_all_tabs(PandaBrowserUI* ui) {
    if (!ui || !ui->notebook || !ui->adblock_filter) {
        return;
    }
    const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(ui->notebook));
    for (gint i = 0; i < n; ++i) {
        GtkWidget* child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(ui->notebook), i);
        if (child) {
            apply_filter_to_view(ui, WEBKIT_WEB_VIEW(child));
        }
    }
}

void on_adblock_filter_saved(GObject* source, GAsyncResult* res, gpointer data) {
    auto* ui = static_cast<PandaBrowserUI*>(data);
    GError* err = nullptr;
    WebKitUserContentFilter* filter = webkit_user_content_filter_store_save_finish(
        WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &err
    );
    if (err) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_warning("panda-browser content filter compile error: %s", err->message);
        }
        g_error_free(err);
        return;
    }
    if (!ui || !ui->window || g_cancellable_is_cancelled(ui->filter_cancellable)) {
        if (filter) {
            webkit_user_content_filter_unref(filter);
        }
        return;
    }
    if (ui->adblock_filter) {
        webkit_user_content_filter_unref(ui->adblock_filter);
    }
    ui->adblock_filter = filter;
    apply_filter_to_all_tabs(ui);
}

void configure_web_view_settings(WebKitWebView* view) {
    WebKitSettings* settings = webkit_web_view_get_settings(view);

    // Core web app & SPA support (YouTube, Canva, modern web apps)
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

    // Smooth UX & low-RAM on-demand hardware acceleration
    webkit_settings_set_enable_smooth_scrolling(settings, TRUE);
    webkit_settings_set_enable_back_forward_navigation_gestures(settings, TRUE);
    webkit_settings_set_enable_developer_extras(settings, TRUE);
    webkit_settings_set_hardware_acceleration_policy(settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_ON_DEMAND);

    // Modern desktop Safari/WebKit User-Agent so sites like Canva/YouTube recognize the engine
    webkit_settings_set_user_agent(settings, kCompatibleUserAgent);
}

gchar* escape_html(const char* input) {
    if (!input) {
        return g_strdup("");
    }
    return g_markup_escape_text(input, -1);
}

void show_error_page(WebKitWebView* view, const char* heading, const char* uri, const char* details) {
    gchar* esc_heading = escape_html(heading);
    gchar* esc_uri = escape_html(uri ? uri : "");
    gchar* esc_details = escape_html(details ? details : "Unknown error");

    gchar* html = g_strdup_printf(
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<title>%s</title>"
        "<style>"
        "body{font-family:system-ui,-apple-system,sans-serif;background:#111317;color:#e5e7eb;"
        "display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0;}"
        ".card{max-width:560px;padding:28px;border:1px solid #2a2f3a;border-radius:10px;background:#181b22;}"
        "h1{font-size:20px;margin:0 0 10px 0;color:#f9fafb;}"
        ".url{font-family:monospace;font-size:13px;color:#9ca3af;word-break:break-all;margin-bottom:12px;}"
        "p{font-size:14px;line-height:1.5;color:#d1d5db;margin:0 0 18px 0;}"
        "button{background:#2563eb;color:#fff;border:none;border-radius:6px;padding:9px 16px;"
        "font-size:14px;cursor:pointer;}"
        "button:hover{background:#1d4ed8;}"
        "</style></head><body><div class='card'>"
        "<h1>%s</h1>"
        "<div class='url'>%s</div>"
        "<p>%s</p>"
        "<button onclick='location.reload()'>Reload Page</button>"
        "</div></body></html>",
        esc_heading, esc_heading, esc_uri, esc_details
    );

    webkit_web_view_load_alternate_html(view, html, uri ? uri : "about:blank", uri);

    g_free(html);
    g_free(esc_heading);
    g_free(esc_uri);
    g_free(esc_details);
}

bool is_supported_browser_uri(const char* uri) noexcept {
    if (!uri) {
        return false;
    }
    const std::string_view sv(uri);
    return starts_with_ci(sv, "http://") ||
           starts_with_ci(sv, "https://") ||
           starts_with_ci(sv, "file://") ||
           starts_with_ci(sv, "about:") ||
           starts_with_ci(sv, "data:") ||
           starts_with_ci(sv, "blob:") ||
           starts_with_ci(sv, "webkit:");
}

gboolean on_decide_policy(
    WebKitWebView*,
    WebKitPolicyDecision* decision,
    WebKitPolicyDecisionType decision_type,
    gpointer user_data
) {
    auto* ui = static_cast<PandaBrowserUI*>(user_data);

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
            if (uri && !is_supported_browser_uri(uri)) {
                gtk_show_uri_on_window(GTK_WINDOW(ui->window), uri, GDK_CURRENT_TIME, nullptr);
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
    // Deny sensitive hardware/notification prompts silently by default for privacy
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
    show_error_page(
        view,
        "Page Failed to Load",
        failing_uri,
        error ? error->message : "The connection could not be completed."
    );
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
    auto* tw = static_cast<TabWidgets*>(data);
    if (tw && tw->suspended) {
        // Intentional suspension to free RAM — do not respawn WebProcess with alternate HTML
        return;
    }

    const char* msg = "The renderer process stopped unexpectedly.";
    if (reason == WEBKIT_WEB_PROCESS_TERMINATED_BY_API) {
        return;
    } else if (reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT) {
        msg = "This tab exceeded its memory limit and was closed to protect system RAM.";
    }
    show_error_page(view, "Tab Renderer Stopped", webkit_web_view_get_uri(view), msg);
}

gboolean on_download_decide_destination(
    WebKitDownload* download,
    gchar* suggested_filename,
    gpointer data
) {
    auto* ui = static_cast<PandaBrowserUI*>(data);
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
        gchar* msg = g_strdup_printf("Downloading: %s", base_name);
        set_status_message(ui, msg);
        g_free(msg);
        g_free(dest_uri);
    }

    g_free(dest_path);
    g_free(base_name);
    return TRUE;
}

void on_download_started(WebKitWebContext*, WebKitDownload* download, gpointer data) {
    auto* ui = static_cast<PandaBrowserUI*>(data);
    g_signal_connect(download, "decide-destination", G_CALLBACK(on_download_decide_destination), ui);
    g_signal_connect(
        download,
        "finished",
        G_CALLBACK(+[](WebKitDownload* dl, gpointer user_data) {
            auto* ui_ptr = static_cast<PandaBrowserUI*>(user_data);
            const gchar* dest = webkit_download_get_destination(dl);
            gchar* fname = dest ? g_filename_from_uri(dest, nullptr, nullptr) : nullptr;
            gchar* base = fname ? g_path_get_basename(fname) : g_strdup("file");
            gchar* msg = g_strdup_printf("Saved to Downloads: %s", base);
            set_status_message(ui_ptr, msg);
            g_free(msg);
            g_free(base);
            g_free(fname);
        }),
        ui
    );
    g_signal_connect(
        download,
        "failed",
        G_CALLBACK(+[](WebKitDownload*, GError* err, gpointer user_data) {
            auto* ui_ptr = static_cast<PandaBrowserUI*>(user_data);
            gchar* msg = g_strdup_printf("Download failed: %s", err ? err->message : "unknown error");
            set_status_message(ui_ptr, msg);
            g_free(msg);
        }),
        ui
    );
}

void on_tab_title_changed(WebKitWebView* view, GParamSpec*, gpointer data) {
    auto* tw = static_cast<TabWidgets*>(data);
    if (tw->suspended) {
        return;
    }
    const gchar* title = webkit_web_view_get_title(view);
    if (title && title[0] != '\0') {
        gtk_label_set_text(GTK_LABEL(tw->tab_label), title);
    }
    if (active_web_view(tw->ui) == view) {
        sync_toolbar(tw->ui);
    }
}

void on_tab_state_changed(WebKitWebView* view, GParamSpec*, gpointer data) {
    auto* tw = static_cast<TabWidgets*>(data);
    if (active_web_view(tw->ui) == view) {
        sync_toolbar(tw->ui);
    }
}

void close_tab_widget(PandaBrowserUI* ui, GtkWidget* web_view) {
    if (!ui || !ui->notebook || !web_view) {
        return;
    }
    const gint idx = gtk_notebook_page_num(GTK_NOTEBOOK(ui->notebook), web_view);
    if (idx < 0) {
        return;
    }
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(ui->notebook)) <= 1) {
        gtk_widget_destroy(ui->window);
        return;
    }
    gtk_notebook_remove_page(GTK_NOTEBOOK(ui->notebook), idx);
    sync_toolbar(ui);
}

void on_close_tab_clicked(GtkButton*, gpointer data) {
    auto* tw = static_cast<TabWidgets*>(data);
    close_tab_widget(tw->ui, tw->web_view);
}

void suspend_tab(TabWidgets* tw) {
    if (!tw || !tw->web_view || tw->suspended) {
        return;
    }
    WebKitWebView* view = WEBKIT_WEB_VIEW(tw->web_view);
    const gchar* uri = webkit_web_view_get_uri(view);
    if (!uri || uri[0] == '\0') {
        return;
    }

    g_free(tw->suspended_url);
    tw->suspended_url = g_strdup(uri);
    tw->suspended = true;

    const gchar* cur_title = gtk_label_get_text(GTK_LABEL(tw->tab_label));
    gchar* zzz_title = g_strdup_printf("[zzz] %s", cur_title ? cur_title : "Tab");
    gtk_label_set_text(GTK_LABEL(tw->tab_label), zzz_title);
    g_free(zzz_title);

#if WEBKIT_CHECK_VERSION(2, 34, 0)
    webkit_web_view_terminate_web_process(view);
#endif
    if (active_web_view(tw->ui) == view) {
        set_status_message(tw->ui, "Tab suspended (0 MB RAM) — press Reload to wake");
        sync_toolbar(tw->ui);
    }
}

void on_tab_widget_destroy(GtkWidget*, gpointer data) {
    auto* tw = static_cast<TabWidgets*>(data);
    g_free(tw->suspended_url);
    g_free(tw);
}

GtkWidget* open_tab(
    PandaBrowserUI* ui,
    const char* url,
    WebKitWebView* related_view,
    bool switch_to_tab = true
) {
    if (!ui || !ui->notebook) {
        return nullptr;
    }
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(ui->notebook)) >= static_cast<gint>(g_max_pages)) {
        set_status_message(ui, "Tab limit reached (max-pages)");
        return nullptr;
    }

    GtkWidget* view_widget = related_view
        ? webkit_web_view_new_with_related_view(related_view)
        : webkit_web_view_new_with_context(ui->web_context);
    WebKitWebView* view = WEBKIT_WEB_VIEW(view_widget);

    configure_web_view_settings(view);
    apply_filter_to_view(ui, view);

    auto* tw = static_cast<TabWidgets*>(g_malloc0(sizeof(TabWidgets)));
    tw->ui = ui;
    tw->web_view = view_widget;
    g_object_set_data(G_OBJECT(view_widget), "panda-tab", tw);

    GtkWidget* tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    tw->tab_label = gtk_label_new("New Tab");
    gtk_label_set_ellipsize(GTK_LABEL(tw->tab_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(tw->tab_label), 16);

    GtkWidget* close_btn = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close_btn), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(close_btn, FALSE);

    gtk_box_pack_start(GTK_BOX(tab_box), tw->tab_label, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(tab_box), close_btn, FALSE, FALSE, 0);

    GtkWidget* tab_event_box = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(tab_event_box), FALSE);
    gtk_container_add(GTK_CONTAINER(tab_event_box), tab_box);
    gtk_widget_show_all(tab_event_box);

    g_signal_connect(
        tab_event_box,
        "button-press-event",
        G_CALLBACK(+[](GtkWidget*, GdkEventButton* ev, gpointer data) -> gboolean {
            auto* tab = static_cast<TabWidgets*>(data);
            if (ev->button == 2) {
                close_tab_widget(tab->ui, tab->web_view);
                return TRUE;
            }
            if (ev->button == 3) {
                GtkWidget* menu = gtk_menu_new();
                GtkWidget* suspend_item = gtk_menu_item_new_with_label("Suspend Tab (Free RAM)");
                GtkWidget* dup_item = gtk_menu_item_new_with_label("Duplicate Tab");
                GtkWidget* close_item = gtk_menu_item_new_with_label("Close Tab");

                gtk_widget_set_sensitive(suspend_item, !tab->suspended);

                g_signal_connect(
                    suspend_item,
                    "activate",
                    G_CALLBACK(+[](GtkMenuItem*, gpointer d) {
                        suspend_tab(static_cast<TabWidgets*>(d));
                    }),
                    tab
                );
                g_signal_connect(
                    dup_item,
                    "activate",
                    G_CALLBACK(+[](GtkMenuItem*, gpointer d) {
                        auto* t = static_cast<TabWidgets*>(d);
                        const gchar* cur = t->suspended && t->suspended_url
                            ? t->suspended_url
                            : webkit_web_view_get_uri(WEBKIT_WEB_VIEW(t->web_view));
                        open_tab(t->ui, cur ? cur : kHomeUrl, nullptr, true);
                    }),
                    tab
                );
                g_signal_connect(
                    close_item,
                    "activate",
                    G_CALLBACK(+[](GtkMenuItem*, gpointer d) {
                        auto* t = static_cast<TabWidgets*>(d);
                        close_tab_widget(t->ui, t->web_view);
                    }),
                    tab
                );

                gtk_menu_shell_append(GTK_MENU_SHELL(menu), suspend_item);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), dup_item);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), close_item);
                gtk_widget_show_all(menu);

                g_signal_connect(
                    menu,
                    "deactivate",
                    G_CALLBACK(+[](GtkMenuShell* m, gpointer) {
                        g_idle_add(+[](gpointer ptr) -> gboolean {
                            gtk_widget_destroy(GTK_WIDGET(ptr));
                            return G_SOURCE_REMOVE;
                        }, m);
                    }),
                    nullptr
                );

                gtk_menu_popup_at_pointer(GTK_MENU(menu), reinterpret_cast<const GdkEvent*>(ev));
                return TRUE;
            }
            return FALSE;
        }),
        tw
    );

    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_tab_clicked), tw);
    g_signal_connect(view_widget, "destroy", G_CALLBACK(on_tab_widget_destroy), tw);
    g_signal_connect(view, "notify::title", G_CALLBACK(on_tab_title_changed), tw);
    g_signal_connect(view, "notify::uri", G_CALLBACK(on_tab_state_changed), tw);
    g_signal_connect(view, "notify::is-loading", G_CALLBACK(on_tab_state_changed), tw);
    g_signal_connect(view, "notify::estimated-load-progress", G_CALLBACK(on_tab_state_changed), tw);
    g_signal_connect(view, "decide-policy", G_CALLBACK(on_decide_policy), ui);
    g_signal_connect(view, "permission-request", G_CALLBACK(on_permission_request), ui);
    g_signal_connect(view, "load-failed", G_CALLBACK(on_load_failed), ui);
    g_signal_connect(view, "load-failed-with-tls-errors", G_CALLBACK(on_load_failed_tls), ui);
    g_signal_connect(view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), tw);

    // Handle target="_blank" / window.open() / OAuth login popups in a new tab
    g_signal_connect(
        view,
        "create",
        G_CALLBACK(+[](WebKitWebView* parent, WebKitNavigationAction*, gpointer user_data) -> GtkWidget* {
            auto* ui_ptr = static_cast<PandaBrowserUI*>(user_data);
            return open_tab(ui_ptr, nullptr, parent, false);
        }),
        ui
    );
    g_signal_connect(
        view,
        "ready-to-show",
        G_CALLBACK(+[](WebKitWebView* v, gpointer user_data) {
            auto* tab = static_cast<TabWidgets*>(user_data);
            const gint page_idx = gtk_notebook_page_num(GTK_NOTEBOOK(tab->ui->notebook), tab->web_view);
            if (page_idx >= 0) {
                gtk_notebook_set_current_page(GTK_NOTEBOOK(tab->ui->notebook), page_idx);
                gtk_widget_grab_focus(GTK_WIDGET(v));
                sync_toolbar(tab->ui);
            }
        }),
        tw
    );
    g_signal_connect(
        view,
        "close",
        G_CALLBACK(+[](WebKitWebView* v, gpointer user_data) {
            auto* tab = static_cast<TabWidgets*>(user_data);
            if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(tab->ui->notebook)) <= 1) {
                webkit_web_view_load_uri(v, kHomeUrl);
            } else {
                close_tab_widget(tab->ui, tab->web_view);
            }
        }),
        tw
    );

    const gint idx = gtk_notebook_append_page(GTK_NOTEBOOK(ui->notebook), view_widget, tab_event_box);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(ui->notebook), view_widget, TRUE);
    gtk_widget_show(view_widget);
    if (switch_to_tab) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(ui->notebook), idx);
    }

    if (url && url[0] != '\0') {
        load_user_input(view, url);
    }
    sync_toolbar(ui);
    return view_widget;
}

void reload_or_wake_active_tab(PandaBrowserUI* ui) {
    WebKitWebView* v = active_web_view(ui);
    if (!v) {
        return;
    }
    TabWidgets* tw = get_tab_widgets(v);
    if (tw && tw->suspended) {
        wake_tab_if_suspended(v);
        return;
    }
    if (webkit_web_view_is_loading(v)) {
        webkit_web_view_stop_loading(v);
    } else {
        webkit_web_view_reload(v);
    }
}

void toggle_find_bar(PandaBrowserUI* ui, bool show) {
    if (!ui || !ui->find_revealer) {
        return;
    }
    gtk_revealer_set_reveal_child(GTK_REVEALER(ui->find_revealer), show ? TRUE : FALSE);
    if (show) {
        gtk_widget_grab_focus(ui->find_entry);
        gtk_editable_select_region(GTK_EDITABLE(ui->find_entry), 0, -1);
    } else if (WebKitWebView* v = active_web_view(ui)) {
        webkit_find_controller_search_finish(webkit_web_view_get_find_controller(v));
        gtk_widget_grab_focus(GTK_WIDGET(v));
    }
}

gboolean on_window_key_press(GtkWidget*, GdkEventKey* ev, gpointer data) {
    auto* ui = static_cast<PandaBrowserUI*>(data);
    const guint mods = ev->state & gtk_accelerator_get_default_mod_mask();

    if (mods == GDK_CONTROL_MASK) {
        if (ev->keyval == GDK_KEY_t || ev->keyval == GDK_KEY_T) {
            open_tab(ui, kHomeUrl, nullptr, true);
            gtk_widget_grab_focus(ui->url_entry);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_w || ev->keyval == GDK_KEY_W) {
            const gint idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(ui->notebook));
            if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(ui->notebook)) <= 1) {
                gtk_widget_destroy(ui->window);
            } else if (idx >= 0) {
                gtk_notebook_remove_page(GTK_NOTEBOOK(ui->notebook), idx);
                sync_toolbar(ui);
            }
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_l || ev->keyval == GDK_KEY_L ||
            ev->keyval == GDK_KEY_k || ev->keyval == GDK_KEY_K) {
            gtk_widget_grab_focus(ui->url_entry);
            gtk_editable_select_region(GTK_EDITABLE(ui->url_entry), 0, -1);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_r || ev->keyval == GDK_KEY_R) {
            reload_or_wake_active_tab(ui);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_f || ev->keyval == GDK_KEY_F) {
            toggle_find_bar(ui, true);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_equal || ev->keyval == GDK_KEY_plus || ev->keyval == GDK_KEY_KP_Add) {
            if (WebKitWebView* v = active_web_view(ui)) {
                const gdouble z = std::min(3.0, webkit_web_view_get_zoom_level(v) + 0.1);
                webkit_web_view_set_zoom_level(v, z);
            }
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_minus || ev->keyval == GDK_KEY_KP_Subtract) {
            if (WebKitWebView* v = active_web_view(ui)) {
                const gdouble z = std::max(0.3, webkit_web_view_get_zoom_level(v) - 0.1);
                webkit_web_view_set_zoom_level(v, z);
            }
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_0 || ev->keyval == GDK_KEY_KP_0) {
            if (WebKitWebView* v = active_web_view(ui)) {
                webkit_web_view_set_zoom_level(v, 1.0);
            }
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_Tab || ev->keyval == GDK_KEY_Page_Down) {
            const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(ui->notebook));
            if (n > 1) {
                const gint cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(ui->notebook));
                gtk_notebook_set_current_page(GTK_NOTEBOOK(ui->notebook), (cur + 1) % n);
            }
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_Page_Up) {
            const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(ui->notebook));
            if (n > 1) {
                const gint cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(ui->notebook));
                gtk_notebook_set_current_page(GTK_NOTEBOOK(ui->notebook), (cur - 1 + n) % n);
            }
            return TRUE;
        }
    } else if (mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        if (ev->keyval == GDK_KEY_ISO_Left_Tab || ev->keyval == GDK_KEY_Tab) {
            const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(ui->notebook));
            if (n > 1) {
                const gint cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(ui->notebook));
                gtk_notebook_set_current_page(GTK_NOTEBOOK(ui->notebook), (cur - 1 + n) % n);
            }
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_B || ev->keyval == GDK_KEY_b) {
            const gboolean next = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->shield_btn));
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->shield_btn), next);
            return TRUE;
        }
    } else if (ev->keyval == GDK_KEY_F5) {
        reload_or_wake_active_tab(ui);
        return TRUE;
    } else if (ev->keyval == GDK_KEY_F12) {
        if (WebKitWebView* v = active_web_view(ui)) {
            WebKitWebInspector* inspector = webkit_web_view_get_inspector(v);
            webkit_web_inspector_show(inspector);
        }
        return TRUE;
    } else if (ev->keyval == GDK_KEY_Escape) {
        if (gtk_revealer_get_reveal_child(GTK_REVEALER(ui->find_revealer))) {
            toggle_find_bar(ui, false);
            return TRUE;
        }
        if (WebKitWebView* v = active_web_view(ui)) {
            if (webkit_web_view_is_loading(v)) {
                webkit_web_view_stop_loading(v);
                return TRUE;
            }
        }
    } else if (mods == GDK_MOD1_MASK) {
        if (ev->keyval == GDK_KEY_Left) {
            if (WebKitWebView* v = active_web_view(ui)) {
                webkit_web_view_go_back(v);
            }
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_Right) {
            if (WebKitWebView* v = active_web_view(ui)) {
                webkit_web_view_go_forward(v);
            }
            return TRUE;
        }
    }
    return FALSE;
}

void init_web_context(PandaBrowserUI* ui) {
    gchar* data_dir = g_build_filename(g_get_user_data_dir(), "panda-browser", nullptr);
    gchar* cache_dir = g_build_filename(g_get_user_cache_dir(), "panda-browser", nullptr);
    gchar* filter_dir = g_build_filename(cache_dir, "content-filters", nullptr);

    g_mkdir_with_parents(data_dir, 0700);
    g_mkdir_with_parents(cache_dir, 0700);
    g_mkdir_with_parents(filter_dir, 0700);

    WebKitWebsiteDataManager* data_mgr = webkit_website_data_manager_new(
        "base-data-directory", data_dir,
        "base-cache-directory", cache_dir,
        nullptr
    );
    // Allow cross-site OAuth flows (Google/Canva/YouTube login) without ITP breakage
    webkit_website_data_manager_set_itp_enabled(data_mgr, FALSE);

    ui->web_context = webkit_web_context_new_with_website_data_manager(data_mgr);
    g_object_unref(data_mgr);

    // Low-RAM balanced cache (multiple secondary processes is the default in WebKitGTK 2.40+)
    webkit_web_context_set_cache_model(ui->web_context, WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);

    WebKitCookieManager* cm = webkit_web_context_get_cookie_manager(ui->web_context);
    gchar* cookie_path = g_build_filename(data_dir, "cookies.sqlite", nullptr);
    webkit_cookie_manager_set_persistent_storage(cm, cookie_path, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
    g_free(cookie_path);

    g_signal_connect(ui->web_context, "download-started", G_CALLBACK(on_download_started), ui);

    // Compile lightweight native content-blocking rules asynchronously
    ui->filter_store = webkit_user_content_filter_store_new(filter_dir);
    ui->filter_cancellable = g_cancellable_new();
    GBytes* rules_bytes = g_bytes_new_static(kAdblockRulesJson, std::strlen(kAdblockRulesJson));
    webkit_user_content_filter_store_save(
        ui->filter_store,
        "panda-core-shield",
        rules_bytes,
        ui->filter_cancellable,
        on_adblock_filter_saved,
        ui
    );
    g_bytes_unref(rules_bytes);

    g_free(filter_dir);
    g_free(cache_dir);
    g_free(data_dir);
}

#endif // USE_GUI_WEBKIT

} // namespace

extern "C" {

// Initializes C++ subsystem hooks according to Gentoo-style build flags
int32_t cpp_engine_init(bool enable_simd, uint32_t max_pages) {
    if (max_pages == 0) {
        return -1;
    }
    g_simd_enabled = enable_simd;
    g_max_pages = max_pages;
#if defined(USE_V8_ENGINE)
    // Hook for V8 isolate / platform initialization
#elif defined(USE_QUICKJS_ENGINE)
    // Hook for QuickJS runtime initialization
#endif
    return 0;
}

// Single-pass HTML scanner writing normalized title and visible text into caller buffers
PandaParseStats cpp_parse_document(
    const uint8_t* html_ptr,
    size_t html_len,
    uint8_t* title_buf,
    size_t title_cap,
    uint8_t* text_buf,
    size_t text_cap
) {
    PandaParseStats stats{};
    if (title_buf && title_cap > 0) {
        title_buf[0] = '\0';
    }
    if (text_buf && text_cap > 0) {
        text_buf[0] = '\0';
    }
    if (!html_ptr || html_len == 0) {
        return stats;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const std::string_view doc(reinterpret_cast<const char*>(html_ptr), html_len);

    size_t pos = 0;
    size_t text_pos = 0;
    size_t title_pos = 0;
    bool in_title = false;

    while (pos < doc.size()) {
        const size_t lt = find_char(doc, pos, '<');
        const std::string_view chunk = (lt == std::string_view::npos)
            ? doc.substr(pos)
            : doc.substr(pos, lt - pos);

        if (!chunk.empty()) {
            emit_text_chunk(
                chunk,
                in_title,
                title_buf,
                title_cap,
                title_pos,
                text_buf,
                text_cap,
                text_pos
            );
        }

        if (lt == std::string_view::npos) {
            break;
        }

        // Skip HTML comments <!-- ... -->
        if (doc.substr(lt).starts_with("<!--")) {
            const size_t comment_end = doc.find("-->", lt + 4);
            if (comment_end == std::string_view::npos) {
                break;
            }
            pos = comment_end + 3;
            continue;
        }

        const size_t gt = find_tag_end(doc, lt + 1);
        if (gt == std::string_view::npos) {
            break;
        }

        // Parse tag name after '<' and optional '/'
        std::string_view tag_inner = doc.substr(lt + 1, gt - (lt + 1));
        while (!tag_inner.empty() && is_ascii_ws(tag_inner.front())) {
            tag_inner.remove_prefix(1);
        }

        bool closing = false;
        if (!tag_inner.empty() && tag_inner.front() == '/') {
            closing = true;
            tag_inner.remove_prefix(1);
        }
        while (!tag_inner.empty() && is_ascii_ws(tag_inner.front())) {
            tag_inner.remove_prefix(1);
        }

        const bool self_closing = !tag_inner.empty() && tag_inner.back() == '/';

        size_t name_end = 0;
        while (name_end < tag_inner.size() &&
               !is_ascii_ws(tag_inner[name_end]) &&
               tag_inner[name_end] != '/') {
            ++name_end;
        }
        const std::string_view tag_name = tag_inner.substr(0, name_end);

        if (iequals(tag_name, "title")) {
            in_title = !closing && !self_closing;
        } else if (iequals(tag_name, "script")) {
            if (!closing) {
                ++stats.script_count;
                if (!self_closing) {
                    const size_t end_pos = find_closing_tag(doc, gt + 1, "script");
                    if (end_pos == std::string_view::npos) {
                        break;
                    }
                    pos = end_pos;
                    continue;
                }
            }
        } else if (iequals(tag_name, "style") || iequals(tag_name, "noscript")) {
            if (!closing && !self_closing) {
                const size_t end_pos = find_closing_tag(doc, gt + 1, tag_name);
                if (end_pos == std::string_view::npos) {
                    break;
                }
                pos = end_pos;
                continue;
            }
        } else if (iequals(tag_name, "a") && !closing) {
            ++stats.link_count;
        } else if (is_block_tag(tag_name) && !in_title) {
            push_char(text_buf, text_cap, text_pos, ' ', true);
        } else if (starts_with_ci(tag_name, "!doctype")) {
            // Skip DOCTYPE declarations cleanly
        }

        pos = gt + 1;
    }

    trim_trailing_space(title_buf, title_pos);
    trim_trailing_space(text_buf, text_pos);

    if (title_buf && title_cap > 0) {
        title_buf[title_pos] = '\0';
    }
    if (text_buf && text_cap > 0) {
        text_buf[text_pos] = '\0';
    }

    const auto t1 = std::chrono::steady_clock::now();
    stats.title_len = title_pos;
    stats.text_len = text_pos;
    stats.parse_us = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()
    );
    return stats;
}

// Formats script output prefixed by the active compile-time JS engine tag
size_t cpp_eval_script(
    const uint8_t* script_ptr,
    size_t script_len,
    uint8_t* out_buf,
    size_t out_cap
) {
    if (!out_buf || out_cap == 0) {
        return 0;
    }
    out_buf[0] = '\0';
    if (!script_ptr && script_len > 0) {
        return 0;
    }

    const std::string_view src = script_ptr
        ? std::string_view(reinterpret_cast<const char*>(script_ptr), script_len)
        : std::string_view{};

#if defined(USE_V8_ENGINE)
    const char* engine_tag = "[V8-JIT] ";
#elif defined(USE_QUICKJS_ENGINE)
    const char* engine_tag = "[QuickJS] ";
#else
    const char* engine_tag = "[No-JS] ";
#endif

    const size_t tag_len = std::strlen(engine_tag);
    const size_t copy_tag = std::min(tag_len, out_cap - 1);
    std::memcpy(out_buf, engine_tag, copy_tag);

    const size_t rem = (out_cap - 1) - copy_tag;
    const size_t copy_src = std::min(src.size(), rem);
    if (copy_src > 0) {
        std::memcpy(out_buf + copy_tag, src.data(), copy_src);
    }
    out_buf[copy_tag + copy_src] = '\0';
    return copy_tag + copy_src;
}

int32_t cpp_launch_gui(const char* initial_url) {
#if defined(USE_GUI_WEBKIT)
    // Prevent WebKitGTK DMABUF white-screen/GPU crashes on Linux while respecting user overrides
    g_setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", FALSE);

    g_set_prgname("panda-browser");
    g_set_application_name("Panda Browser");

    if (!gtk_init_check(nullptr, nullptr)) {
        return -1;
    }

    PandaBrowserUI ui{};
    init_web_context(&ui);

    ui.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(ui.window), 1280, 800);
    gtk_window_set_title(GTK_WINDOW(ui.window), "Panda Browser");
    gtk_window_set_icon_name(GTK_WINDOW(ui.window), "web-browser");

    GtkWidget* root_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(ui.window), root_vbox);

    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 4);
    gtk_box_pack_start(GTK_BOX(root_vbox), toolbar, FALSE, FALSE, 0);

    ui.back_btn = gtk_button_new_from_icon_name("go-previous-symbolic", GTK_ICON_SIZE_BUTTON);
    ui.fwd_btn = gtk_button_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_BUTTON);
    ui.reload_btn = gtk_button_new();
    ui.reload_icon = gtk_image_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(ui.reload_btn), ui.reload_icon);

    ui.shield_btn = gtk_toggle_button_new();
    GtkWidget* shield_icon = gtk_image_new_from_icon_name("security-high-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(ui.shield_btn), shield_icon);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.shield_btn), TRUE);
    gtk_widget_set_tooltip_text(ui.shield_btn, "Ad & Tracker Shield (Ctrl+Shift+B)");

    GtkWidget* new_tab_btn = gtk_button_new_from_icon_name("tab-new-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(new_tab_btn, "New Tab (Ctrl+T)");

    ui.url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.url_entry), "Search or enter web address...");

    gtk_box_pack_start(GTK_BOX(toolbar), ui.back_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), ui.fwd_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), ui.reload_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), ui.url_entry, TRUE, TRUE, 4);
    gtk_box_pack_end(GTK_BOX(toolbar), new_tab_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(toolbar), ui.shield_btn, FALSE, FALSE, 0);

    // Find-in-Page bar (hidden until Ctrl+F)
    ui.find_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(ui.find_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    GtkWidget* find_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(find_bar), 4);
    ui.find_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.find_entry), "Find in page...");
    GtkWidget* find_prev_btn = gtk_button_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget* find_next_btn = gtk_button_new_from_icon_name("go-down-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget* find_close_btn = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(GTK_BOX(find_bar), ui.find_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(find_bar), find_prev_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(find_bar), find_next_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(find_bar), find_close_btn, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(ui.find_revealer), find_bar);
    gtk_box_pack_start(GTK_BOX(root_vbox), ui.find_revealer, FALSE, FALSE, 0);

    ui.notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(ui.notebook), TRUE);
    gtk_box_pack_start(GTK_BOX(root_vbox), ui.notebook, TRUE, TRUE, 0);

    // Subtle status bar (only visible during downloads, tab sleep, or warnings)
    ui.status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(ui.status_label), 0.0f);
    gtk_widget_set_margin_start(ui.status_label, 8);
    gtk_widget_set_margin_end(ui.status_label, 8);
    gtk_widget_set_margin_top(ui.status_label, 2);
    gtk_widget_set_margin_bottom(ui.status_label, 2);
    gtk_box_pack_end(GTK_BOX(root_vbox), ui.status_label, FALSE, FALSE, 0);

    g_signal_connect(
        ui.window,
        "destroy",
        G_CALLBACK(+[](GtkWidget*, gpointer data) {
            auto* ui_ptr = static_cast<PandaBrowserUI*>(data);
            ui_ptr->window = nullptr;
            gtk_main_quit();
        }),
        &ui
    );
    g_signal_connect(ui.window, "key-press-event", G_CALLBACK(on_window_key_press), &ui);

    g_signal_connect(
        ui.back_btn,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            if (WebKitWebView* v = active_web_view(static_cast<PandaBrowserUI*>(data))) {
                webkit_web_view_go_back(v);
            }
        }),
        &ui
    );
    g_signal_connect(
        ui.fwd_btn,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            if (WebKitWebView* v = active_web_view(static_cast<PandaBrowserUI*>(data))) {
                webkit_web_view_go_forward(v);
            }
        }),
        &ui
    );
    g_signal_connect(
        ui.reload_btn,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            reload_or_wake_active_tab(static_cast<PandaBrowserUI*>(data));
        }),
        &ui
    );
    g_signal_connect(
        ui.shield_btn,
        "toggled",
        G_CALLBACK(+[](GtkToggleButton* btn, gpointer data) {
            auto* ui_ptr = static_cast<PandaBrowserUI*>(data);
            ui_ptr->adblock_enabled = gtk_toggle_button_get_active(btn) == TRUE;
            apply_filter_to_all_tabs(ui_ptr);
            set_status_message(
                ui_ptr,
                ui_ptr->adblock_enabled ? "Shield ON: blocking third-party trackers" : "Shield OFF"
            );
        }),
        &ui
    );
    g_signal_connect(
        new_tab_btn,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* ui_ptr = static_cast<PandaBrowserUI*>(data);
            open_tab(ui_ptr, kHomeUrl, nullptr, true);
            gtk_widget_grab_focus(ui_ptr->url_entry);
        }),
        &ui
    );
    g_signal_connect(
        ui.url_entry,
        "activate",
        G_CALLBACK(+[](GtkEntry* entry, gpointer data) {
            auto* ui_ptr = static_cast<PandaBrowserUI*>(data);
            if (WebKitWebView* v = active_web_view(ui_ptr)) {
                load_user_input(v, gtk_entry_get_text(entry));
                gtk_widget_grab_focus(GTK_WIDGET(v));
            }
        }),
        &ui
    );
    g_signal_connect_after(
        ui.notebook,
        "switch-page",
        G_CALLBACK(+[](GtkNotebook*, GtkWidget* page, guint, gpointer data) {
            auto* ui_ptr = static_cast<PandaBrowserUI*>(data);
            if (page) {
                wake_tab_if_suspended(WEBKIT_WEB_VIEW(page));
            }
            sync_toolbar(ui_ptr);
        }),
        &ui
    );

    // Find-in-Page signals
    g_signal_connect(
        ui.find_entry,
        "search-changed",
        G_CALLBACK(+[](GtkSearchEntry* entry, gpointer data) {
            auto* ui_ptr = static_cast<PandaBrowserUI*>(data);
            if (WebKitWebView* v = active_web_view(ui_ptr)) {
                WebKitFindController* fc = webkit_web_view_get_find_controller(v);
                const gchar* txt = gtk_entry_get_text(GTK_ENTRY(entry));
                if (!txt || txt[0] == '\0') {
                    webkit_find_controller_search_finish(fc);
                } else {
                    webkit_find_controller_search(
                        fc,
                        txt,
                        WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND,
                        1000
                    );
                }
            }
        }),
        &ui
    );
    g_signal_connect(
        ui.find_entry,
        "activate",
        G_CALLBACK(+[](GtkEntry*, gpointer data) {
            if (WebKitWebView* v = active_web_view(static_cast<PandaBrowserUI*>(data))) {
                webkit_find_controller_search_next(webkit_web_view_get_find_controller(v));
            }
        }),
        &ui
    );
    g_signal_connect(
        find_next_btn,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            if (WebKitWebView* v = active_web_view(static_cast<PandaBrowserUI*>(data))) {
                webkit_find_controller_search_next(webkit_web_view_get_find_controller(v));
            }
        }),
        &ui
    );
    g_signal_connect(
        find_prev_btn,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            if (WebKitWebView* v = active_web_view(static_cast<PandaBrowserUI*>(data))) {
                webkit_find_controller_search_previous(webkit_web_view_get_find_controller(v));
            }
        }),
        &ui
    );
    g_signal_connect(
        find_close_btn,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            toggle_find_bar(static_cast<PandaBrowserUI*>(data), false);
        }),
        &ui
    );

    const char* start_url = (initial_url && initial_url[0] != '\0')
        ? initial_url
        : kHomeUrl;
    open_tab(&ui, start_url, nullptr, true);

    gtk_widget_show_all(ui.window);
    gtk_widget_hide(ui.status_label);
    gtk_main();

    if (ui.filter_cancellable) {
        g_cancellable_cancel(ui.filter_cancellable);
        g_object_unref(ui.filter_cancellable);
    }
    if (ui.adblock_filter) {
        webkit_user_content_filter_unref(ui.adblock_filter);
    }
    if (ui.filter_store) {
        g_object_unref(ui.filter_store);
    }
    if (ui.web_context) {
        g_object_unref(ui.web_context);
    }
    return 0;
#else
    (void)initial_url;
    return -1;
#endif
}

// Cleans up C++ engine state during shutdown
void cpp_engine_shutdown(void) {
    g_simd_enabled = false;
}

} // extern "C"
