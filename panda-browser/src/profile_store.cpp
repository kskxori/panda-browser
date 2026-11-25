#include "profile_store.hpp"

#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <string_view>
#include <unistd.h>

namespace panda {

namespace {

constexpr size_t kMaxHistory = 1000;

std::string sanitize_tsv(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char ch : sv) {
        if (ch == '\t' || ch == '\n' || ch == '\r') {
            out.push_back(' ');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::vector<std::string_view> split_tabs(std::string_view line) {
    std::vector<std::string_view> cols;
    size_t start = 0;
    while (start <= line.size()) {
        size_t pos = line.find('\t', start);
        if (pos == std::string_view::npos) {
            cols.push_back(line.substr(start));
            break;
        }
        cols.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return cols;
}

uint64_t read_statm_rss_kb(const char* path, uint64_t page_kb) {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    char buf[96];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';

    unsigned long size_pages = 0;
    unsigned long rss_pages = 0;
    if (std::sscanf(buf, "%lu %lu", &size_pages, &rss_pages) != 2) {
        return 0;
    }
    return static_cast<uint64_t>(rss_pages) * page_kb;
}

bool read_proc_ppid(const char* stat_path, pid_t& out_ppid) {
    const int fd = ::open(stat_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    char buf[384];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';

    const char* rparen = std::strrchr(buf, ')');
    if (!rparen) {
        return false;
    }
    char state = '\0';
    long ppid_val = 0;
    if (std::sscanf(rparen + 1, " %c %ld", &state, &ppid_val) != 2) {
        return false;
    }
    out_ppid = static_cast<pid_t>(ppid_val);
    return true;
}

bool is_numeric_name(const char* name) {
    if (!name || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0; name[i] != '\0'; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
            return false;
        }
    }
    return true;
}

void fill_random_bytes(uint8_t* dst, size_t len) {
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t off = 0;
        while (off < len) {
            const ssize_t r = ::read(fd, dst + off, len - off);
            if (r <= 0) break;
            off += static_cast<size_t>(r);
        }
        ::close(fd);
        if (off == len) {
            return;
        }
    }
    for (size_t i = 0; i < len; ++i) {
        dst[i] = static_cast<uint8_t>(g_random_int() & 0xff);
    }
}

void compute_hmac_sha256(
    const uint8_t* key,
    size_t key_len,
    const uint8_t* a,
    size_t a_len,
    const uint8_t* b,
    size_t b_len,
    uint8_t out[32]
) {
    GHmac* hmac = g_hmac_new(G_CHECKSUM_SHA256, key, static_cast<gssize>(key_len));
    if (a && a_len > 0) {
        g_hmac_update(hmac, a, static_cast<gssize>(a_len));
    }
    if (b && b_len > 0) {
        g_hmac_update(hmac, b, static_cast<gssize>(b_len));
    }
    gsize dlen = 32;
    g_hmac_get_digest(hmac, out, &dlen);
    g_hmac_unref(hmac);
}

void xor_keystream(const uint8_t key[32], const uint8_t nonce[16], uint8_t* data, size_t len) {
    uint32_t counter = 0;
    size_t offset = 0;
    while (offset < len) {
        uint8_t ctr_bytes[4] = {
            static_cast<uint8_t>(counter & 0xff),
            static_cast<uint8_t>((counter >> 8) & 0xff),
            static_cast<uint8_t>((counter >> 16) & 0xff),
            static_cast<uint8_t>((counter >> 24) & 0xff),
        };
        uint8_t block[32];
        compute_hmac_sha256(key, 32, nonce, 16, ctr_bytes, 4, block);
        const size_t chunk = std::min<size_t>(32, len - offset);
        for (size_t i = 0; i < chunk; ++i) {
            data[offset + i] ^= block[i];
        }
        offset += chunk;
        ++counter;
    }
}

} // namespace

ProfileStore::ProfileStore() {
    gchar* cfg = g_build_filename(g_get_user_config_dir(), "panda-browser", nullptr);
    gchar* dat = g_build_filename(g_get_user_data_dir(), "panda-browser", nullptr);
    gchar* cch = g_build_filename(g_get_user_cache_dir(), "panda-browser", nullptr);

    g_mkdir_with_parents(cfg, 0700);
    g_mkdir_with_parents(dat, 0700);
    g_mkdir_with_parents(cch, 0700);

    config_dir_ = cfg;
    data_dir_ = dat;
    cache_dir_ = cch;

    g_free(cfg);
    g_free(dat);
    g_free(cch);
}

BrowserConfig ProfileStore::load_config() const {
    BrowserConfig cfg;
    std::string path = config_dir_ + "/config.ini";

    GKeyFile* kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
        g_key_file_free(kf);
        return cfg;
    }

    const char* grp = "General";
    if (g_key_file_has_key(kf, grp, "search_engine", nullptr)) {
        const int v = g_key_file_get_integer(kf, grp, "search_engine", nullptr);
        if (v >= 0 && v <= 4) cfg.search_engine = static_cast<SearchEngine>(v);
    }
    if (g_key_file_has_key(kf, grp, "cache_profile", nullptr)) {
        const int v = g_key_file_get_integer(kf, grp, "cache_profile", nullptr);
        if (v >= 0 && v <= 2) cfg.cache_profile = static_cast<CacheProfile>(v);
    }
    if (g_key_file_has_key(kf, grp, "gpu_policy", nullptr)) {
        const int v = g_key_file_get_integer(kf, grp, "gpu_policy", nullptr);
        if (v >= 0 && v <= 2) cfg.gpu_policy = static_cast<GpuPolicy>(v);
    }
    if (g_key_file_has_key(kf, grp, "ua_preset", nullptr)) {
        const int v = g_key_file_get_integer(kf, grp, "ua_preset", nullptr);
        if (v >= 0 && v <= 2) cfg.ua_preset = static_cast<UserAgentPreset>(v);
    }
    if (g_key_file_has_key(kf, grp, "shield_enabled", nullptr)) {
        cfg.shield_enabled = g_key_file_get_boolean(kf, grp, "shield_enabled", nullptr) == TRUE;
    }
    if (g_key_file_has_key(kf, grp, "force_dark", nullptr)) {
        cfg.force_dark = g_key_file_get_boolean(kf, grp, "force_dark", nullptr) == TRUE;
    }
    if (g_key_file_has_key(kf, grp, "show_bookmarks_bar", nullptr)) {
        cfg.show_bookmarks_bar = g_key_file_get_boolean(kf, grp, "show_bookmarks_bar", nullptr) == TRUE;
    }
    if (g_key_file_has_key(kf, grp, "restore_session", nullptr)) {
        cfg.restore_session = g_key_file_get_boolean(kf, grp, "restore_session", nullptr) == TRUE;
    }
    if (g_key_file_has_key(kf, grp, "remember_history", nullptr)) {
        cfg.remember_history = g_key_file_get_boolean(kf, grp, "remember_history", nullptr) == TRUE;
    }
    if (g_key_file_has_key(kf, grp, "remember_passwords", nullptr)) {
        cfg.remember_passwords = g_key_file_get_boolean(kf, grp, "remember_passwords", nullptr) == TRUE;
    }
    if (g_key_file_has_key(kf, grp, "hibernate_minutes", nullptr)) {
        cfg.hibernate_minutes = std::max(0, g_key_file_get_integer(kf, grp, "hibernate_minutes", nullptr));
    }
    if (g_key_file_has_key(kf, grp, "home_url", nullptr)) {
        gchar* val = g_key_file_get_string(kf, grp, "home_url", nullptr);
        if (val && val[0] != '\0') {
            cfg.home_url = val;
        }
        g_free(val);
    }

    g_key_file_free(kf);
    return cfg;
}

void ProfileStore::save_config(const BrowserConfig& cfg) const {
    GKeyFile* kf = g_key_file_new();
    const char* grp = "General";

    g_key_file_set_integer(kf, grp, "search_engine", static_cast<int>(cfg.search_engine));
    g_key_file_set_integer(kf, grp, "cache_profile", static_cast<int>(cfg.cache_profile));
    g_key_file_set_integer(kf, grp, "gpu_policy", static_cast<int>(cfg.gpu_policy));
    g_key_file_set_integer(kf, grp, "ua_preset", static_cast<int>(cfg.ua_preset));
    g_key_file_set_boolean(kf, grp, "shield_enabled", cfg.shield_enabled ? TRUE : FALSE);
    g_key_file_set_boolean(kf, grp, "force_dark", cfg.force_dark ? TRUE : FALSE);
    g_key_file_set_boolean(kf, grp, "show_bookmarks_bar", cfg.show_bookmarks_bar ? TRUE : FALSE);
    g_key_file_set_boolean(kf, grp, "restore_session", cfg.restore_session ? TRUE : FALSE);
    g_key_file_set_boolean(kf, grp, "remember_history", cfg.remember_history ? TRUE : FALSE);
    g_key_file_set_boolean(kf, grp, "remember_passwords", cfg.remember_passwords ? TRUE : FALSE);
    g_key_file_set_integer(kf, grp, "hibernate_minutes", cfg.hibernate_minutes);
    g_key_file_set_string(kf, grp, "home_url", cfg.home_url.c_str());

    gsize len = 0;
    gchar* data = g_key_file_to_data(kf, &len, nullptr);
    if (data) {
        std::string path = config_dir_ + "/config.ini";
        g_file_set_contents(path.c_str(), data, static_cast<gssize>(len), nullptr);
        g_free(data);
    }
    g_key_file_free(kf);
}

std::vector<BookmarkItem> ProfileStore::load_bookmarks() const {
    std::vector<BookmarkItem> items;
    std::string raw = read_optional_file(data_dir_ + "/bookmarks.tsv");
    if (raw.empty()) {
        items.push_back({"DuckDuckGo", "https://duckduckgo.com", true});
        items.push_back({"Wikipedia", "https://en.wikipedia.org", true});
        items.push_back({"YouTube", "https://www.youtube.com", true});
        items.push_back({"Canva", "https://www.canva.com", true});
        items.push_back({"GitHub", "https://github.com", true});
        return items;
    }

    size_t pos = 0;
    while (pos < raw.size()) {
        size_t nl = raw.find('\n', pos);
        if (nl == std::string::npos) nl = raw.size();
        std::string_view line(raw.data() + pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) continue;

        auto cols = split_tabs(line);
        if (cols.size() >= 2 && !cols[1].empty()) {
            BookmarkItem bm;
            bm.title = std::string(cols[0]);
            bm.url = std::string(cols[1]);
            bm.pinned = (cols.size() >= 3 && cols[2] == "1");
            items.push_back(std::move(bm));
        }
    }
    return items;
}

void ProfileStore::save_bookmarks(const std::vector<BookmarkItem>& items) const {
    std::string out;
    out.reserve(items.size() * 64);
    for (const auto& bm : items) {
        out += sanitize_tsv(bm.title);
        out.push_back('\t');
        out += sanitize_tsv(bm.url);
        out.push_back('\t');
        out.push_back(bm.pinned ? '1' : '0');
        out.push_back('\n');
    }
    std::string path = data_dir_ + "/bookmarks.tsv";
    g_file_set_contents(path.c_str(), out.data(), static_cast<gssize>(out.size()), nullptr);
}

std::vector<HistoryItem> ProfileStore::load_history() const {
    std::vector<HistoryItem> items;
    std::string raw = read_optional_file(data_dir_ + "/history.tsv");
    if (raw.empty()) {
        return items;
    }

    size_t pos = 0;
    while (pos < raw.size()) {
        size_t nl = raw.find('\n', pos);
        if (nl == std::string::npos) nl = raw.size();
        std::string_view line(raw.data() + pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) continue;

        auto cols = split_tabs(line);
        if (cols.size() >= 3 && !cols[2].empty()) {
            HistoryItem h;
            h.timestamp = std::strtoll(std::string(cols[0]).c_str(), nullptr, 10);
            h.title = std::string(cols[1]);
            h.url = std::string(cols[2]);
            items.push_back(std::move(h));
        }
    }
    return items;
}

void ProfileStore::save_history(const std::vector<HistoryItem>& items) const {
    const size_t start = items.size() > kMaxHistory ? (items.size() - kMaxHistory) : 0;
    std::string out;
    out.reserve((items.size() - start) * 80);
    for (size_t i = start; i < items.size(); ++i) {
        out += std::to_string(items[i].timestamp);
        out.push_back('\t');
        out += sanitize_tsv(items[i].title);
        out.push_back('\t');
        out += sanitize_tsv(items[i].url);
        out.push_back('\n');
    }
    std::string path = data_dir_ + "/history.tsv";
    g_file_set_contents(path.c_str(), out.data(), static_cast<gssize>(out.size()), nullptr);
}

void ProfileStore::upsert_history(
    std::vector<HistoryItem>& cache,
    const std::string& title,
    const std::string& url
) const {
    if (url.empty() || url.rfind("panda://", 0) == 0 || url.rfind("about:", 0) == 0 ||
        url.rfind("data:", 0) == 0 || url.rfind("blob:", 0) == 0) {
        return;
    }

    const std::string clean_title = title.empty() ? url : title;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));

