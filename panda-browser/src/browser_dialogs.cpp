#include "browser_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>

namespace panda {

namespace {

constexpr WebKitWebsiteDataTypes kSiteDataTypes = static_cast<WebKitWebsiteDataTypes>(
    WEBKIT_WEBSITE_DATA_COOKIES |
    WEBKIT_WEBSITE_DATA_LOCAL_STORAGE |
    WEBKIT_WEBSITE_DATA_SESSION_STORAGE |
    WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES |
    WEBKIT_WEBSITE_DATA_DISK_CACHE |
    WEBKIT_WEBSITE_DATA_MEMORY_CACHE |
    WEBKIT_WEBSITE_DATA_OFFLINE_APPLICATION_CACHE
);

void open_local_path_uri(BrowserWindow* win, const std::string& path) {
    if (!win || path.empty()) return;
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    if (uri) {
        gtk_show_uri_on_window(GTK_WINDOW(win->window), uri, GDK_CURRENT_TIME, nullptr);
        g_free(uri);
    }
}

bool host_matches_site_data(std::string_view host, std::string_view data_name) noexcept {
    if (host.empty() || data_name.empty()) return false;
    if (ascii_iequals(host, data_name)) return true;
    if (host.size() > data_name.size() + 1 &&
        host[host.size() - data_name.size() - 1] == '.' &&
        ascii_iequals(host.substr(host.size() - data_name.size()), data_name)) {
        return true;
    }
    if (data_name.size() > host.size() + 1 &&
        data_name[data_name.size() - host.size() - 1] == '.' &&
        ascii_iequals(data_name.substr(data_name.size() - host.size()), host)) {
        return true;
    }
    return false;
}

struct SiteClearCtx {
    GWeakRef win_ref;
    BrowserWindow* win = nullptr;
    WebKitWebsiteDataManager* mgr = nullptr;
    std::string host;
};

void free_site_clear_ctx(SiteClearCtx* ctx) {
    if (!ctx) return;
    g_weak_ref_clear(&ctx->win_ref);
    if (ctx->mgr) g_object_unref(ctx->mgr);
    delete ctx;
}

void on_site_data_removed(GObject* source, GAsyncResult* res, gpointer user_data) {
    auto* ctx = static_cast<SiteClearCtx*>(user_data);
    auto* mgr = WEBKIT_WEBSITE_DATA_MANAGER(source);
    GError* err = nullptr;
    const gboolean ok = webkit_website_data_manager_remove_finish(mgr, res, &err);

    if (GtkWidget* w = GTK_WIDGET(g_weak_ref_get(&ctx->win_ref))) {
        if (ok) {
            std::string msg = "Cleared site cookies & storage for " + ctx->host;
            set_status_message(ctx->win, msg.c_str());
        } else {
            std::string msg = std::string("Failed to clear site data: ") + (err ? err->message : "unknown");
            set_status_message(ctx->win, msg.c_str());
        }
        g_object_unref(w);
    }
    if (err) g_error_free(err);
    free_site_clear_ctx(ctx);
}

void on_site_data_fetched(GObject* source, GAsyncResult* res, gpointer user_data) {
    auto* ctx = static_cast<SiteClearCtx*>(user_data);
    auto* mgr = WEBKIT_WEBSITE_DATA_MANAGER(source);
    GError* err = nullptr;
    GList* list = webkit_website_data_manager_fetch_finish(mgr, res, &err);
    if (err) {
        if (GtkWidget* w = GTK_WIDGET(g_weak_ref_get(&ctx->win_ref))) {
            set_status_message(ctx->win, err->message);
            g_object_unref(w);
        }
        g_error_free(err);
        free_site_clear_ctx(ctx);
        return;
    }

    GList* to_remove = nullptr;
    for (GList* l = list; l != nullptr; l = l->next) {
        auto* wd = static_cast<WebKitWebsiteData*>(l->data);
        const gchar* name = webkit_website_data_get_name(wd);
        if (name && host_matches_site_data(ctx->host, name)) {
            to_remove = g_list_prepend(to_remove, webkit_website_data_ref(wd));
        }
    }
    g_list_free_full(list, reinterpret_cast<GDestroyNotify>(webkit_website_data_unref));

    if (!to_remove) {
        if (GtkWidget* w = GTK_WIDGET(g_weak_ref_get(&ctx->win_ref))) {
            std::string msg = "No stored cookies or data found for " + ctx->host;
            set_status_message(ctx->win, msg.c_str());
            g_object_unref(w);
        }
        free_site_clear_ctx(ctx);
        return;
    }

    webkit_website_data_manager_remove(mgr, kSiteDataTypes, to_remove, nullptr, on_site_data_removed, ctx);
    g_list_free_full(to_remove, reinterpret_cast<GDestroyNotify>(webkit_website_data_unref));
}

void apply_host_overrides_to_matching_tabs(BrowserWindow* win, const std::string& host) {
    if (!win || !win->notebook) return;
    const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook));
    for (gint i = 0; i < n; ++i) {
        GtkWidget* child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(win->notebook), i);
        TabData* td = get_tab_data_from_page(child);
        if (!td || !td->web_view || !td->internal_uri.empty()) continue;
        const gchar* u = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(td->web_view));
        if (extract_host(u) == host) {
            td->last_applied_host.clear();
            apply_site_prefs_to_tab(td);
        }
    }
}

