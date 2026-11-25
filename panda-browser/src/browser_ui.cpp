#include "browser_state.hpp"
#include "browser_ui.hpp"
#include "content_blocker.hpp"
#include "profile_store.hpp"

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace panda {

namespace {

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

gchar* escape_html(const char* raw) {
    return g_markup_escape_text(raw ? raw : "", -1);
}

std::string js_quote(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() + 8);
    out.push_back('"');
    for (unsigned char c : sv) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '<':  out += "\\u003c"; break;
            case '>':  out += "\\u003e"; break;
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
    out.clear();
    std::string needle = "\"" + std::string(key) + "\"";
    const size_t pos = json.find(needle);
    if (pos == std::string_view::npos) return false;
    size_t i = pos + needle.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == ':')) ++i;
    if (i >= json.size() || json[i] != '"') return false;
    ++i;
    while (i < json.size()) {
        const char ch = json[i++];
        if (ch == '"') return true;
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

void update_downloads_button_label(BrowserWindow* win) {
    if (!win || !win->downloads_btn) return;
    int active_count = 0;
    for (const auto& dl : win->downloads) {
        if (!dl.finished && !dl.failed) ++active_count;
    }
    if (active_count > 0) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "Downloads (%d)", active_count);
        gtk_button_set_label(GTK_BUTTON(win->downloads_btn), buf);
    } else {
        gtk_button_set_label(GTK_BUTTON(win->downloads_btn), "Downloads");
    }
}

bool is_local_host_input(std::string_view sv) noexcept {
    return starts_with_ci(sv, "localhost") ||
           sv.rfind("127.", 0) == 0 ||
           sv.rfind("192.168.", 0) == 0 ||
           sv.rfind("10.", 0) == 0 ||
           sv.rfind("[::1]", 0) == 0;
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
            if (e.password == password) return;
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
    if (td && td->suspended) return;
    if (reason == WEBKIT_WEB_PROCESS_TERMINATED_BY_API) return;
    const char* msg = (reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT)
        ? "This tab exceeded its memory limit and was closed to protect system RAM."
        : "The renderer process stopped unexpectedly.";
    show_error_page(view, "Tab Renderer Stopped", webkit_web_view_get_uri(view), msg);
}

DownloadRecord* find_download(BrowserWindow* win, uint64_t id) {
    for (auto& d : win->downloads) {
        if (d.id == id) return &d;
    }
    return nullptr;
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
        const uint64_t dl_id = static_cast<uint64_t>(GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(download), "panda-dl-id")));
        if (DownloadRecord* rec = find_download(win, dl_id)) {
            rec->filename = base_name;
            rec->dest_path = dest_path;
        }
        gchar* msg = g_strdup_printf("Downloading: %s (0%%)", base_name);
        set_status_message(win, msg);
        g_free(msg);
        update_downloads_button_label(win);
        if (win->downloads_popover) refresh_downloads_popover(win);
        g_free(dest_uri);
    }

    g_free(dest_path);
    g_free(base_name);
    return TRUE;
}