    if (!cache.empty() && cache.back().url == url) {
        if (cache.back().title == clean_title) {
            return;
        }
        cache.back().title = clean_title;
        cache.back().timestamp = now;
        save_history(cache);
        return;
    }

    cache.push_back({clean_title, url, now});
    if (cache.size() > kMaxHistory + 100) {
        cache.erase(cache.begin(), cache.begin() + static_cast<ptrdiff_t>(cache.size() - kMaxHistory));
        save_history(cache);
        return;
    }

    // Fast single-line O_APPEND write without rewriting the whole file
    std::string path = data_dir_ + "/history.tsv";
    FILE* fp = std::fopen(path.c_str(), "a");
    if (fp) {
        std::string line = std::to_string(now);
        line.push_back('\t');
        line += sanitize_tsv(clean_title);
        line.push_back('\t');
        line += sanitize_tsv(url);
        line.push_back('\n');
        std::fwrite(line.data(), 1, line.size(), fp);
        std::fclose(fp);
    } else {
        save_history(cache);
    }
}

void ProfileStore::append_history(const std::string& title, const std::string& url) const {
    auto items = load_history();
    upsert_history(items, title, url);
}

void ProfileStore::clear_history() const {
    std::string path = data_dir_ + "/history.tsv";
    g_file_set_contents(path.c_str(), "", 0, nullptr);
}

