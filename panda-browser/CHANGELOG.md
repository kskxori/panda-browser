# Changelog

All notable changes to **Panda Browser (Linux)** are documented in this file.
Versioning follows `vMAJOR.MINOR.PATCH` (`v2.xx.xx`).

---

## [v2.01.00] - 2026-11-25

### Added
- **Encrypted Password Manager (`panda://passwords`, `Ctrl+Shift+K`)**:
  - Local encrypted credential vault (`passwords.dat` + `0600` `.vault.key` using HMAC-SHA256-CTR encryption and HMAC verification).
  - Non-intrusive `GtkInfoBar` prompt to **Save**, **Update**, or **Never save** logins per origin (`passwords_never.txt`).
  - Automatic exact-origin autofill (compatible with standard forms and React/SPA inputs).
  - Searchable password manager view with Show/Hide, Copy, Add Login, Open Site, and Delete actions.
  - Added **Remember passwords** toggle in Preferences (`Ctrl+,`).
- **Searchable Browsing History (`panda://history`, `Ctrl+H`)**:
  - Native history view with real-time search filtering, timestamps, click-to-open, individual entry removal, and Clear All.
  - Added **Remember browsing history** toggle in Preferences (`Ctrl+,`).
- **Native `panda://` Internal Pages**:
  - Converted each tab into a `GtkStack` (`home`, `history`, `bookmarks`, `passwords`, `web`) with native GTK3 rendering for internal pages.

### Changed & Improved
- **Much Faster `panda://home` Startup**:
  - Internal `panda://` pages no longer spawn a `WebKitWebProcess` until you navigate to a real website, keeping initial memory and CPU usage near idle.
  - Content Blocker now loads pre-compiled filter bytecode from cache first (`webkit_user_content_filter_store_load`) instead of recompiling rules on every launch.
  - `/proc` RAM sampling is deferred off the main navigation path and cached/refreshed every 4 seconds.
  - History updates now append/upsert cleanly (`history.tsv`, capped at 1,000 entries) and update URL autocomplete incrementally instead of rewriting the full file and list store on every page load.
- **Content Blocker Script Preservation**:
  - Only removes the custom user script when toggling user JS so built-in password capture/autofill continues working reliably.

---

## [v2.00.01] - 2026-11-25

### Fixed
- **Zig 0.17+ Build Compatibility (`build.zig`)**:
  - Replaced removed `b.getInstallPath(.bin, ...)` with `LazyPath` (`.deployed_bin` / `.install_bin`) and `addDirectoryArg2` / `addOutputFileArg2` to generate and install `panda-browser.desktop`.
- **ReleaseFast Compile Error (`src/main.zig`)**:
  - Fixed `unable to evaluate comptime expression` by removing `comptime` from the runtime `(opts.use_gui and args[1].len == 0)` check.

---

## [v2.00.00] - 2026-11-24

The first full desktop browser release of **Panda Browser**, built with **Zig + C++20 + GTK3 + WebKitGTK 4.1**.

### Added
- **Multi-Tab GTK3 Browser UI**:
  - Tabbed browsing with **Pin Tab**, **Private/Incognito Window**, **Session Restore** (`session.tsv`), and tab close/reopen ergonomics.
  - Smart omnibox with URL/search detection and dropdown autocomplete from bookmarks and history.
  - **Command Palette (`Ctrl+Shift+P`)**, **Find in Page (`Ctrl+F`)**, and **Page Zoom (`Ctrl++` / `Ctrl+-` / `Ctrl+0`)**.
  - Built-in **Download Manager** toolbar popover.
- **Home & Bookmarks (`panda://home`, `panda://bookmarks`)**:
  - New Tab / Home page with quick links, pinned/recent bookmark tiles, search bar, and live RAM/tab/shield stats.
  - Bookmarks Bar toggle, quick star button (`Ctrl+D`), and Bookmarks manager (`Ctrl+Shift+O`).
- **Low-Resource Engine & Privacy Controls**:
  - Built-in **Ad & Tracker Shield (`Ctrl+Shift+B`)**, **Force Dark Mode**, **Reader Mode**, and custom `user.css` / `user.js` injection.
  - **Idle Tab Hibernation** timer (configurable in minutes or disabled).
  - Selectable **Cache Profiles** (`Ultra-Low RAM`, `Balanced`, `Fast`), **GPU Policies** (`On-Demand`, `Always`, `Battery Saver`), **User-Agent Presets** (`Safari WebKit`, `Firefox Linux`, `Chrome Linux`), and **Search Engines** (`DuckDuckGo`, `Startpage`, `Brave`, `Google`, `Wikipedia`).
- **Desktop & CLI Integration**:
  - Desktop launcher (`panda-browser.desktop`) and custom vector icon (`panda-browser.svg`).
  - Zig CLI flags (`--version`, `--help`) and `zig build test` support alongside the C++20 engine bridge.