void on_download_started(WebKitWebContext*, WebKitDownload* download, gpointer data) {
    auto* win = static_cast<BrowserWindow*>(data);
    const uint64_t dl_id = win->next_download_id++;
    g_object_set_data(G_OBJECT(download), "panda-dl-id", GSIZE_TO_POINTER(static_cast<gsize>(dl_id)));

    win->downloads.push_back({dl_id, "Starting download...", "", "", 0, false, false});
    if (win->downloads.size() > 25) {
        win->downloads.erase(win->downloads.begin());
    }
    update_downloads_button_label(win);

    g_signal_connect(download, "decide-destination", G_CALLBACK(on_download_decide_destination), win);
    g_signal_connect(
        download,
        "notify::estimated-progress",
        G_CALLBACK(+[](WebKitDownload* dl, GParamSpec*, gpointer d) {
            auto* w = static_cast<BrowserWindow*>(d);
            const char* name = static_cast<const char*>(g_object_get_data(G_OBJECT(dl), "panda-dl-name"));
            const int pct = static_cast<int>(webkit_download_get_estimated_progress(dl) * 100.0);
            const uint64_t id = static_cast<uint64_t>(GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(dl), "panda-dl-id")));
            if (DownloadRecord* rec = find_download(w, id)) {
                rec->progress_pct = pct;
            }
            gchar* msg = g_strdup_printf("Downloading %s (%d%%)", name ? name : "file", pct);
            set_status_message(w, msg);
            g_free(msg);
            if (w->downloads_popover) refresh_downloads_popover(w);
        }),
        win
    );
    g_signal_connect(
        download,
        "finished",
        G_CALLBACK(+[](WebKitDownload* dl, gpointer d) {
            auto* w = static_cast<BrowserWindow*>(d);
            const char* name = static_cast<const char*>(g_object_get_data(G_OBJECT(dl), "panda-dl-name"));
            const uint64_t id = static_cast<uint64_t>(GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(dl), "panda-dl-id")));
            if (DownloadRecord* rec = find_download(w, id)) {
                rec->progress_pct = 100;
                rec->finished = true;
            }
            gchar* msg = g_strdup_printf("Saved to Downloads: %s", name ? name : "file");
            set_status_message(w, msg);
            g_free(msg);
            update_downloads_button_label(w);
            if (w->downloads_popover) refresh_downloads_popover(w);
        }),
        win
    );
    g_signal_connect(
        download,
        "failed",
        G_CALLBACK(+[](WebKitDownload* dl, GError* err, gpointer d) {
            auto* w = static_cast<BrowserWindow*>(d);
            const uint64_t id = static_cast<uint64_t>(GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(dl), "panda-dl-id")));
            if (DownloadRecord* rec = find_download(w, id)) {
                rec->failed = true;
                rec->error_msg = err ? err->message : "unknown error";
            }
            gchar* msg = g_strdup_printf("Download failed: %s", err ? err->message : "unknown error");
            set_status_message(w, msg);
            g_free(msg);
            update_downloads_button_label(w);
            if (w->downloads_popover) refresh_downloads_popover(w);
        }),
        win
    );
}

void wake_tab_if_suspended(TabData* td) {
    if (!td) return;
    td->last_active_sec = monotonic_seconds();
    if (!td->suspended) return;

    gchar* url = td->suspended_url;
    td->suspended_url = nullptr;
    td->suspended = false;
    if (url) {
        load_user_input(td, url);
        g_free(url);
    }
    sync_toolbar(td->win);
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
        if (!cur_uri.empty()) {
            win->closed_tabs.push_back(std::move(cur_uri));
            if (win->closed_tabs.size() > 20) {
                win->closed_tabs.erase(win->closed_tabs.begin());
            }
        }
    }

    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook)) <= 1) {
        if (td) show_internal_view(td, win->config.home_url);
        return;
    }
    gtk_notebook_remove_page(GTK_NOTEBOOK(win->notebook), idx);
    sync_toolbar(win);
}

void suspend_tab(TabData* td) {
    if (!td || td->suspended || td->pinned || !td->web_view || !td->internal_uri.empty()) return;
    WebKitWebView* view = WEBKIT_WEB_VIEW(td->web_view);
    if (webkit_web_view_is_playing_audio(view)) return;

    const gchar* uri = webkit_web_view_get_uri(view);
    if (!uri || uri[0] == '\0' || starts_with_ci(uri, "panda://")) return;

    const gchar* title = webkit_web_view_get_title(view);
    td->suspended_url = g_strdup(uri);
    td->suspended = true;
    td->last_applied_host.clear();
    update_tab_label_text(td, (title && title[0] != '\0') ? title : uri);

#if WEBKIT_CHECK_VERSION(2, 34, 0)
    webkit_web_view_terminate_web_process(view);
#else
    webkit_web_view_load_html(view, "<html><body style='background:#111317'></body></html>", "about:blank");
#endif
}