std::vector<SessionTab> ProfileStore::load_session() const {
    std::vector<SessionTab> tabs;
    std::string raw = read_optional_file(data_dir_ + "/session.tsv");
    if (raw.empty()) {
        return tabs;
    }

    size_t pos = 0;
    while (pos < raw.size()) {
        size_t nl = raw.find('\n', pos);
        if (nl == std::string::npos) nl = raw.size();
        std::string_view line(raw.data() + pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) continue;

        auto cols = split_tabs(line);
        if (cols.size() >= 2 && !cols[1].empty()) {
            SessionTab t;
            t.title = std::string(cols[0]);
            t.url = std::string(cols[1]);
            t.pinned = (cols.size() >= 3 && cols[2] == "1");
            tabs.push_back(std::move(t));
        }
    }
    return tabs;
}

void ProfileStore::save_session(const std::vector<SessionTab>& tabs) const {
    std::string out;
    out.reserve(tabs.size() * 80);
    for (const auto& t : tabs) {
        if (t.url.empty()) continue;
        out += sanitize_tsv(t.title);
        out.push_back('\t');
        out += sanitize_tsv(t.url);
        out.push_back('\t');
        out.push_back(t.pinned ? '1' : '0');
        out.push_back('\n');
    }
    std::string path = data_dir_ + "/session.tsv";
    g_file_set_contents(path.c_str(), out.data(), static_cast<gssize>(out.size()), nullptr);
}