void update_zoom_label(GtkLabel* lbl, double z) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::round(z * 100.0)));
    gtk_label_set_text(lbl, buf);
}

} // namespace

void print_active_tab(BrowserWindow* win) {
    if (!win) return;
    WebKitWebView* v = active_web_view(win);
    if (!v) {
        set_status_message(win, "Print / Save as PDF is available on web pages");
        return;
    }
    WebKitPrintOperation* op = webkit_print_operation_new(v);
    webkit_print_operation_run_dialog(op, GTK_WINDOW(win->window));
    g_object_unref(op);
}

void show_site_info_popover(BrowserWindow* win) {
    if (!win || !win->url_entry) return;
    TabData* td = active_tab(win);
    if (!td) return;

    std::string uri;
    if (!td->internal_uri.empty()) {
        uri = td->internal_uri;
    } else if (td->suspended && td->suspended_url) {
        uri = td->suspended_url;
    } else if (td->web_view) {
        const gchar* wu = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(td->web_view));
        if (wu) uri = wu;
    }

    const std::string host = extract_host(uri.c_str());
    const bool is_https = starts_with_ci(uri, "https://");
    const bool is_http = starts_with_ci(uri, "http://");

    GtkWidget* pop = gtk_popover_new(win->url_entry);
    GdkRectangle icon_rect{};
    gtk_entry_get_icon_area(GTK_ENTRY(win->url_entry), GTK_ENTRY_ICON_PRIMARY, &icon_rect);
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &icon_rect);

    g_signal_connect(pop, "closed", G_CALLBACK(+[](GtkPopover* p, gpointer) {
        g_idle_add(+[](gpointer ptr) -> gboolean {
            gtk_widget_destroy(GTK_WIDGET(ptr));
            return G_SOURCE_REMOVE;
        }, p);
    }), nullptr);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_widget_set_size_request(box, 320, -1);

    const char* title_text = !host.empty() ? host.c_str() : (uri.empty() ? "New Tab" : uri.c_str());
    GtkWidget* host_lbl = gtk_label_new(title_text);
    gtk_label_set_xalign(GTK_LABEL(host_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(host_lbl), PANGO_ELLIPSIZE_END);
    PangoAttrList* attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(attrs, pango_attr_scale_new(1.1));
    gtk_label_set_attributes(GTK_LABEL(host_lbl), attrs);
    pango_attr_list_unref(attrs);

    const char* sec_text = is_https
        ? "HTTPS · Encrypted Connection"
        : (is_http ? "HTTP · Connection Is Not Encrypted" : "Panda Internal / Local Page");
    GtkWidget* sec_lbl = gtk_label_new(sec_text);
    gtk_label_set_xalign(GTK_LABEL(sec_lbl), 0.0f);
    gtk_widget_set_opacity(sec_lbl, 0.78);

    gtk_box_pack_start(GTK_BOX(box), host_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sec_lbl, FALSE, FALSE, 0);

    if (!host.empty()) {
        gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);

        g_object_set_data(G_OBJECT(pop), "site-win", win);
        g_object_set_data_full(G_OBJECT(pop), "site-host", g_strdup(host.c_str()), g_free);

        const SitePref* sp = find_site_pref(win, host);
        const int shield_mode = sp ? sp->shield_mode : -1;
        const int dark_mode = sp ? sp->dark_mode : -1;
        const double zoom = sp ? sp->zoom_level : 1.0;

        GtkWidget* grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 10);

        auto add_mode_combo = [&](int row_idx, const char* label, bool global_on, int cur_mode, GCallback cb) {
            GtkWidget* lbl = gtk_label_new(label);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
            GtkWidget* combo = gtk_combo_box_text_new();
            std::string def_txt = std::string("Global Default (") + (global_on ? "ON)" : "OFF)");
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), def_txt.c_str());
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Always ON (This Site)");
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "OFF (This Site)");
            int active_idx = 0;
            if (cur_mode == 1) active_idx = 1;
            else if (cur_mode == 0) active_idx = 2;
            gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active_idx);
            g_signal_connect(combo, "changed", cb, pop);
            gtk_grid_attach(GTK_GRID(grid), lbl, 0, row_idx, 1, 1);
            gtk_widget_set_hexpand(combo, TRUE);
            gtk_grid_attach(GTK_GRID(grid), combo, 1, row_idx, 1, 1);
        };

        add_mode_combo(0, "Ad & Tracker Shield", win->config.shield_enabled, shield_mode, G_CALLBACK(+[](GtkComboBox* cb, gpointer d) {
            auto* p = GTK_WIDGET(d);
            auto* w = static_cast<BrowserWindow*>(g_object_get_data(G_OBJECT(p), "site-win"));
            const char* h = static_cast<const char*>(g_object_get_data(G_OBJECT(p), "site-host"));
            if (!w || !h) return;
            const int idx = gtk_combo_box_get_active(cb);
            SitePref& sp_ref = upsert_site_pref(w, h);
            sp_ref.shield_mode = (idx == 1) ? 1 : (idx == 2 ? 0 : -1);
            w->store.save_site_prefs(w->site_prefs);
            apply_host_overrides_to_matching_tabs(w, h);
            set_status_message(w, "Updated Shield rule for this site (reload page if needed)");
        }));

        add_mode_combo(1, "Force Dark Mode", win->config.force_dark, dark_mode, G_CALLBACK(+[](GtkComboBox* cb, gpointer d) {
            auto* p = GTK_WIDGET(d);
            auto* w = static_cast<BrowserWindow*>(g_object_get_data(G_OBJECT(p), "site-win"));
            const char* h = static_cast<const char*>(g_object_get_data(G_OBJECT(p), "site-host"));
            if (!w || !h) return;
            const int idx = gtk_combo_box_get_active(cb);
            SitePref& sp_ref = upsert_site_pref(w, h);
            sp_ref.dark_mode = (idx == 1) ? 1 : (idx == 2 ? 0 : -1);
            w->store.save_site_prefs(w->site_prefs);
            apply_host_overrides_to_matching_tabs(w, h);
        }));

        // Per-site zoom row
        GtkWidget* zoom_lbl = gtk_label_new("Site Zoom");
        gtk_label_set_xalign(GTK_LABEL(zoom_lbl), 0.0f);
        GtkWidget* zoom_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget* z_minus = gtk_button_new_with_label("-");
        GtkWidget* z_val = gtk_label_new("100%");
        update_zoom_label(GTK_LABEL(z_val), zoom);
        gtk_widget_set_size_request(z_val, 46, -1);
        GtkWidget* z_plus = gtk_button_new_with_label("+");
        GtkWidget* z_reset = gtk_button_new_with_label("Reset");

        gtk_box_pack_start(GTK_BOX(zoom_box), z_minus, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(zoom_box), z_val, FALSE, FALSE, 4);
        gtk_box_pack_start(GTK_BOX(zoom_box), z_plus, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(zoom_box), z_reset, FALSE, FALSE, 4);

        g_object_set_data(G_OBJECT(pop), "zoom-lbl", z_val);
        g_signal_connect(z_minus, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
            auto* p = GTK_WIDGET(d);
            auto* w = static_cast<BrowserWindow*>(g_object_get_data(G_OBJECT(p), "site-win"));
            auto* lbl = GTK_LABEL(g_object_get_data(G_OBJECT(p), "zoom-lbl"));
            if (WebKitWebView* v = active_web_view(w)) {
                const double nz = std::max(0.3, webkit_web_view_get_zoom_level(v) - 0.1);
                set_active_tab_zoom(w, nz);
                update_zoom_label(lbl, nz);
            }
        }), pop);
        g_signal_connect(z_plus, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
            auto* p = GTK_WIDGET(d);
            auto* w = static_cast<BrowserWindow*>(g_object_get_data(G_OBJECT(p), "site-win"));
            auto* lbl = GTK_LABEL(g_object_get_data(G_OBJECT(p), "zoom-lbl"));
            if (WebKitWebView* v = active_web_view(w)) {
                const double nz = std::min(3.0, webkit_web_view_get_zoom_level(v) + 0.1);
                set_active_tab_zoom(w, nz);
                update_zoom_label(lbl, nz);
            }
        }), pop);
        g_signal_connect(z_reset, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
            auto* p = GTK_WIDGET(d);
            auto* w = static_cast<BrowserWindow*>(g_object_get_data(G_OBJECT(p), "site-win"));
            auto* lbl = GTK_LABEL(g_object_get_data(G_OBJECT(p), "zoom-lbl"));
            set_active_tab_zoom(w, 1.0);
            update_zoom_label(lbl, 1.0);
        }), pop);

        gtk_grid_attach(GTK_GRID(grid), zoom_lbl, 0, 2, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), zoom_box, 1, 2, 1, 1);

        gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);

        GtkWidget* clear_site_btn = gtk_button_new_with_label("Clear Cookies & Site Data");
        gtk_style_context_add_class(gtk_widget_get_style_context(clear_site_btn), "destructive-action");
        g_signal_connect(clear_site_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
            auto* p = GTK_WIDGET(d);
            auto* w = static_cast<BrowserWindow*>(g_object_get_data(G_OBJECT(p), "site-win"));
            const char* h = static_cast<const char*>(g_object_get_data(G_OBJECT(p), "site-host"));
            if (!w || !h || !w->web_context) return;

            WebKitWebsiteDataManager* mgr = webkit_web_context_get_website_data_manager(w->web_context);
            if (!mgr) return;

            auto* ctx = new SiteClearCtx();
            g_weak_ref_init(&ctx->win_ref, w->window);
            ctx->win = w;
            ctx->host = h;
            ctx->mgr = WEBKIT_WEBSITE_DATA_MANAGER(g_object_ref(mgr));

            webkit_website_data_manager_fetch(mgr, kSiteDataTypes, nullptr, on_site_data_fetched, ctx);
            gtk_widget_hide(p);
        }), pop);
        gtk_box_pack_start(GTK_BOX(box), clear_site_btn, FALSE, FALSE, 0);
    }

    gtk_container_add(GTK_CONTAINER(pop), box);
    gtk_widget_show_all(pop);
}