void toggle_pin_tab(TabData* td) {
    if (!td) return;
    td->pinned = !td->pinned;
    gtk_label_set_width_chars(GTK_LABEL(td->tab_label), td->pinned ? 10 : 15);
    std::string cur = td->tab_label ? gtk_label_get_text(GTK_LABEL(td->tab_label)) : "";
    if (cur.rfind("[*] ", 0) == 0) cur.erase(0, 4);
    if (cur.rfind("[zzz] ", 0) == 0) cur.erase(0, 6);
    update_tab_label_text(td, cur.c_str());
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
    if (!uri || uri[0] == '\0' || starts_with_ci(uri, "panda://")) return;
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

    g_signal_connect(view_widget, "button-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton* ev, gpointer d) -> gboolean {
        auto* t = static_cast<TabData*>(d);
        if (ev->button == 8) {
            go_back_active_tab(t->win);
            return TRUE;
        }
        if (ev->button == 9) {
            go_forward_active_tab(t->win);
            return TRUE;
        }
        return FALSE;
    }), td);

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
        apply_site_prefs_to_tab(t);
        if (active_tab(t->win) == t) {
            sync_toolbar(t->win);
        }
    }), td);

    g_signal_connect(view, "load-changed", G_CALLBACK(+[](WebKitWebView* v, WebKitLoadEvent ev, gpointer d) {
        auto* t = static_cast<TabData*>(d);
        if (ev == WEBKIT_LOAD_STARTED || ev == WEBKIT_LOAD_COMMITTED) {
            apply_site_prefs_to_tab(t);
        }
        if (ev == WEBKIT_LOAD_COMMITTED) {
            record_history_for_view(t, v);
        } else if (ev == WEBKIT_LOAD_FINISHED) {
            apply_site_prefs_to_tab(t);
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
        if (ev->keyval == GDK_KEY_p || ev->keyval == GDK_KEY_P) {
            print_active_tab(win);
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
                set_active_tab_zoom(win, std::min(3.0, webkit_web_view_get_zoom_level(v) + 0.1));
            }
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_minus || ev->keyval == GDK_KEY_KP_Subtract) {
            if (WebKitWebView* v = active_web_view(win)) {
                set_active_tab_zoom(win, std::max(0.3, webkit_web_view_get_zoom_level(v) - 0.1));
            }
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_0 || ev->keyval == GDK_KEY_KP_0) {
            set_active_tab_zoom(win, 1.0);
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
#if WEBKIT_CHECK_VERSION(2, 34, 0)
    WebKitMemoryPressureSettings* mps = webkit_memory_pressure_settings_new();
    guint mem_limit_mb = 320;
    gdouble poll_sec = 10.0;
    gdouble cons_thresh = 0.40;
    gdouble strict_thresh = 0.65;
    if (win->config.cache_profile == CacheProfile::Balanced) {
        mem_limit_mb = 640;
        poll_sec = 15.0;
        cons_thresh = 0.55;
        strict_thresh = 0.78;
    } else if (win->config.cache_profile == CacheProfile::Fast) {
        mem_limit_mb = 1280;
        poll_sec = 20.0;
        cons_thresh = 0.70;
        strict_thresh = 0.85;
    }
    webkit_memory_pressure_settings_set_memory_limit(mps, mem_limit_mb);
    webkit_memory_pressure_settings_set_poll_interval(mps, poll_sec);
    webkit_memory_pressure_settings_set_conservative_threshold(mps, cons_thresh);
    webkit_memory_pressure_settings_set_strict_threshold(mps, strict_thresh);
    webkit_memory_pressure_settings_set_kill_threshold(mps, 0.96);
    webkit_website_data_manager_set_memory_pressure_settings(mps);

    if (win->is_private) {
        WebKitWebsiteDataManager* eph_mgr = webkit_website_data_manager_new_ephemeral();
        win->web_context = WEBKIT_WEB_CONTEXT(g_object_new(
            WEBKIT_TYPE_WEB_CONTEXT,
            "website-data-manager", eph_mgr,
            "memory-pressure-settings", mps,
            nullptr
        ));
        g_object_unref(eph_mgr);
    } else {
        WebKitWebsiteDataManager* data_mgr = webkit_website_data_manager_new(
            "base-data-directory", win->store.data_dir().c_str(),
            "base-cache-directory", win->store.cache_dir().c_str(),
            nullptr
        );
        webkit_website_data_manager_set_itp_enabled(data_mgr, FALSE);
        win->web_context = WEBKIT_WEB_CONTEXT(g_object_new(
            WEBKIT_TYPE_WEB_CONTEXT,
            "website-data-manager", data_mgr,
            "memory-pressure-settings", mps,
            nullptr
        ));
        g_object_unref(data_mgr);

        WebKitCookieManager* cm = webkit_web_context_get_cookie_manager(win->web_context);
        std::string cookie_path = win->store.data_dir() + "/cookies.sqlite";
        webkit_cookie_manager_set_persistent_storage(cm, cookie_path.c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
        webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
    }
    webkit_memory_pressure_settings_free(mps);
#else
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
#endif

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

} // namespace

// ============================================================================
// Shared utility & per-site preference helpers declared in browser_state.hpp
// ============================================================================

gint64 monotonic_seconds() noexcept {
    return g_get_monotonic_time() / G_USEC_PER_SEC;
}

bool ascii_iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) return false;
    }
    return true;
}

bool starts_with_ci(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && ascii_iequals(s.substr(0, prefix.size()), prefix);
}

std::string extract_origin(const char* uri) {
    if (!uri) return {};
    std::string_view sv(uri);
    const size_t scheme_end = sv.find("://");
    if (scheme_end == std::string_view::npos) return {};
    const std::string_view scheme = sv.substr(0, scheme_end);
    if (!ascii_iequals(scheme, "http") && !ascii_iequals(scheme, "https")) {
        return {};
    }
    const size_t host_start = scheme_end + 3;
    size_t path_start = sv.find('/', host_start);
    if (path_start == std::string_view::npos) path_start = sv.size();
    const size_t q_start = sv.find('?', host_start);
    if (q_start < path_start) path_start = q_start;
    if (path_start <= host_start) return {};

    std::string origin;
    origin.reserve(path_start);
    for (size_t i = 0; i < path_start; ++i) {
        origin.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(sv[i]))));
    }
    return origin;
}

