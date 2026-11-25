#include "browser_state.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>

namespace panda {

void refresh_home_view(TabData* td) {
    if (!td || !td->win || !td->home_tiles_grid) return;
    BrowserWindow* win = td->win;

    clear_container_children(td->home_tiles_grid);

    const size_t cap = std::min<size_t>(win->bookmarks.size(), 8);
    for (size_t i = 0; i < cap; ++i) {
        GtkWidget* btn = gtk_button_new();
        gtk_widget_set_size_request(btn, 185, 64);
        gtk_widget_set_tooltip_text(btn, win->bookmarks[i].url.c_str());

        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget* t_lbl = gtk_label_new(win->bookmarks[i].title.c_str());
        gtk_label_set_ellipsize(GTK_LABEL(t_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(t_lbl), 20);

        PangoAttrList* attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes(GTK_LABEL(t_lbl), attrs);
        pango_attr_list_unref(attrs);

        GtkWidget* u_lbl = gtk_label_new(win->bookmarks[i].url.c_str());
        gtk_label_set_ellipsize(GTK_LABEL(u_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(u_lbl), 24);
        gtk_widget_set_opacity(u_lbl, 0.72);

        gtk_box_pack_start(GTK_BOX(box), t_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), u_lbl, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(btn), box);

        g_object_set_data_full(G_OBJECT(btn), "tile-url", g_strdup(win->bookmarks[i].url.c_str()), g_free);
        g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer d) {
            const char* u = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "tile-url"));
            if (u) load_user_input(static_cast<TabData*>(d), u);
        }), td);

        const int row = static_cast<int>(i / 4);
        const int col = static_cast<int>(i % 4);
        gtk_grid_attach(GTK_GRID(td->home_tiles_grid), btn, col, row, 1, 1);
    }
    gtk_widget_show_all(td->home_tiles_grid);

    if (td->home_stats_lbl) {
        const double total_mb = static_cast<double>(win->cached_mem.total_rss_kb) / 1024.0;
        const gint tab_count = win->notebook ? gtk_notebook_get_n_pages(GTK_NOTEBOOK(win->notebook)) : 1;
        char stats[160];
        std::snprintf(
            stats,
            sizeof(stats),
            "RAM: %.1f MB · Open Tabs: %d · Saved Logins: %zu · Shield: %s · Ctrl+Shift+P Palette",
            total_mb,
            tab_count,
            win->passwords.size(),
            win->config.shield_enabled ? "ON" : "OFF"
        );
        gtk_label_set_text(GTK_LABEL(td->home_stats_lbl), stats);
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
    if (win->is_private) {
        GtkWidget* sub_lbl = gtk_label_new("Private Session · Zero Disk History or Passwords");
        gtk_widget_set_opacity(sub_lbl, 0.72);
        gtk_box_pack_start(GTK_BOX(center_box), sub_lbl, FALSE, FALSE, 0);
    }
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

    int shown = 0;
    const auto& items = td->win->history;
    for (size_t rev = items.size(); rev > 0 && shown < 250; --rev) {
        const size_t idx = rev - 1;
        const auto& h = items[idx];
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

        std::string meta = format_timestamp(h.timestamp) + "  ·  " + h.url;
        GtkWidget* u_lbl = gtk_label_new(meta.c_str());
        gtk_label_set_xalign(GTK_LABEL(u_lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(u_lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_opacity(u_lbl, 0.70);

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
    gtk_widget_set_size_request(td->history_search, 240, -1);

    GtkWidget* clear_data_btn = gtk_button_new_with_label("Clear Data...");
    GtkWidget* clear_btn = gtk_button_new_with_label("Clear All History");
    gtk_style_context_add_class(gtk_widget_get_style_context(clear_btn), "destructive-action");

    gtk_box_pack_start(GTK_BOX(header), title, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(header), clear_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(header), clear_data_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(header), td->history_search, FALSE, FALSE, 0);

    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    td->history_list = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scroll), td->history_list);

    g_signal_connect(td->history_search, "search-changed", G_CALLBACK(+[](GtkSearchEntry*, gpointer d) {
        refresh_history_view(static_cast<TabData*>(d));
    }), td);

    g_signal_connect(clear_data_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        show_clear_data_dialog(static_cast<TabData*>(d)->win);
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

} // namespace panda