void refresh_downloads_popover(BrowserWindow* win) {
    if (!win || !win->downloads_list) return;
    clear_container_children(win->downloads_list);

    if (win->downloads.empty()) {
        GtkWidget* empty = gtk_label_new("No downloads in this session.");
        gtk_widget_set_margin_top(empty, 18);
        gtk_widget_set_margin_bottom(empty, 18);
        gtk_widget_set_opacity(empty, 0.72);
        gtk_container_add(GTK_CONTAINER(win->downloads_list), empty);
        gtk_widget_show_all(win->downloads_list);
        return;
    }

    for (auto it = win->downloads.rbegin(); it != win->downloads.rend(); ++it) {
        const auto& dl = *it;
        GtkWidget* row = gtk_list_box_row_new();
        GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(hbox, 8);
        gtk_widget_set_margin_end(hbox, 8);
        gtk_widget_set_margin_top(hbox, 6);
        gtk_widget_set_margin_bottom(hbox, 6);

        GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget* name_lbl = gtk_label_new(dl.filename.c_str());
        gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name_lbl), 28);

        std::string status_str;
        if (dl.failed) {
            status_str = "Failed: " + (dl.error_msg.empty() ? "unknown error" : dl.error_msg);
        } else if (dl.finished) {
            status_str = "Completed";
        } else {
            status_str = "Downloading · " + std::to_string(dl.progress_pct) + "%";
        }
        GtkWidget* sub_lbl = gtk_label_new(status_str.c_str());
        gtk_label_set_xalign(GTK_LABEL(sub_lbl), 0.0f);
        gtk_widget_set_opacity(sub_lbl, 0.72);

        gtk_box_pack_start(GTK_BOX(vbox), name_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), sub_lbl, FALSE, FALSE, 0);

        GtkWidget* open_btn = gtk_button_new_with_label("Open");
        gtk_widget_set_sensitive(open_btn, dl.finished && !dl.failed && !dl.dest_path.empty());
        g_object_set_data_full(G_OBJECT(open_btn), "dl-path", g_strdup(dl.dest_path.c_str()), g_free);
        g_signal_connect(open_btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer d) {
            auto* w = static_cast<BrowserWindow*>(d);
            const char* path = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "dl-path"));
            if (path && path[0] != '\0') open_local_path_uri(w, path);
        }), win);

        GtkWidget* dir_btn = gtk_button_new_from_icon_name("folder-open-symbolic", GTK_ICON_SIZE_MENU);
        gtk_widget_set_tooltip_text(dir_btn, "Show in Folder");
        g_object_set_data_full(G_OBJECT(dir_btn), "dl-path", g_strdup(dl.dest_path.c_str()), g_free);
        g_signal_connect(dir_btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer d) {
            auto* w = static_cast<BrowserWindow*>(d);
            const char* path = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "dl-path"));
            if (path && path[0] != '\0') {
                gchar* dir = g_path_get_dirname(path);
                if (dir) {
                    open_local_path_uri(w, dir);
                    g_free(dir);
                }
            }
        }), win);

        gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), dir_btn, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), open_btn, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), hbox);
        gtk_container_add(GTK_CONTAINER(win->downloads_list), row);
    }
    gtk_widget_show_all(win->downloads_list);
}