std::string extract_host(const char* uri) {
    if (!uri) return {};
    std::string_view sv(uri);
    const size_t scheme_end = sv.find("://");
    if (scheme_end == std::string_view::npos) return {};
    const std::string_view scheme = sv.substr(0, scheme_end);
    if (!ascii_iequals(scheme, "http") && !ascii_iequals(scheme, "https")) {
        return {};
    }
    size_t host_start = scheme_end + 3;
    size_t authority_end = sv.find('/', host_start);
    if (authority_end == std::string_view::npos) authority_end = sv.size();
    const size_t q_pos = sv.find('?', host_start);
    if (q_pos < authority_end) authority_end = q_pos;
    if (authority_end <= host_start) return {};

    std::string_view auth = sv.substr(host_start, authority_end - host_start);
    const size_t at_pos = auth.rfind('@');
    if (at_pos != std::string_view::npos) {
        auth = auth.substr(at_pos + 1);
    }
    if (auth.empty()) return {};
    if (auth.front() == '[') {
        const size_t close_br = auth.find(']');
        if (close_br != std::string_view::npos) {
            auth = auth.substr(0, close_br + 1);
        }
    } else {
        const size_t colon = auth.rfind(':');
        if (colon != std::string_view::npos) {
            auth = auth.substr(0, colon);
        }
    }
    std::string out;
    out.reserve(auth.size());
    for (char ch : auth) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
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

SitePref* find_site_pref(BrowserWindow* win, std::string_view host) {
    if (!win || host.empty()) return nullptr;
    for (auto& sp : win->site_prefs) {
        if (sp.host == host) return &sp;
    }
    return nullptr;
}

SitePref& upsert_site_pref(BrowserWindow* win, const std::string& host) {
    if (SitePref* existing = find_site_pref(win, host)) {
        return *existing;
    }
    win->site_prefs.push_back({host, -1, -1, 1.0});
    return win->site_prefs.back();
}

bool effective_shield_for_host(const BrowserWindow* win, std::string_view host) {
    if (!win) return true;
    if (!host.empty()) {
        for (const auto& sp : win->site_prefs) {
            if (sp.host == host && sp.shield_mode != -1) {
                return sp.shield_mode == 1;
            }
        }
    }
    return win->config.shield_enabled;
}

bool effective_dark_for_host(const BrowserWindow* win, std::string_view host) {
    if (!win) return false;
    if (!host.empty()) {
        for (const auto& sp : win->site_prefs) {
            if (sp.host == host && sp.dark_mode != -1) {
                return sp.dark_mode == 1;
            }
        }
    }
    return win->config.force_dark;
}

double effective_zoom_for_host(const BrowserWindow* win, std::string_view host) {
    if (!win || host.empty()) return 1.0;
    for (const auto& sp : win->site_prefs) {
        if (sp.host == host) return sp.zoom_level;
    }
    return 1.0;
}

void apply_site_prefs_to_tab(TabData* td) {
    if (!td || !td->win || !td->web_view || !td->internal_uri.empty()) return;
    WebKitWebView* view = WEBKIT_WEB_VIEW(td->web_view);
    const std::string host = extract_host(webkit_web_view_get_uri(view));
    if (host == td->last_applied_host) return;
    td->last_applied_host = host;

    const bool eff_shield = effective_shield_for_host(td->win, host);
    const bool eff_dark = effective_dark_for_host(td->win, host);
    const double eff_zoom = effective_zoom_for_host(td->win, host);

    td->win->blocker.apply_to_view(view, eff_shield, eff_dark);
    if (std::fabs(webkit_web_view_get_zoom_level(view) - eff_zoom) >= 0.01) {
        webkit_web_view_set_zoom_level(view, eff_zoom);
    }
}

void set_active_tab_zoom(BrowserWindow* win, double zoom) {
    if (!win) return;
    WebKitWebView* v = active_web_view(win);
    if (!v) return;
    const double clamped = std::clamp(zoom, 0.3, 3.0);
    webkit_web_view_set_zoom_level(v, clamped);

    const std::string host = extract_host(webkit_web_view_get_uri(v));
    if (!host.empty()) {
        SitePref& sp = upsert_site_pref(win, host);
        sp.zoom_level = clamped;
        if (!win->is_private) {
            win->store.save_site_prefs(win->site_prefs);
        }
    }

    char msg[48];
    std::snprintf(msg, sizeof(msg), "Zoom: %d%%", static_cast<int>(std::round(clamped * 100.0)));
    set_status_message(win, msg);
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
    gtk_label_set_text(GTK_LABEL(win->status_label), msg ? msg : "Ready");
}

void update_tab_label_text(TabData* td, const char* title) {
    if (!td || !td->tab_label) return;
    std::string text;
    if (td->pinned) text += "[*] ";
    if (td->suspended) text += "[zzz] ";
    text += (title && title[0] != '\0') ? title : "New Tab";
    gtk_label_set_text(GTK_LABEL(td->tab_label), text.c_str());
}

void sync_toolbar(BrowserWindow* win) {
    if (!win) return;
    TabData* td = active_tab(win);
    const char* suffix = win->is_private ? "Panda Browser (Private)" : "Panda Browser";

    auto set_padlock = [&](const char* icon_name, const char* tip) {
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(win->url_entry), GTK_ENTRY_ICON_PRIMARY, icon_name);
        gtk_entry_set_icon_tooltip_text(GTK_ENTRY(win->url_entry), GTK_ENTRY_ICON_PRIMARY, tip);
    };

    if (!td) {
        gtk_window_set_title(GTK_WINDOW(win->window), suffix);
        gtk_entry_set_text(GTK_ENTRY(win->url_entry), "");
        gtk_entry_set_progress_fraction(GTK_ENTRY(win->url_entry), 0.0);
        set_padlock("applications-system-symbolic", "Site Info");
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

        set_padlock("applications-system-symbolic", "Internal Panda Page");
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

    if (uri && starts_with_ci(uri, "https://")) {
        set_padlock("channel-secure-symbolic", "HTTPS Secure — Click for Site Permissions & Cookies");
    } else if (uri && starts_with_ci(uri, "http://")) {
        set_padlock("channel-insecure-symbolic", "HTTP (Not Encrypted) — Click for Site Permissions & Cookies");
    } else {
        set_padlock("folder-symbolic", "Local / Other Page");
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

void load_user_input(TabData* td, const char* raw) {
    if (!td || !raw) return;
    BrowserWindow* win = td->win;

    std::string_view sv(raw);
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t')) sv.remove_suffix(1);
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
    }
    td->forward_to_web = false;
    td->last_applied_host.clear();

    WebKitWebView* view = ensure_web_view(td, nullptr);
    if (!view) return;
    gtk_stack_set_visible_child_name(GTK_STACK(td->stack), "web");

    if (sv.find("://") != std::string_view::npos || starts_with_ci(sv, "about:") || starts_with_ci(sv, "data:")) {
        std::string target(sv);
        webkit_web_view_load_uri(view, target.c_str());
        sync_toolbar(win);
        return;
    }

    if (sv.front() == '/' || sv.rfind("~/", 0) == 0) {
        gchar* path = (sv.front() == '~')
            ? g_build_filename(g_get_home_dir(), std::string(sv.substr(2)).c_str(), nullptr)
            : g_strndup(sv.data(), sv.size());
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

void sleep_all_background_tabs(BrowserWindow* win) {
    if (!win || !win->notebook) return;
    TabData* active = active_tab(win);
    const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook));
    int count = 0;
    for (gint i = 0; i < n; ++i) {
        GtkWidget* child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(win->notebook), i);
        TabData* td = get_tab_data_from_page(child);
        if (td && td != active && !td->suspended && !td->pinned && td->web_view && td->internal_uri.empty()) {
            suspend_tab(td);
            ++count;
        }
    }
    gchar* msg = g_strdup_printf("Hibernated %d background tab(s) to save RAM", count);
    set_status_message(win, msg);
    g_free(msg);
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

void open_browser_window(const char* initial_url, uint32_t max_pages, bool is_private) {
    auto* win = new BrowserWindow();
    win->max_pages = max_pages;
    win->is_private = is_private;
    win->config = win->store.load_config();
    win->bookmarks = win->store.load_bookmarks();
    win->site_prefs = win->store.load_site_prefs();
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
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(win->url_entry), GTK_ENTRY_ICON_PRIMARY, "applications-system-symbolic");
    gtk_entry_set_icon_activatable(GTK_ENTRY(win->url_entry), GTK_ENTRY_ICON_PRIMARY, TRUE);

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

    win->pass_bar = gtk_info_bar_new();
    gtk_info_bar_set_message_type(GTK_INFO_BAR(win->pass_bar), GTK_MESSAGE_QUESTION);
    win->pass_bar_label = gtk_label_new("Save password?");
    gtk_label_set_ellipsize(GTK_LABEL(win->pass_bar_label), PANGO_ELLIPSIZE_END);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(win->pass_bar))), win->pass_bar_label);
    gtk_info_bar_add_button(GTK_INFO_BAR(win->pass_bar), "Save", GTK_RESPONSE_YES);
    gtk_info_bar_add_button(GTK_INFO_BAR(win->pass_bar), "Never for this site", GTK_RESPONSE_REJECT);
    gtk_info_bar_add_button(GTK_INFO_BAR(win->pass_bar), "Not now", GTK_RESPONSE_CANCEL);
    gtk_box_pack_start(GTK_BOX(root_vbox), win->pass_bar, FALSE, FALSE, 0);

    g_signal_connect(win->pass_bar, "response", G_CALLBACK(+[](GtkInfoBar* bar, gint resp, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        if (resp == GTK_RESPONSE_YES && !w->pending_origin.empty() && !w->pending_secret.empty()) {
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
            set_status_message(w, updated ? "Password updated in encrypted vault" : "Password saved to encrypted vault");
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
    win->downloads_btn = gtk_button_new_with_label("Downloads");
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
    g_signal_connect(win->window, "button-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton* ev, gpointer d) -> gboolean {
        auto* w = static_cast<BrowserWindow*>(d);
        if (ev->button == 8) {
            go_back_active_tab(w);
            return TRUE;
        }
        if (ev->button == 9) {
            go_forward_active_tab(w);
            return TRUE;
        }
        return FALSE;
    }), win);

    g_signal_connect(win->url_entry, "icon-press", G_CALLBACK(+[](GtkEntry*, GtkEntryIconPosition pos, GdkEvent*, gpointer d) {
        if (pos == GTK_ENTRY_ICON_PRIMARY) {
            show_site_info_popover(static_cast<BrowserWindow*>(d));
        }
    }), win);

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

    auto reapply_all_tabs = +[](BrowserWindow* w) {
        w->blocker.apply_to_all_tabs(w->config.shield_enabled, w->config.force_dark);
        const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->notebook));
        for (gint i = 0; i < n; ++i) {
            TabData* td = get_tab_data_from_page(gtk_notebook_get_nth_page(GTK_NOTEBOOK(w->notebook), i));
            if (td && td->web_view && td->internal_uri.empty()) {
                td->last_applied_host.clear();
                apply_site_prefs_to_tab(td);
            }
        }
    };

    g_signal_connect(win->shield_btn, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        w->config.shield_enabled = gtk_toggle_button_get_active(b) == TRUE;
        w->store.save_config(w->config);
        w->blocker.apply_to_all_tabs(w->config.shield_enabled, w->config.force_dark);
        const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->notebook));
        for (gint i = 0; i < n; ++i) {
            TabData* td = get_tab_data_from_page(gtk_notebook_get_nth_page(GTK_NOTEBOOK(w->notebook), i));
            if (td && td->web_view && td->internal_uri.empty()) {
                td->last_applied_host.clear();
                apply_site_prefs_to_tab(td);
            }
        }
        set_status_message(w, w->config.shield_enabled ? "Shield ON: blocking trackers" : "Shield OFF");
    }), win);
    g_signal_connect(win->dark_btn, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        w->config.force_dark = gtk_toggle_button_get_active(b) == TRUE;
        w->store.save_config(w->config);
        w->blocker.apply_to_all_tabs(w->config.shield_enabled, w->config.force_dark);
        const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->notebook));
        for (gint i = 0; i < n; ++i) {
            TabData* td = get_tab_data_from_page(gtk_notebook_get_nth_page(GTK_NOTEBOOK(w->notebook), i));
            if (td && td->web_view && td->internal_uri.empty()) {
                td->last_applied_host.clear();
                apply_site_prefs_to_tab(td);
            }
        }
    }), win);
    (void)reapply_all_tabs;

    g_signal_connect(new_tab_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        open_tab(w, w->config.home_url.c_str(), nullptr, true);
        gtk_widget_grab_focus(w->url_entry);
    }), win);
    g_signal_connect(win->menu_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        show_command_palette(static_cast<BrowserWindow*>(d));
    }), win);
    g_signal_connect(win->downloads_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        show_downloads_popover(static_cast<BrowserWindow*>(d));
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
    rebuild_bookmarks_bar(win);

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

} // namespace panda

namespace panda {

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
