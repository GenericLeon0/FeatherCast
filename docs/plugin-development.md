# LeanCast Plugin Development

LeanCast plugins are native Windows DLLs loaded through `LeanCastPluginHost.exe`. The host process isolates plugin crashes from the launcher process and communicates with each plugin using one-line JSON requests and responses over standard input/output.

## Install Locations

LeanCast discovers plugins from both of these folders:

- `%APPDATA%\LeanCast\plugins\<plugin-id>\plugin.json`
- `<LeanCast install directory>\plugins\<plugin-id>\plugin.json`

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
LEANCAST_EXTENSION_EXPORT uint32_t LeanCastExtensionApiVersion() {
  return LEANCAST_EXTENSION_API_VERSION;
}

LEANCAST_EXTENSION_EXPORT uint32_t LeanCastExtensionHandleJson(
    const char* requestUtf8,
    char* responseUtf8,
    uint32_t responseCapacity);
```

`LeanCastExtensionHandleJson` returns the required response buffer size, including the trailing null byte. If `responseCapacity` is too small, do not write a partial response; return the required size so the host can retry.

## Query Request

LeanCast sends a query request when the user types:

```json
{
  "apiVersion": 1,
  "type": "query",
  "query": "demo",
  "limit": 20,
  "context": {
    "pluginId": "sample",
    "pluginDir": "C:\\Path\\To\\Plugin",
    "dataDir": "C:\\Users\\You\\AppData\\Roaming\\LeanCast"
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
      "payload": {"value": "demo"}
    }
  ]
}
```

Only `id` and `title` are required for each item. `payload` is passed back unchanged during activation.

## Activate Request

LeanCast sends activation when the user presses Enter on a plugin result:

```json
{
  "apiVersion": 1,
  "type": "activate",
  "itemId": "demo",
  "payload": {"value": "demo"},
  "context": {
    "pluginId": "sample",
    "pluginDir": "C:\\Path\\To\\Plugin",
    "dataDir": "C:\\Users\\You\\AppData\\Roaming\\LeanCast"
  }
}
```

Supported host actions:

```json
{"handled": true, "closeOverlay": true, "action": {"type": "openUrl", "value": "https://example.com"}}
{"handled": true, "closeOverlay": true, "action": {"type": "openPath", "value": "C:\\Path\\To\\File.txt"}}
{"handled": true, "closeOverlay": true, "action": {"type": "copyText", "value": "Copied text"}}
```

Use `{"handled": true}` when the plugin performed all work itself.

## Limits And Failure Behavior

- Query calls time out after about 250 ms.
- Activation calls time out after about 2 seconds.
- A response must be no larger than 1 MiB.
- Plugin exceptions and access violations are caught by the host and returned as errors.
- If a plugin process exits, times out, or returns malformed data, LeanCast marks it unavailable until extensions are reloaded.
- Logs are written to `%APPDATA%\LeanCast\extension-log.txt`.

## Testing

Build the host and run it directly with your plugin DLL:

```powershell
cmake --build build-native --config Release --target LeanCastPluginHost
build-native\Release\LeanCastPluginHost.exe C:\Path\To\SamplePlugin.dll
```

Then type one JSON request per line and read one JSON response per line. LeanCast's own host tests in `native/tests/plugin_host_tests.cpp` are useful examples for automating this.