void show_downloads_popover(BrowserWindow* win) {
    if (!win || !win->downloads_btn) return;
    if (win->downloads_popover) {
        gtk_widget_destroy(win->downloads_popover);
        return;
    }

    GtkWidget* pop = gtk_popover_new(win->downloads_btn);
    win->downloads_popover = pop;

    g_signal_connect(pop, "closed", G_CALLBACK(+[](GtkPopover* p, gpointer) {
        g_idle_add(+[](gpointer ptr) -> gboolean {
            gtk_widget_destroy(GTK_WIDGET(ptr));
            return G_SOURCE_REMOVE;
        }, p);
    }), nullptr);

    g_signal_connect(pop, "destroy", G_CALLBACK(+[](GtkWidget* p, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        if (w->downloads_popover == p) {
            w->downloads_popover = nullptr;
            w->downloads_list = nullptr;
        }
    }), win);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(root), 10);
    gtk_widget_set_size_request(root, 380, -1);

    GtkWidget* hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* title = gtk_label_new("Downloads");
    PangoAttrList* attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(title), attrs);
    pango_attr_list_unref(attrs);

    GtkWidget* folder_btn = gtk_button_new_with_label("Open Folder");
    g_signal_connect(folder_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        const char* dl_dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
        if (!dl_dir || dl_dir[0] == '\0') dl_dir = g_get_home_dir();
        open_local_path_uri(w, dl_dir);
    }), win);

    GtkWidget* clear_btn = gtk_button_new_with_label("Clear");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        auto* w = static_cast<BrowserWindow*>(d);
        w->downloads.clear();
        refresh_downloads_popover(w);
    }), win);

    gtk_box_pack_start(GTK_BOX(hdr), title, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hdr), folder_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hdr), clear_btn, FALSE, FALSE, 0);

    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, 360, 230);
    win->downloads_list = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scroll), win->downloads_list);

    gtk_box_pack_start(GTK_BOX(root), hdr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(pop), root);

    refresh_downloads_popover(win);
    gtk_widget_show_all(pop);
}