bool ProfileStore::load_or_create_vault_key(uint8_t out_key[32]) const {
    std::string key_path = data_dir_ + "/.vault.key";
    gchar* raw = nullptr;
    gsize raw_len = 0;
    if (g_file_get_contents(key_path.c_str(), &raw, &raw_len, nullptr)) {
        if (raw_len == 32) {
            std::memcpy(out_key, raw, 32);
            g_free(raw);
            g_chmod(key_path.c_str(), 0600);
            return true;
        }
        g_free(raw);
    }

    fill_random_bytes(out_key, 32);
    if (!g_file_set_contents(key_path.c_str(), reinterpret_cast<const gchar*>(out_key), 32, nullptr)) {
        return false;
    }
    g_chmod(key_path.c_str(), 0600);
    return true;
}

std::string ProfileStore::encrypt_secret(const uint8_t key[32], const std::string& plaintext) const {
    uint8_t nonce[16];
    fill_random_bytes(nonce, sizeof(nonce));

    std::vector<uint8_t> cipher(plaintext.begin(), plaintext.end());
    if (!cipher.empty()) {
        xor_keystream(key, nonce, cipher.data(), cipher.size());
    }

    uint8_t tag[32];
    compute_hmac_sha256(key, 32, nonce, sizeof(nonce), cipher.data(), cipher.size(), tag);

    std::vector<uint8_t> blob;
    blob.reserve(sizeof(nonce) + sizeof(tag) + cipher.size());
    blob.insert(blob.end(), nonce, nonce + sizeof(nonce));
    blob.insert(blob.end(), tag, tag + sizeof(tag));
    blob.insert(blob.end(), cipher.begin(), cipher.end());

    gchar* b64 = g_base64_encode(blob.data(), blob.size());
    std::string res = b64 ? b64 : "";
    g_free(b64);
    return res;
}

