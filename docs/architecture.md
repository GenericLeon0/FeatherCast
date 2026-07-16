# FeatherCast Architecture

FeatherCast is a Windows-native C++23 launcher with a Win32 message-loop boundary and Direct2D/DirectWrite rendering.

## Runtime Services

- The UI thread owns windows, input state, rendering, and user-visible settings state.
- Search queries run on a dedicated coalescing worker.
- Query-independent search snapshots are prepared off-thread and published only when their data revision is still current.
- App discovery and optional file indexing share a persistent generation-cancelled worker; refresh requests never synchronously join that worker from the UI thread.
- Settings and clipboard writes use a serialized persistence executor that drains during orderly shutdown.
- Native plugins run out of process with request timeouts, strike handling, bounded query concurrency, and Job Object cleanup/resource limits.

## Data Locations

- `%APPDATA%\FeatherCast`: settings, snippets, themes, and user plugins.
- `%LOCALAPPDATA%\FeatherCast`: SQLite operational data, icon cache, updates, and diagnostics.

The SQLite database uses a schema version, busy timeout, integrity checking, and corrupt-file quarantine. Clipboard text and previews are protected with user-scoped Windows DPAPI. The file index is disposable and can be rebuilt; clipboard recovery failures are surfaced to the user.