void show_clear_data_dialog(BrowserWindow* win) {
    if (!win) return;
    GtkWidget* dlg = gtk_dialog_new_with_buttons(
        "Clear Browsing Data",
        GTK_WINDOW(win->window),
        GTK_DIALOG_MODAL,
        "Clear Data",
        GTK_RESPONSE_ACCEPT,
        "Cancel",
        GTK_RESPONSE_CANCEL,
        nullptr
    );
    gtk_window_set_default_size(GTK_WINDOW(dlg), 410, 230);

    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 14);
    gtk_container_add(GTK_CONTAINER(area), vbox);

    GtkWidget* range_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* range_lbl = gtk_label_new("Time Range");
    GtkWidget* range_cb = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(range_cb), "Last Hour");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(range_cb), "Last 24 Hours");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(range_cb), "All Time");
    gtk_combo_box_set_active(GTK_COMBO_BOX(range_cb), 1);
    gtk_box_pack_start(GTK_BOX(range_row), range_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(range_row), range_cb, TRUE, TRUE, 0);

    GtkWidget* hist_chk = gtk_check_button_new_with_label("Browsing history");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hist_chk), TRUE);
    GtkWidget* cache_chk = gtk_check_button_new_with_label("Cached files & images");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cache_chk), TRUE);
    GtkWidget* cookies_chk = gtk_check_button_new_with_label("Cookies & site storage (clears all site logins; keeps saved vault passwords)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cookies_chk), FALSE);

    gtk_box_pack_start(GTK_BOX(vbox), range_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hist_chk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), cache_chk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), cookies_chk, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        const int range_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(range_cb));
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        int64_t cutoff = 0;
        GTimeSpan span = 0;
        if (range_idx == 0) {
            cutoff = now - 3600;
            span = static_cast<GTimeSpan>(3600) * G_USEC_PER_SEC;
        } else if (range_idx == 1) {
            cutoff = now - 86400;
            span = static_cast<GTimeSpan>(86400) * G_USEC_PER_SEC;
        }

        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hist_chk)) == TRUE) {
            win->store.clear_history_since(win->history, cutoff);
            refresh_url_completions(win);
            if (win->notebook) {
                const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook));
                for (gint i = 0; i < n; ++i) {
                    TabData* td = get_tab_data_from_page(gtk_notebook_get_nth_page(GTK_NOTEBOOK(win->notebook), i));
                    if (td && td->internal_uri == kPandaHistoryUri) refresh_history_view(td);
                }
            }
        }

        if (win->web_context) {
            if (WebKitWebsiteDataManager* mgr = webkit_web_context_get_website_data_manager(win->web_context)) {
                if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cache_chk)) == TRUE) {
                    const WebKitWebsiteDataTypes cache_types = static_cast<WebKitWebsiteDataTypes>(
                        WEBKIT_WEBSITE_DATA_DISK_CACHE | WEBKIT_WEBSITE_DATA_MEMORY_CACHE
                    );
                    webkit_website_data_manager_clear(mgr, cache_types, span, nullptr, nullptr, nullptr);
                }
                if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cookies_chk)) == TRUE) {
                    const WebKitWebsiteDataTypes storage_types = static_cast<WebKitWebsiteDataTypes>(
                        WEBKIT_WEBSITE_DATA_LOCAL_STORAGE |
                        WEBKIT_WEBSITE_DATA_SESSION_STORAGE |
                        WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES |
                        WEBKIT_WEBSITE_DATA_OFFLINE_APPLICATION_CACHE
                    );
                    webkit_website_data_manager_clear(mgr, storage_types, span, nullptr, nullptr, nullptr);
                    // WebKitGTK requires GTimeSpan == 0 to wipe stored cookies reliably
                    webkit_website_data_manager_clear(mgr, WEBKIT_WEBSITE_DATA_COOKIES, 0, nullptr, nullptr, nullptr);
                }
            }
        }
        set_status_message(win, "Selected browsing data cleared");
    }
    gtk_widget_destroy(dlg);
}

