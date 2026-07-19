# FeatherCast Releasing

FeatherCast releases are distributed through GitHub Releases for `GenericLeon0/FeatherCast`. The in-app updater checks the latest stable release and expects the Windows installer plus a SHA-256 sidecar file.

## Automated Release (preferred)

Pushing a tag `vX.Y.Z` triggers `.github/workflows/ci.yml`: it builds, runs tests, packages via CPack, generates `.sha256` sidecars, and creates a **draft** GitHub Release with all assets attached. The updater ignores drafts, so verify the draft and publish it manually. The manual flow below remains as fallback.

Signed release tags use the `WINDOWS_CERTIFICATE_BASE64` and `WINDOWS_CERTIFICATE_PASSWORD` secrets plus the `FEATHERCAST_EXPECTED_PUBLISHER` and `FEATHERCAST_ALLOWED_SIGNER_THUMBPRINTS` repository variables. The thumbprint variable is a semicolon-separated list of SHA-256 signer-certificate thumbprints; keep the old and new certificates listed together during rotation. CI rejects a partial signing configuration. When all four values are present, it signs and timestamps the application, plugin host, and installer, then verifies their status, publisher, timestamp, and certificate pin.

When none of the four values is configured, CI creates unsigned artifacts as a manual-installation fallback. The release notes must identify such a release as unsigned. Builds without embedded signer pins can check for releases, but the updater will only open the GitHub Release page instead of downloading or launching the installer.

## Build And Test

Run from a Visual Studio developer PowerShell:

```powershell
cmake --preset windows-x64
cmake --build --preset release
ctest --preset release
cpack --config build-native/CPackConfig.cmake -C Release
```

Manual release builds must also configure both signer values:

```powershell
cmake -S . -B build-native -A x64 `
  "-DFEATHERCAST_EXPECTED_PUBLISHER=<publisher display name>" `
  "-DFEATHERCAST_ALLOWED_SIGNER_THUMBPRINTS=<sha256 thumbprint>"
```

A local or pull-request build without a signer thumbprint can check for releases but will not download or launch an installer. If it is published, it must be clearly identified as unsigned and distributed for manual installation only.

Release binaries use the static MSVC runtime. Before packaging, CI audits PE
dependencies and runs these side-effect-free startup checks:

```powershell
build-native\Release\FeatherCast.exe --self-test
build-native\Release\FeatherCastPluginHost.exe --self-test
```

The configured package filename is:

```text
FeatherCast-<version>-win64.exe
FeatherCast-<version>-win64.zip
```

The version comes from `project(... VERSION ...)` in `CMakeLists.txt` and is compiled into `version.hpp`.

## Release Assets

For a release tag `v0.3.0` or `0.3.0`, upload:

```text
FeatherCast-0.3.0-win64.exe
FeatherCast-0.3.0-win64.exe.sha256
```

Create the hash sidecar with:

```powershell
Get-FileHash .\FeatherCast-0.3.0-win64.exe -Algorithm SHA256 |
  ForEach-Object { "$($_.Hash.ToLowerInvariant())  FeatherCast-0.3.0-win64.exe" } |
  Set-Content -Encoding ascii .\FeatherCast-0.3.0-win64.exe.sha256
```

The updater will not run an installer unless the `.sha256` asset exists and matches the downloaded file.

## Updater Behavior

- FeatherCast checks GitHub Releases automatically at startup at most once per day when update checks are enabled.
- Users can run `Check for Updates` from the launcher or Settings.
- Draft and prerelease GitHub releases are ignored.
- Tags may be `vX.Y.Z` or `X.Y.Z`.
- If the user dismisses an automatic update prompt, FeatherCast will not prompt again for that same version.
- Installers are downloaded to `%LOCALAPPDATA%\FeatherCast\updates`.
- Update logs are written to `%LOCALAPPDATA%\FeatherCast\update-log.txt`.

## Suggested Release Flow

1. Update `project(FeatherCastNative VERSION X.Y.Z ...)` in `CMakeLists.txt`.
2. Build, test, and package with the commands above.
3. Generate the `.sha256` sidecar for the NSIS installer.
4. Create a GitHub Release tagged `vX.Y.Z`.
5. Upload the installer and sidecar assets.
6. Verify `Check for Updates` from the previous version detects the new release.

## 0.6 And Unsigned 0.7 Upgrade Note

The published 0.6 installer was unsigned and its binary contains no trusted
signer pin. The initial 0.7 release is also unsigned because repository signing
credentials were not configured. Users must download and install it manually
from the GitHub Release page. A later signed installation with embedded signer
pins can use the verified in-app update path.
