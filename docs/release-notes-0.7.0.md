# FeatherCast 0.7.0

FeatherCast 0.7 adds a native Library manager under Settings for creating,
editing, and deleting snippets and quicklinks. The Extensions page now reports
each plugin's version, availability, failure strikes, and latest error.

Search ranking and launcher motion have been refined for predictable result
tiers, typo tolerance, smooth scrolling, display-paced transitions, and reduced
motion support.

Search & Files adds selectable `@files`, `@apps`, `@windows`, `@commands`,
`@clipboard`, and `@snippets` scopes. The opt-in file index now watches selected
local folders recursively, while a separate Privacy opt-in enables bounded local
full-text search for supported text/source files. `Ctrl+Space` opens an
on-demand text, image, or metadata preview without persisting preview content.

Existing installations migrate their local SQLite data to schema v3 after a
backup. Full-text indexing stays disabled after upgrade until explicitly
enabled; clipboard data remains preserved and DPAPI-protected.

Release packages are now self-contained through the static MSVC runtime. This
0.7.0 publication is unsigned because repository signing credentials are not
configured, so Windows may show an unknown-publisher warning. It is intended for
manual installation from the official GitHub Release page. The in-app updater
continues to require timestamped Authenticode signatures whose publisher and
signer certificate match pins embedded in the application.

## Manual upgrade from 0.6

The 0.6 installer was unsigned and that build cannot securely install an update
inside the app. Download the unsigned 0.7.0 installer from the official GitHub
Release page and run it manually. A future signed build with embedded signer
pins can use FeatherCast's verified in-app updater.