void show_preferences_dialog(BrowserWindow* win) {
    GtkWidget* dlg = gtk_dialog_new_with_buttons(
        "Preferences — Panda Browser",
        GTK_WINDOW(win->window),
        GTK_DIALOG_MODAL,
        "Save",
        GTK_RESPONSE_ACCEPT,
        "Cancel",
        GTK_RESPONSE_CANCEL,
        nullptr
    );
    gtk_window_set_default_size(GTK_WINDOW(dlg), 470, 430);

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

    GtkWidget* clear_data_btn = gtk_button_new_with_label("Clear Browsing Data...");
    g_signal_connect(clear_data_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        show_clear_data_dialog(static_cast<BrowserWindow*>(d));
    }), win);
    gtk_grid_attach(GTK_GRID(grid), clear_data_btn, 0, row++, 2, 1);

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
        GTK_RESPONSE_CLOSE,
        nullptr
    );
    gtk_window_set_default_size(GTK_WINDOW(dlg), 540, 380);

    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(area), vbox);

    GtkWidget* search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Type command, tab title, or bookmark...");
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    GtkWidget* list = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scroll), list);
    gtk_box_pack_start(GTK_BOX(vbox), search, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    struct PalItem {
        std::string label;
        std::string payload;
    };
    std::vector<PalItem> entries = {
        {"Site Info & Permissions (Padlock)", "cmd:site_info"},
        {"Print / Save as PDF (Ctrl+P)", "cmd:print"},
        {"Open Downloads", "cmd:downloads"},
        {"Clear Browsing Data...", "cmd:clear_data"},
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
        if (sv == "cmd:site_info") show_site_info_popover(w);
        else if (sv == "cmd:print") print_active_tab(w);
        else if (sv == "cmd:downloads") show_downloads_popover(w);
        else if (sv == "cmd:clear_data") show_clear_data_dialog(w);
        else if (sv == "cmd:sleep_all") sleep_all_background_tabs(w);
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

} // namespace panda