bool ProfileStore::decrypt_secret(const uint8_t key[32], const std::string& b64, std::string& out_plain) const {
    gsize dec_len = 0;
    guchar* dec = g_base64_decode(b64.c_str(), &dec_len);
    if (!dec || dec_len < 48) {
        g_free(dec);
        return false;
    }

    const uint8_t* nonce = dec;
    const uint8_t* tag = dec + 16;
    const uint8_t* cipher = dec + 48;
    const size_t cipher_len = dec_len - 48;

    uint8_t expected[32];
    compute_hmac_sha256(key, 32, nonce, 16, cipher, cipher_len, expected);

    uint8_t diff = 0;
    for (size_t i = 0; i < 32; ++i) {
        diff |= static_cast<uint8_t>(tag[i] ^ expected[i]);
    }
    if (diff != 0) {
        g_free(dec);
        return false;
    }

    std::vector<uint8_t> plain(cipher, cipher + cipher_len);
    if (!plain.empty()) {
        xor_keystream(key, nonce, plain.data(), plain.size());
    }
    out_plain.assign(reinterpret_cast<const char*>(plain.data()), plain.size());
    g_free(dec);
    return true;
}

std::vector<PasswordEntry> ProfileStore::load_passwords() const {
    std::vector<PasswordEntry> entries;
    std::string raw = read_optional_file(data_dir_ + "/passwords.dat");
    if (raw.empty()) {
        return entries;
    }

    uint8_t key[32];
    if (!load_or_create_vault_key(key)) {
        return entries;
    }

    size_t pos = 0;
    while (pos < raw.size()) {
        size_t nl = raw.find('\n', pos);
        if (nl == std::string::npos) nl = raw.size();
        std::string_view line(raw.data() + pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) continue;

        auto cols = split_tabs(line);
        if (cols.size() >= 4 && !cols[1].empty() && !cols[3].empty()) {
            std::string decrypted;
            if (decrypt_secret(key, std::string(cols[3]), decrypted)) {
                PasswordEntry p;
                p.updated_at = std::strtoll(std::string(cols[0]).c_str(), nullptr, 10);
                p.origin = std::string(cols[1]);
                p.username = std::string(cols[2]);
                p.password = std::move(decrypted);
                entries.push_back(std::move(p));
            }
        }
    }
    return entries;
}

bool ProfileStore::save_passwords(const std::vector<PasswordEntry>& entries) const {
    uint8_t key[32];
    if (!load_or_create_vault_key(key)) {
        return false;
    }

    std::string out;
    out.reserve(entries.size() * 128);
    for (const auto& e : entries) {
        if (e.origin.empty() || e.password.empty()) continue;
        out += std::to_string(e.updated_at);
        out.push_back('\t');
        out += sanitize_tsv(e.origin);
        out.push_back('\t');
        out += sanitize_tsv(e.username);
        out.push_back('\t');
        out += encrypt_secret(key, e.password);
        out.push_back('\n');
    }

    std::string path = data_dir_ + "/passwords.dat";
    const gboolean ok = g_file_set_contents(path.c_str(), out.data(), static_cast<gssize>(out.size()), nullptr);
    g_chmod(path.c_str(), 0600);
    return ok == TRUE;
}

