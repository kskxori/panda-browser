// ============================================================================
// FILE: panda-browser/src/engine_bridge.cpp
// Standalone C++20 DOM scanner, JS bridge, and GUI dispatcher (v2.00.00)
// ============================================================================
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#if defined(USE_GUI_WEBKIT)
#include "browser_ui.hpp"
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

} // namespace

extern "C" {

int32_t cpp_engine_init(bool enable_simd, uint32_t max_pages) {
    if (max_pages == 0) {
        return -1;
    }
    g_simd_enabled = enable_simd;
    g_max_pages = max_pages;
    return 0;
}

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
            // Skip DOCTYPE
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
    return panda::launch_browser_gui(initial_url, g_max_pages);
#else
    (void)initial_url;
    return -1;
#endif
}

void cpp_engine_shutdown(void) {
    g_simd_enabled = false;
}

} // extern "C"
