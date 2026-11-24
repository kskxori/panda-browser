#include "content_blocker.hpp"

#include <cstring>

namespace panda {

namespace {

// Compiled natively by WebKit in C++ with near-zero RAM/CPU overhead
constexpr const char* kShieldRulesJson = R"JSON([
  {
    "trigger": {
      "url-filter": "^https?://([^/]+\\.)?(doubleclick\\.net|googlesyndication\\.com|googleadservices\\.com|adservice\\.google\\.com|google-analytics\\.com|googletagmanager\\.com|scorecardresearch\\.com|taboola\\.com|outbrain\\.com|criteo\\.com|pubmatic\\.com|adnxs\\.com|quantserve\\.com|facebook\\.net|ads-twitter\\.com|hotjar\\.com|fullstory\\.com|clarity\\.ms|mixpanel\\.com|amplitude\\.com|segment\\.io|sentry\\.io|coinhive\\.com|cryptoloot\\.pro)/",
      "load-type": ["third-party"]
    },
    "action": { "type": "block" }
  }
])JSON";

constexpr const char* kForceDarkCss = R"CSS(
html {
  filter: invert(0.90) hue-rotate(180deg) !important;
  background-color: #111317 !important;
}
img, video, picture, canvas, svg, [style*="background-image"] {
  filter: invert(1) hue-rotate(180deg) !important;
}
)CSS";

constexpr const char* kReaderModeJs = R"JS(
(() => {
  if (document.getElementById('__panda_reader_root')) {
    location.reload();
    return;
  }
  const cand = document.querySelector('article, main, [role="main"], .post-content, .article-body') || document.body;
  const title = document.title || location.hostname;
  const clone = cand ? cand.cloneNode(true) : document.createElement('div');
  clone.querySelectorAll('script,style,noscript,iframe,nav,aside,footer,header,form').forEach(e => e.remove());
  document.head.innerHTML = `<meta charset="utf-8"><title>${title} — Reader</title><style>
    body{background:#12151b;color:#e5e7eb;font-family:Georgia,serif;line-height:1.72;margin:0;padding:36px 20px;}
    #__panda_reader_root{max-width:740px;margin:0 auto;}
    h1{font-family:system-ui,sans-serif;font-size:28px;line-height:1.25;margin-bottom:20px;color:#f9fafb;}
    img{max-width:100%;height:auto;border-radius:6px;}
    a{color:#60a5fa;}
    pre,code{background:#1d222c;padding:2px 6px;border-radius:4px;overflow-x:auto;}
    .panda-exit{font-family:system-ui,sans-serif;font-size:13px;background:#2563eb;color:#fff;border:none;border-radius:6px;padding:6px 12px;cursor:pointer;margin-bottom:20px;}
  </style>`;
  document.body.innerHTML = `<div id="__panda_reader_root"><button class="panda-exit" onclick="location.reload()">Exit Reader Mode</button><h1>${title}</h1></div>`;
  document.getElementById('__panda_reader_root').appendChild(clone);
})();
)JS";

} // namespace

ContentBlocker::~ContentBlocker() {
    shutdown();
}

void ContentBlocker::init(
    const std::string& cache_dir,
    const std::string& custom_user_css,
    const std::string& custom_user_js,
    GtkWidget* notebook_widget
) {
    notebook_ = notebook_widget;

    dark_sheet_ = webkit_user_style_sheet_new(
        kForceDarkCss,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_STYLE_LEVEL_USER,
        nullptr,
        nullptr
    );

    if (!custom_user_css.empty()) {
        user_css_sheet_ = webkit_user_style_sheet_new(
            custom_user_css.c_str(),
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_STYLE_LEVEL_USER,
            nullptr,
            nullptr
        );
    }

    if (!custom_user_js.empty()) {
        user_js_script_ = webkit_user_script_new(
            custom_user_js.c_str(),
            WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
            nullptr,
            nullptr
        );
    }

    gchar* filter_dir = g_build_filename(cache_dir.c_str(), "content-filters", nullptr);
    g_mkdir_with_parents(filter_dir, 0700);
    filter_store_ = webkit_user_content_filter_store_new(filter_dir);
    g_free(filter_dir);

    cancellable_ = g_cancellable_new();
    GBytes* rules_bytes = g_bytes_new_static(kShieldRulesJson, std::strlen(kShieldRulesJson));
    webkit_user_content_filter_store_save(
        filter_store_,
        "panda-v2-shield",
        rules_bytes,
        cancellable_,
        on_filter_saved,
        this
    );
    g_bytes_unref(rules_bytes);
}

