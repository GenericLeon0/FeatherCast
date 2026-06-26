# FeatherCast Releasing

FeatherCast releases are distributed through GitHub Releases for `GenericLeon0/FeatherCast`. The in-app updater checks the latest stable release and expects the Windows installer plus a SHA-256 sidecar file.

## Build And Test

Run from a Visual Studio developer PowerShell:

```powershell
cmake -S . -B build-native -G "Visual Studio 18 2026" -A x64
cmake --build build-native --config Release
ctest --test-dir build-native -C Release
cpack --config build-native/CPackConfig.cmake -C Release
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
- Update logs are written to `%APPDATA%\FeatherCast\update-log.txt`.

## Suggested Release Flow

1. Update `project(FeatherCastNative VERSION X.Y.Z ...)` in `CMakeLists.txt`.
2. Build, test, and package with the commands above.
3. Generate the `.sha256` sidecar for the NSIS installer.
4. Create a GitHub Release tagged `vX.Y.Z`.
5. Upload the installer and sidecar assets.
6. Verify `Check for Updates` from the previous version detects the new release.
