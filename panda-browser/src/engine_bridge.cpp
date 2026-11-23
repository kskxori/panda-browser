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

bool starts_with_ci(std::string_view sv, std::string_view prefix) noexcept {
    if (sv.size() < prefix.size()) {
        return false;
    }
    return iequals(sv.substr(0, prefix.size()), prefix);
}

// Fast character scan: uses libc SIMD-accelerated memchr when enabled
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

struct PandaBrowserUI {
    GtkWidget* window = nullptr;
    GtkWidget* notebook = nullptr;
    GtkWidget* back_btn = nullptr;
    GtkWidget* fwd_btn = nullptr;
    GtkWidget* reload_btn = nullptr;
    GtkWidget* url_entry = nullptr;
};

struct TabWidgets {
    PandaBrowserUI* ui = nullptr;
    GtkWidget* web_view = nullptr;
    GtkWidget* tab_label = nullptr;
};

WebKitWebView* active_web_view(PandaBrowserUI* ui) {
    const gint page = gtk_notebook_get_current_page(GTK_NOTEBOOK(ui->notebook));
    if (page < 0) {
        return nullptr;
    }
    GtkWidget* child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(ui->notebook), page);
    return child ? WEBKIT_WEB_VIEW(child) : nullptr;
}

void sync_toolbar(PandaBrowserUI* ui) {
    WebKitWebView* view = active_web_view(ui);
    if (!view) {
        gtk_window_set_title(GTK_WINDOW(ui->window), "Panda Browser");
        gtk_entry_set_text(GTK_ENTRY(ui->url_entry), "");
        gtk_entry_set_progress_fraction(GTK_ENTRY(ui->url_entry), 0.0);
        gtk_widget_set_sensitive(ui->back_btn, FALSE);
        gtk_widget_set_sensitive(ui->fwd_btn, FALSE);
        return;
    }

    const gchar* uri = webkit_web_view_get_uri(view);
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
    const gdouble progress = loading ? webkit_web_view_get_estimated_load_progress(view) : 0.0;
    gtk_entry_set_progress_fraction(GTK_ENTRY(ui->url_entry), progress);
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

    if (starts_with_ci(sv, "http://") || starts_with_ci(sv, "https://") ||
        starts_with_ci(sv, "file://") || starts_with_ci(sv, "about:")) {
        gchar* exact = g_strndup(sv.data(), sv.size());
        webkit_web_view_load_uri(view, exact);
        g_free(exact);
        return;
    }

    // Domain heuristic: no whitespace and contains '.' or starts with "localhost"
    const bool has_space = (sv.find(' ') != std::string_view::npos);
    const bool looks_like_host = !has_space &&
        (sv.find('.') != std::string_view::npos || starts_with_ci(sv, "localhost"));

    if (looks_like_host) {
        gchar* host = g_strndup(sv.data(), sv.size());
        gchar* url = g_strdup_printf("https://%s", host);
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

void on_tab_title_changed(WebKitWebView* view, GParamSpec*, gpointer data) {
    auto* tw = static_cast<TabWidgets*>(data);
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

void on_close_tab_clicked(GtkButton*, gpointer data) {
    auto* tw = static_cast<TabWidgets*>(data);
    const gint idx = gtk_notebook_page_num(GTK_NOTEBOOK(tw->ui->notebook), tw->web_view);
    if (idx < 0) {
        return;
    }
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(tw->ui->notebook)) <= 1) {
        gtk_widget_destroy(tw->ui->window);
        return;
    }
    gtk_notebook_remove_page(GTK_NOTEBOOK(tw->ui->notebook), idx);
    sync_toolbar(tw->ui);
}

void on_tab_widget_destroy(GtkWidget*, gpointer data) {
    g_free(data);
}

GtkWidget* open_tab(PandaBrowserUI* ui, const char* url, WebKitWebView* related_view) {
    GtkWidget* view_widget = related_view
        ? webkit_web_view_new_with_related_view(related_view)
        : webkit_web_view_new();
    WebKitWebView* view = WEBKIT_WEB_VIEW(view_widget);

    WebKitSettings* settings = webkit_web_view_get_settings(view);
    webkit_settings_set_enable_developer_extras(settings, TRUE);
    webkit_settings_set_enable_smooth_scrolling(settings, TRUE);

    auto* tw = static_cast<TabWidgets*>(g_malloc0(sizeof(TabWidgets)));
    tw->ui = ui;
    tw->web_view = view_widget;

    GtkWidget* tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    tw->tab_label = gtk_label_new("New Tab");
    gtk_label_set_ellipsize(GTK_LABEL(tw->tab_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(tw->tab_label), 16);

    GtkWidget* close_btn = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close_btn), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(close_btn, FALSE);

    gtk_box_pack_start(GTK_BOX(tab_box), tw->tab_label, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(tab_box), close_btn, FALSE, FALSE, 0);
    gtk_widget_show_all(tab_box);

    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_tab_clicked), tw);
    g_signal_connect(view_widget, "destroy", G_CALLBACK(on_tab_widget_destroy), tw);
    g_signal_connect(view, "notify::title", G_CALLBACK(on_tab_title_changed), tw);
    g_signal_connect(view, "notify::uri", G_CALLBACK(on_tab_state_changed), tw);
    g_signal_connect(view, "notify::is-loading", G_CALLBACK(on_tab_state_changed), tw);
    g_signal_connect(view, "notify::estimated-load-progress", G_CALLBACK(on_tab_state_changed), tw);

    // Handle target="_blank" / window.open() in a new tab
    g_signal_connect(
        view,
        "create",
        G_CALLBACK(+[](WebKitWebView* parent, WebKitNavigationAction*, gpointer user_data) -> GtkWidget* {
            auto* ui_ptr = static_cast<PandaBrowserUI*>(user_data);
            return open_tab(ui_ptr, nullptr, parent);
        }),
        ui
    );

    const gint idx = gtk_notebook_append_page(GTK_NOTEBOOK(ui->notebook), view_widget, tab_box);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(ui->notebook), view_widget, TRUE);
    gtk_widget_show(view_widget);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(ui->notebook), idx);

    if (url && url[0] != '\0') {
        load_user_input(view, url);
    }
    sync_toolbar(ui);
    return view_widget;
}

