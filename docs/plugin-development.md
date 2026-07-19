# FeatherCast Plugin Development

FeatherCast plugins are native Windows DLLs loaded through `FeatherCastPluginHost.exe`. The host process isolates plugin crashes from the launcher process and communicates with each plugin using one-line JSON requests and responses over standard input/output.

Plugins are trusted native code. Process isolation protects FeatherCast from many crashes, but plugins still run with the current Windows user’s filesystem and network permissions. Install only plugins whose source and publisher you trust.

## Install Locations

FeatherCast discovers plugins from both of these folders:

- `%APPDATA%\FeatherCast\plugins\<plugin-id>\plugin.json`
- `<FeatherCast install directory>\plugins\<plugin-id>\plugin.json`

Roaming user plugins win over bundled plugins with the same `id`.

## Manifest

Each plugin directory must include `plugin.json`:

```json
{
  "id": "sample",
  "name": "Sample Plugin",
  "version": "1.0.0",
  "description": "Searches sample data.",
  "dll": "SamplePlugin.dll",
  "enabled": true
}
```

Required fields are `id`, `name`, `version`, and `dll`. The DLL path must remain inside the plugin directory. `enabled` defaults to `true`.

## DLL ABI

Include `native/src/IExtension.h` and export both functions:

```cpp
FEATHERCAST_EXTENSION_EXPORT uint32_t FeatherCastExtensionApiVersion() {
  return FEATHERCAST_EXTENSION_API_VERSION;
}

FEATHERCAST_EXTENSION_EXPORT uint32_t FeatherCastExtensionHandleJson(
    const char* requestUtf8,
    char* responseUtf8,
    uint32_t responseCapacity);
```

`FeatherCastExtensionHandleJson` returns the required response buffer size, including the trailing null byte. If `responseCapacity` is too small, do not write a partial response; return the required size so the host can retry.

## Query Request

FeatherCast sends a query request when the user types:

```json
{
  "apiVersion": 2,
  "type": "query",
  "query": "demo",
  "limit": 20,
  "capabilities": {
    "detailMarkdown": true,
    "setQuery": true
  },
  "context": {
    "pluginId": "sample",
    "pluginDir": "C:\\Path\\To\\Plugin",
    "dataDir": "C:\\Users\\You\\AppData\\Roaming\\FeatherCast"
  }
}
```

Respond with up to `limit` items:

```json
{
  "items": [
    {
      "id": "demo",
      "title": "Demo Result",
      "subtitle": "From Sample Plugin",
      "keywords": ["demo", "sample"],
      "score": 42,
      "iconPath": "C:\\Path\\To\\icon.png",
      "detail": {
        "type": "markdown",
        "title": "Demo Detail",
        "body": "# Heading\n- Bullet\n`inline code`\n```\ncode block\n```"
      },
      "payload": {"value": "demo"}
    }
  ]
}
```

Only `id` and `title` are required for each item. `payload` is passed back unchanged during activation.
`detail` is optional and currently supports `type: "markdown"` for a native detail pane on the selected plugin result.

## Activate Request

FeatherCast sends activation when the user presses Enter on a plugin result:

```json
{
  "apiVersion": 2,
  "type": "activate",
  "itemId": "demo",
  "payload": {"value": "demo"},
  "context": {
    "pluginId": "sample",
    "pluginDir": "C:\\Path\\To\\Plugin",
    "dataDir": "C:\\Users\\You\\AppData\\Roaming\\FeatherCast"
  }
}
```

Supported host actions:

```json
{"handled": true, "closeOverlay": true, "action": {"type": "openUrl", "value": "https://example.com"}}
{"handled": true, "closeOverlay": true, "action": {"type": "openPath", "value": "C:\\Path\\To\\File.txt"}}
{"handled": true, "closeOverlay": true, "action": {"type": "copyText", "value": "Copied text"}}
{"handled": true, "closeOverlay": false, "action": {"type": "setQuery", "value": "next search"}}
```

Use `{"handled": true}` when the plugin performed all work itself.

v1 plugins are still accepted. The host keeps the native ABI shape unchanged and sends v1 requests to v1 plugins.

## Limits And Failure Behavior

- Query calls time out after about 250 ms.
- Activation calls time out after about 2 seconds.
- A response must be no larger than 1 MiB.
- Plugin exceptions and access violations are caught by the host and returned as errors.
- If a plugin process exits, times out, or has host I/O failures, FeatherCast restarts the plugin host and records a strike. Three consecutive strikes mark the plugin unavailable until extensions are reloaded. A successful request resets the strike count.
- Settings > Extensions shows each plugin as Available, Degraded, or Unavailable
  together with its version and latest in-memory failure reason.
- FeatherCast runs at most four plugin queries concurrently.
- Each plugin host is assigned to a kill-on-close Windows Job Object with one active process and a 256 MiB process-memory limit.
- Logs are written to `%APPDATA%\FeatherCast\extension-log.txt`.

## Testing

Build the host and run it directly with your plugin DLL:

```powershell
cmake --build build-native --config Release --target FeatherCastPluginHost
build-native\Release\FeatherCastPluginHost.exe C:\Path\To\SamplePlugin.dll
```

Then type one JSON request per line and read one JSON response per line. FeatherCast's own host tests in `native/tests/plugin_host_tests.cpp` are useful examples for automating this.