void ContentBlocker::shutdown() {
    notebook_ = nullptr;
    if (cancellable_) {
        g_cancellable_cancel(cancellable_);
        g_object_unref(cancellable_);
        cancellable_ = nullptr;
    }
    if (shield_filter_) {
        webkit_user_content_filter_unref(shield_filter_);
        shield_filter_ = nullptr;
    }
    if (filter_store_) {
        g_object_unref(filter_store_);
        filter_store_ = nullptr;
    }
    if (dark_sheet_) {
        webkit_user_style_sheet_unref(dark_sheet_);
        dark_sheet_ = nullptr;
    }
    if (user_css_sheet_) {
        webkit_user_style_sheet_unref(user_css_sheet_);
        user_css_sheet_ = nullptr;
    }
    if (user_js_script_) {
        webkit_user_script_unref(user_js_script_);
        user_js_script_ = nullptr;
    }
}

void ContentBlocker::on_filter_saved(GObject* source, GAsyncResult* res, gpointer data) {
    auto* self = static_cast<ContentBlocker*>(data);
    GError* err = nullptr;
    WebKitUserContentFilter* filter = webkit_user_content_filter_store_save_finish(
        WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &err
    );
    if (err) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_warning("panda-browser shield compile error: %s", err->message);
        }
        g_error_free(err);
        return;
    }
    if (!self || !self->cancellable_ || g_cancellable_is_cancelled(self->cancellable_) || !self->notebook_) {
        if (filter) {
            webkit_user_content_filter_unref(filter);
        }
        return;
    }
    if (self->shield_filter_) {
        webkit_user_content_filter_unref(self->shield_filter_);
    }
    self->shield_filter_ = filter;
    self->apply_to_all_tabs(self->desired_shield_, self->desired_dark_);
}

void ContentBlocker::apply_to_view(WebKitWebView* view, bool shield_enabled, bool force_dark) const {
    if (!view) {
        return;
    }
    WebKitUserContentManager* ucm = webkit_web_view_get_user_content_manager(view);
    if (!ucm) {
        return;
    }

    if (shield_filter_) {
        if (shield_enabled) {
            webkit_user_content_manager_add_filter(ucm, shield_filter_);
        } else {
            webkit_user_content_manager_remove_filter(ucm, shield_filter_);
        }
    }

    if (dark_sheet_) {
        webkit_user_content_manager_remove_style_sheet(ucm, dark_sheet_);
        if (force_dark) {
            webkit_user_content_manager_add_style_sheet(ucm, dark_sheet_);
        }
    }

    if (user_css_sheet_) {
        webkit_user_content_manager_remove_style_sheet(ucm, user_css_sheet_);
        webkit_user_content_manager_add_style_sheet(ucm, user_css_sheet_);
    }

    if (user_js_script_) {
        webkit_user_content_manager_remove_all_scripts(ucm);
        webkit_user_content_manager_add_script(ucm, user_js_script_);
    }
}

void ContentBlocker::apply_to_all_tabs(bool shield_enabled, bool force_dark) const {
    const_cast<ContentBlocker*>(this)->desired_shield_ = shield_enabled;
    const_cast<ContentBlocker*>(this)->desired_dark_ = force_dark;

    if (!notebook_) {
        return;
    }
    const gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook_));
    for (gint i = 0; i < n; ++i) {
        GtkWidget* child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook_), i);
        if (child) {
            apply_to_view(WEBKIT_WEB_VIEW(child), shield_enabled, force_dark);
        }
    }
}

void ContentBlocker::trigger_reader_mode(WebKitWebView* view) {
    if (!view) {
        return;
    }
#if WEBKIT_CHECK_VERSION(2, 40, 0)
    webkit_web_view_evaluate_javascript(
        view, kReaderModeJs, -1, nullptr, nullptr, nullptr, nullptr, nullptr
    );
#else
    webkit_web_view_run_javascript(view, kReaderModeJs, nullptr, nullptr, nullptr);
#endif
}

} // namespace panda