gboolean on_window_key_press(GtkWidget*, GdkEventKey* ev, gpointer data) {
    auto* ui = static_cast<PandaBrowserUI*>(data);
    const guint mods = ev->state & gtk_accelerator_get_default_mod_mask();

    if (mods == GDK_CONTROL_MASK) {
        if (ev->keyval == GDK_KEY_t || ev->keyval == GDK_KEY_T) {
            open_tab(ui, "https://duckduckgo.com", nullptr);
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
        if (ev->keyval == GDK_KEY_l || ev->keyval == GDK_KEY_L) {
            gtk_widget_grab_focus(ui->url_entry);
            gtk_editable_select_region(GTK_EDITABLE(ui->url_entry), 0, -1);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_r || ev->keyval == GDK_KEY_R) {
            if (WebKitWebView* v = active_web_view(ui)) {
                webkit_web_view_reload(v);
            }
            return TRUE;
        }
    } else if (ev->keyval == GDK_KEY_F5) {
        if (WebKitWebView* v = active_web_view(ui)) {
            webkit_web_view_reload(v);
        }
        return TRUE;
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
    g_set_prgname("panda-browser");
    g_set_application_name("Panda Browser");

    if (!gtk_init_check(nullptr, nullptr)) {
        return -1;
    }

    PandaBrowserUI ui{};
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
    ui.reload_btn = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget* new_tab_btn = gtk_button_new_from_icon_name("tab-new-symbolic", GTK_ICON_SIZE_BUTTON);

    ui.url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.url_entry), "Search or enter web address...");

    gtk_box_pack_start(GTK_BOX(toolbar), ui.back_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), ui.fwd_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), ui.reload_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), ui.url_entry, TRUE, TRUE, 4);
    gtk_box_pack_end(GTK_BOX(toolbar), new_tab_btn, FALSE, FALSE, 0);

    ui.notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(ui.notebook), TRUE);
    gtk_box_pack_start(GTK_BOX(root_vbox), ui.notebook, TRUE, TRUE, 0);

    g_signal_connect(ui.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
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
            if (WebKitWebView* v = active_web_view(static_cast<PandaBrowserUI*>(data))) {
                if (webkit_web_view_is_loading(v)) {
                    webkit_web_view_stop_loading(v);
                } else {
                    webkit_web_view_reload(v);
                }
            }
        }),
        &ui
    );
    g_signal_connect(
        new_tab_btn,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* ui_ptr = static_cast<PandaBrowserUI*>(data);
            open_tab(ui_ptr, "https://duckduckgo.com", nullptr);
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
        G_CALLBACK(+[](GtkNotebook*, GtkWidget*, guint, gpointer data) {
            sync_toolbar(static_cast<PandaBrowserUI*>(data));
        }),
        &ui
    );

    const char* start_url = (initial_url && initial_url[0] != '\0')
        ? initial_url
        : "https://duckduckgo.com";
    open_tab(&ui, start_url, nullptr);

    gtk_widget_show_all(ui.window);
    gtk_main();
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
