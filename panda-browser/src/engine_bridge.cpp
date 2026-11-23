// ============================================================================
// FILE: panda-browser/src/engine_bridge.cpp
// Standalone C++20 zero-allocation DOM scanner & JS evaluation bridge
// ============================================================================
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

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
void cpp_engine_shutdown(void);

} // extern "C"

namespace {

// Converts ASCII uppercase letters to lowercase without locale overhead
constexpr char to_lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

// Case-insensitive ASCII comparison for HTML tag names
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

// Scans forward from `start` for `needle`; eligible for compiler auto-vectorization
size_t find_char(std::string_view sv, size_t start, char needle) noexcept {
    const size_t n = sv.size();
    const char* data = sv.data();
    for (size_t i = start; i < n; ++i) {
        if (data[i] == needle) {
            return i;
        }
    }
    return std::string_view::npos;
}

} // namespace

extern "C" {

// Initializes C++ subsystem hooks according to Gentoo-style build flags
int32_t cpp_engine_init(bool enable_simd, uint32_t max_pages) {
    (void)enable_simd;
    (void)max_pages;
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
    if (!html_ptr || html_len == 0) {
        return stats;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const std::string_view doc(reinterpret_cast<const char*>(html_ptr), html_len);

    size_t pos = 0;
    size_t text_pos = 0;
    size_t title_pos = 0;
    bool in_title = false;
    bool in_ignored_block = false; // True inside <script> or <style>

    while (pos < doc.size()) {
        const size_t lt = find_char(doc, pos, '<');
        const std::string_view chunk = (lt == std::string_view::npos)
            ? doc.substr(pos)
            : doc.substr(pos, lt - pos);

        // Extract visible text outside <script> and <style> blocks
        if (!in_ignored_block && !chunk.empty()) {
            for (char ch : chunk) {
                const bool is_ws = (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t');
                if (in_title && title_buf && title_pos + 1 < title_cap) {
                    if (!(is_ws && title_pos > 0 && title_buf[title_pos - 1] == ' ')) {
                        title_buf[title_pos++] = static_cast<uint8_t>(is_ws ? ' ' : ch);
                    }
                }
                if (text_buf && text_pos + 1 < text_cap) {
                    if (!(is_ws && text_pos > 0 && text_buf[text_pos - 1] == ' ')) {
                        text_buf[text_pos++] = static_cast<uint8_t>(is_ws ? ' ' : ch);
                    }
                }
            }
        }

        if (lt == std::string_view::npos) {
            break;
        }

        const size_t gt = find_char(doc, lt + 1, '>');
        if (gt == std::string_view::npos) {
            break;
        }

        // Parse tag name after '<' and optional '/'
        std::string_view tag_inner = doc.substr(lt + 1, gt - (lt + 1));
        while (!tag_inner.empty() && tag_inner.front() == ' ') {
            tag_inner.remove_prefix(1);
        }

        bool closing = false;
        if (!tag_inner.empty() && tag_inner.front() == '/') {
            closing = true;
            tag_inner.remove_prefix(1);
        }

        size_t name_end = 0;
        while (name_end < tag_inner.size() &&
               tag_inner[name_end] != ' ' &&
               tag_inner[name_end] != '\t' &&
               tag_inner[name_end] != '\n' &&
               tag_inner[name_end] != '/') {
            ++name_end;
        }
        const std::string_view tag_name = tag_inner.substr(0, name_end);

        if (iequals(tag_name, "title")) {
            in_title = !closing;
        } else if (iequals(tag_name, "script")) {
            if (!closing) {
                ++stats.script_count;
            }
            in_ignored_block = !closing;
        } else if (iequals(tag_name, "style")) {
            in_ignored_block = !closing;
        } else if (iequals(tag_name, "a") && !closing) {
            ++stats.link_count;
        }

        pos = gt + 1;
    }

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
    if (!script_ptr || !out_buf || out_cap == 0) {
        return 0;
    }
    const std::string_view src(reinterpret_cast<const char*>(script_ptr), script_len);

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
    std::memcpy(out_buf + copy_tag, src.data(), copy_src);
    out_buf[copy_tag + copy_src] = '\0';
    return copy_tag + copy_src;
}

// Cleans up C++ engine state during shutdown
void cpp_engine_shutdown(void) {
    // Release JS runtime / isolate resources if initialized
}

} // extern "C"