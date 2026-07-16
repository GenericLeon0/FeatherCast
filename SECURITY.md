# Security Policy

## Reporting

Please report suspected security vulnerabilities privately to the repository owner rather than opening a public issue. Include the affected FeatherCast version, reproduction steps, and any relevant logs with personal paths or clipboard contents removed.

## Security Boundaries

- FeatherCast is a local Windows desktop application and does not include AI or account services.
- Clipboard history and file indexing are opt-in and stored under `%LOCALAPPDATA%\FeatherCast`. Clipboard text and previews are protected with user-scoped Windows DPAPI before being written to SQLite.
- Native plugins execute with the current Windows user's permissions. The plugin host provides crash and resource isolation, but it is not a security sandbox. Install plugins only from trusted sources.
- Update installers must pass SHA-256 verification, Windows Authenticode validation, and a compiled signer-certificate pin. Builds without a signer pin cannot install updates.

## Supported Versions

Security fixes are provided for the latest published stable version.
