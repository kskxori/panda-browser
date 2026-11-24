#pragma once

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <string>

namespace panda {

class ContentBlocker {
public:
    ContentBlocker() = default;
    ~ContentBlocker();

    ContentBlocker(const ContentBlocker&) = delete;
    ContentBlocker& operator=(const ContentBlocker&) = delete;

    void init(
        const std::string& cache_dir,
        const std::string& custom_user_css,
        const std::string& custom_user_js,
        GtkWidget* notebook_widget
    );

    void shutdown();

    void apply_to_view(WebKitWebView* view, bool shield_enabled, bool force_dark) const;
    void apply_to_all_tabs(bool shield_enabled, bool force_dark) const;

    static void trigger_reader_mode(WebKitWebView* view);

private:
    static void on_filter_saved(GObject* source, GAsyncResult* res, gpointer data);

    GtkWidget* notebook_ = nullptr;
    WebKitUserContentFilterStore* filter_store_ = nullptr;
    WebKitUserContentFilter* shield_filter_ = nullptr;
    GCancellable* cancellable_ = nullptr;
    WebKitUserStyleSheet* dark_sheet_ = nullptr;
    WebKitUserStyleSheet* user_css_sheet_ = nullptr;
    WebKitUserScript* user_js_script_ = nullptr;
    bool desired_shield_ = true;
    bool desired_dark_ = false;
};

} // namespace panda