std::vector<std::string> ProfileStore::load_never_save_origins() const {
    std::vector<std::string> out;
    std::string raw = read_optional_file(data_dir_ + "/passwords_never.txt");
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t nl = raw.find('\n', pos);
        if (nl == std::string::npos) nl = raw.size();
        std::string_view line(raw.data() + pos, nl - pos);
        pos = nl + 1;
        if (!line.empty()) {
            out.emplace_back(line);
        }
    }
    return out;
}

void ProfileStore::add_never_save_origin(const std::string& origin) const {
    if (origin.empty()) return;
    auto list = load_never_save_origins();
    if (std::find(list.begin(), list.end(), origin) != list.end()) {
        return;
    }
    list.push_back(origin);
    std::string out;
    for (const auto& s : list) {
        out += sanitize_tsv(s);
        out.push_back('\n');
    }
    std::string path = data_dir_ + "/passwords_never.txt";
    g_file_set_contents(path.c_str(), out.data(), static_cast<gssize>(out.size()), nullptr);
    g_chmod(path.c_str(), 0600);
}

std::string ProfileStore::read_optional_file(const std::string& path) const {
    gchar* contents = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(path.c_str(), &contents, &len, nullptr)) {
        return {};
    }
    std::string res(contents, len);
    g_free(contents);
    return res;
}

std::string ProfileStore::build_search_url(SearchEngine engine, const std::string& query) const {
    gchar* escaped = g_uri_escape_string(query.c_str(), nullptr, TRUE);
    const char* q = escaped ? escaped : "";
    std::string url;

    switch (engine) {
        case SearchEngine::Startpage:
            url = std::string("https://www.startpage.com/sp/search?query=") + q;
            break;
        case SearchEngine::Brave:
            url = std::string("https://search.brave.com/search?q=") + q;
            break;
        case SearchEngine::Google:
            url = std::string("https://www.google.com/search?q=") + q;
            break;
        case SearchEngine::Wikipedia:
            url = std::string("https://en.wikipedia.org/w/index.php?search=") + q;
            break;
        case SearchEngine::DuckDuckGo:
        default:
            url = std::string("https://duckduckgo.com/?q=") + q;
            break;
    }

    g_free(escaped);
    return url;
}

const char* ProfileStore::user_agent_string(UserAgentPreset preset) const noexcept {
    switch (preset) {
        case UserAgentPreset::FirefoxLinux:
            return "Mozilla/5.0 (X11; Linux x86_64; rv:134.0) Gecko/20100101 Firefox/134.0";
        case UserAgentPreset::ChromeLinux:
            return "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/132.0.0.0 Safari/537.36";
        case UserAgentPreset::SafariWebKit:
        default:
            return "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15";
    }
}

MemoryStats ProfileStore::sample_memory_stats() const {
    MemoryStats stats;
    const long page_bytes = ::sysconf(_SC_PAGESIZE);
    const uint64_t page_kb = page_bytes > 0 ? static_cast<uint64_t>(page_bytes / 1024) : 4;

    stats.ui_rss_kb = read_statm_rss_kb("/proc/self/statm", page_kb);
    stats.total_rss_kb = stats.ui_rss_kb;

    const pid_t self_pid = ::getpid();
    DIR* dir = ::opendir("/proc");
    if (!dir) {
        return stats;
    }

    struct dirent* ent = nullptr;
    while ((ent = ::readdir(dir)) != nullptr) {
        if (!is_numeric_name(ent->d_name)) {
            continue;
        }
        char stat_path[128];
        std::snprintf(stat_path, sizeof(stat_path), "/proc/%s/stat", ent->d_name);
        pid_t ppid = 0;
        if (!read_proc_ppid(stat_path, ppid) || ppid != self_pid) {
            continue;
        }
        char statm_path[128];
        std::snprintf(statm_path, sizeof(statm_path), "/proc/%s/statm", ent->d_name);
        const uint64_t child_rss = read_statm_rss_kb(statm_path, page_kb);
        stats.total_rss_kb += child_rss;
        ++stats.web_process_count;
    }

    ::closedir(dir);
    return stats;
}

} // namespace panda
