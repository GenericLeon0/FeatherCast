#pragma once

#include "extension_protocol.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace feathercast::extensions {

struct PluginHealth {
  std::wstring id;
  std::wstring name;
  std::wstring version;
  std::filesystem::path directory;
  bool available = false;
  int failureStrikes = 0;
  std::wstring lastError;
};

class ExtensionManager {
 public:
  ExtensionManager() = default;
  ~ExtensionManager() { Shutdown(); }

  ExtensionManager(const ExtensionManager&) = delete;
  ExtensionManager& operator=(const ExtensionManager&) = delete;

  void Initialize(std::filesystem::path dataDir, std::filesystem::path exeDir, HWND notifyHwnd, UINT notifyMessage) {
    stop_.store(false);
    dataDir_ = std::move(dataDir);
    exeDir_ = std::move(exeDir);
    notifyHwnd_ = notifyHwnd;
    notifyMessage_ = notifyMessage;
    Reload();
    StartQueryWorker();
  }

  void OnBackground() {
    {
      std::lock_guard cacheLock(cacheMutex_);
      cache_.clear();
    }
    {
      std::lock_guard pluginsLock(pluginsMutex_);
      for (const auto& plugin : plugins_) {
        std::lock_guard ioLock(plugin->ioMutex);
        StopProcess(*plugin);
      }
    }
  }

  void Shutdown() {
    {
      std::lock_guard lock(queryMutex_);
      stop_.store(true);
      pendingQuery_.reset();
    }
    queryCv_.notify_all();
    if (queryThread_.joinable()) {
      queryThread_.request_stop();
      queryThread_.join();
    }

    std::lock_guard pluginsLock(pluginsMutex_);
    for (const auto& plugin : plugins_) {
      plugin->available.store(false);
      std::lock_guard ioLock(plugin->ioMutex);
      StopProcess(*plugin);
    }
    plugins_.clear();
  }

  void Reload() {
    std::vector<std::shared_ptr<Plugin>> oldPlugins;
    {
      std::lock_guard pluginsLock(pluginsMutex_);
      oldPlugins.swap(plugins_);
    }
    for (const auto& plugin : oldPlugins) {
      plugin->available.store(false);
      std::lock_guard ioLock(plugin->ioMutex);
      StopProcess(*plugin);
    }

    auto discovery = DiscoverManifests(dataDir_, exeDir_);
    for (const auto& error : discovery.errors) Log(error);
    std::vector<std::shared_ptr<Plugin>> loaded;
    loaded.reserve(discovery.manifests.size());
    for (auto& manifest : discovery.manifests) {
      auto plugin = std::make_shared<Plugin>();
      plugin->manifest = std::move(manifest);
      loaded.push_back(std::move(plugin));
    }
    const size_t loadedCount = loaded.size();
    {
      std::lock_guard pluginsLock(pluginsMutex_);
      plugins_ = std::move(loaded);
    }
    Log(L"Loaded " + std::to_wstring(loadedCount) + L" extension(s)");

    {
      std::lock_guard cacheLock(cacheMutex_);
      cache_.clear();
    }
  }

  void RequestQuery(std::wstring query, unsigned long long generation) {
    if (TrimWide(query).empty()) return;
    {
      std::lock_guard cacheLock(cacheMutex_);
      if (cache_.contains(query)) return;
    }

    {
      std::lock_guard queryLock(queryMutex_);
      if (runningQuery_ == query) {
        runningGeneration_ = generation;
        latestRequestedGeneration_ = generation;
        return;
      }
      if (pendingQuery_ && pendingQuery_->query == query) {
        pendingQuery_->generation = generation;
        latestRequestedGeneration_ = generation;
        return;
      }
      pendingQuery_ = PendingQuery{std::move(query), generation};
      latestRequestedGeneration_ = generation;
    }
    queryCv_.notify_one();
  }

  std::vector<QueryResultItem> CachedResultsFor(const std::wstring& query) const {
    std::lock_guard cacheLock(cacheMutex_);
    if (const auto found = cache_.find(query); found != cache_.end()) return found->second;
    return {};
  }

  std::vector<PluginHealth> Health() const {
    std::vector<PluginHealth> out;
    std::lock_guard pluginsLock(pluginsMutex_);
    out.reserve(plugins_.size());
    for (const auto& plugin : plugins_) {
      std::lock_guard healthLock(plugin->healthMutex);
      out.push_back({
          plugin->manifest.id,
          plugin->manifest.name,
          plugin->manifest.version,
          plugin->manifest.directory,
          plugin->available.load(),
          plugin->failureStrikes.load(),
          plugin->lastError,
      });
    }
    return out;
  }

  std::optional<ActivationResponse> Activate(const QueryResultItem& item,
                                             std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    std::shared_ptr<Plugin> plugin;
    {
      std::lock_guard pluginsLock(pluginsMutex_);
      for (const auto& candidate : plugins_) {
        if (candidate->manifest.id == item.pluginId) {
          plugin = candidate;
          break;
        }
      }
    }
    if (!plugin || !plugin->available.load()) return std::nullopt;

    std::string response;
    if (!SendRequest(*plugin, BuildActivateRequestJson(plugin->manifest, dataDir_, item), timeout, response)) {
      return std::nullopt;
    }
    return ParseActivationResponse(response);
  }

 private:
  struct Plugin {
    Manifest manifest;
    std::atomic<bool> available = true;
    std::atomic<int> failureStrikes = 0;
    mutable std::mutex healthMutex;
    std::wstring lastError;
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    std::mutex ioMutex;
  };

  struct PendingQuery {
    std::wstring query;
    unsigned long long generation = 0;
  };

  static std::wstring TrimWide(std::wstring value) {
    auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) { return std::iswspace(ch) != 0; }).base();
    if (first >= last) return L"";
    return std::wstring(first, last);
  }

  static void CloseHandleIfSet(HANDLE& handle) {
    if (handle && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
      handle = nullptr;
    }
  }

  static std::wstring QuoteCommandArg(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::wstring out = L"\"";
    for (const wchar_t ch : value) {
      if (ch == L'"') out += L"\\\"";
      else out.push_back(ch);
    }
    out.push_back(L'"');
    return out;
  }

  static bool ProcessRunning(HANDLE process) {
    return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  }

  void StartQueryWorker() {
    if (queryThread_.joinable()) return;
    queryThread_ = std::jthread([this](std::stop_token stopToken) {
      for (;;) {
        PendingQuery pending;
        {
          std::unique_lock lock(queryMutex_);
          queryCv_.wait(lock, [&] {
            return pendingQuery_.has_value() || stop_.load() || stopToken.stop_requested();
          });
          if (stop_.load() || stopToken.stop_requested()) return;
          pending = std::move(*pendingQuery_);
          pendingQuery_.reset();
          runningQuery_ = pending.query;
          runningGeneration_ = pending.generation;
        }

        std::vector<QueryResultItem> results;
        try {
          results = QueryPlugins(pending.query, stopToken);
        } catch (...) {
          Log(L"plugin query coordinator failed; the launcher kept running");
        }
        {
          std::lock_guard cacheLock(cacheMutex_);
          if (cache_.size() > 32) cache_.clear();
          cache_[pending.query] = std::move(results);
        }

        bool newest = false;
        {
          std::lock_guard queryLock(queryMutex_);
          newest = runningQuery_ == pending.query && runningGeneration_ == latestRequestedGeneration_;
          if (runningQuery_ == pending.query) runningQuery_.clear();
        }
        if (newest && notifyHwnd_) PostMessageW(notifyHwnd_, notifyMessage_, 0, 0);
      }
    });
  }

  std::vector<QueryResultItem> QueryPlugins(const std::wstring& query, std::stop_token stopToken) {
    std::vector<QueryResultItem> results;
    std::vector<std::shared_ptr<Plugin>> plugins;
    {
      std::lock_guard pluginsLock(pluginsMutex_);
      plugins = plugins_;
    }

    std::mutex resultsMutex;
    std::atomic<size_t> nextPlugin = 0;
    std::vector<std::jthread> workers;
    const size_t workerCount = std::min<size_t>(4, plugins.size());
    workers.reserve(workerCount);
    for (size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
      workers.emplace_back([&](std::stop_token workerStop) {
        try {
          for (;;) {
            const size_t pluginIndex =
                nextPlugin.fetch_add(1, std::memory_order_relaxed);
            if (pluginIndex >= plugins.size()) return;
            const auto& plugin = plugins[pluginIndex];
            if (workerStop.stop_requested() || stopToken.stop_requested() ||
                stop_.load() || !plugin->available.load()) {
              continue;
            }

            std::string response;
            if (!SendRequest(
                    *plugin,
                    BuildQueryRequestJson(plugin->manifest, dataDir_, query,
                                          kDefaultQueryLimit),
                    std::chrono::milliseconds(250), response)) {
              continue;
            }

            auto parsed = ParseQueryResponse(response, kDefaultQueryLimit);
            if (!parsed) {
              Log(plugin->manifest.id + L": invalid query response");
              continue;
            }
            std::lock_guard resultsLock(resultsMutex);
            for (auto& item : parsed->items) {
              item.pluginId = plugin->manifest.id;
              item.pluginName = plugin->manifest.name;
              results.push_back(std::move(item));
            }
          }
        } catch (...) {
          Log(L"plugin query worker failed; remaining plugins were skipped");
        }
      });
    }
    for (auto& worker : workers) {
      if (worker.joinable()) worker.join();
    }

    std::sort(results.begin(), results.end(), [](const QueryResultItem& a, const QueryResultItem& b) {
      if (a.score != b.score) return a.score > b.score;
      return a.title < b.title;
    });
    return results;
  }

  bool EnsureProcess(Plugin& plugin) {
    if (!plugin.available.load()) return false;
    if (ProcessRunning(plugin.process)) return true;

    StopProcess(plugin);

    const auto hostPath = exeDir_ / L"FeatherCastPluginHost.exe";
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE childStdinRead = nullptr;
    HANDLE parentStdinWrite = nullptr;
    HANDLE parentStdoutRead = nullptr;
    HANDLE childStdoutWrite = nullptr;
    HANDLE childStderr = nullptr;

    if (!CreatePipe(&childStdinRead, &parentStdinWrite, &inheritable, 0) ||
        !CreatePipe(&parentStdoutRead, &childStdoutWrite, &inheritable, 0)) {
      CloseHandleIfSet(childStdinRead);
      CloseHandleIfSet(parentStdinWrite);
      CloseHandleIfSet(parentStdoutRead);
      CloseHandleIfSet(childStdoutWrite);
      MarkUnavailable(plugin, L"failed to create plugin host pipes");
      return false;
    }

    SetHandleInformation(parentStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentStdoutRead, HANDLE_FLAG_INHERIT, 0);
    childStderr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childStdinRead;
    startup.StartupInfo.hStdOutput = childStdoutWrite;
    startup.StartupInfo.hStdError = childStderr ? childStderr : childStdoutWrite;

    SIZE_T attributesSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributesSize);
    std::vector<BYTE> attributesBuffer(attributesSize);
    startup.lpAttributeList =
        reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributesBuffer.data());
    HANDLE inheritedHandles[] = {
      childStdinRead,
      childStdoutWrite,
      startup.StartupInfo.hStdError,
    };
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributesSize) ||
        !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr)) {
      if (startup.lpAttributeList) DeleteProcThreadAttributeList(startup.lpAttributeList);
      CloseHandleIfSet(childStdinRead);
      CloseHandleIfSet(parentStdinWrite);
      CloseHandleIfSet(parentStdoutRead);
      CloseHandleIfSet(childStdoutWrite);
      CloseHandleIfSet(childStderr);
      MarkUnavailable(plugin, L"failed to restrict plugin host handle inheritance");
      return false;
    }

    PROCESS_INFORMATION process{};
    std::wstring command = QuoteCommandArg(hostPath) + L" " + QuoteCommandArg(plugin.manifest.dllPath);
    const BOOL created = CreateProcessW(hostPath.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED |
                                            EXTENDED_STARTUPINFO_PRESENT,
                                        nullptr, nullptr,
                                        &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(startup.lpAttributeList);

    CloseHandleIfSet(childStdinRead);
    CloseHandleIfSet(childStdoutWrite);
    CloseHandleIfSet(childStderr);

    if (!created) {
      CloseHandleIfSet(parentStdinWrite);
      CloseHandleIfSet(parentStdoutRead);
      MarkUnavailable(plugin, L"failed to start FeatherCastPluginHost.exe");
      return false;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = 256ull * 1024ull * 1024ull;
    if (!job ||
        !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits)) ||
        !AssignProcessToJobObject(job, process.hProcess)) {
      if (job) CloseHandle(job);
      TerminateProcess(process.hProcess, 0);
      WaitForSingleObject(process.hProcess, 1000);
      CloseHandleIfSet(process.hThread);
      CloseHandleIfSet(process.hProcess);
      CloseHandleIfSet(parentStdinWrite);
      CloseHandleIfSet(parentStdoutRead);
      MarkUnavailable(plugin, L"failed to apply plugin host resource limits");
      return false;
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
      CloseHandle(job);
      TerminateProcess(process.hProcess, 0);
      WaitForSingleObject(process.hProcess, 1000);
      CloseHandleIfSet(process.hThread);
      CloseHandleIfSet(process.hProcess);
      CloseHandleIfSet(parentStdinWrite);
      CloseHandleIfSet(parentStdoutRead);
      MarkUnavailable(plugin, L"failed to start constrained plugin host");
      return false;
    }
    CloseHandleIfSet(process.hThread);
    plugin.job = job;
    plugin.process = process.hProcess;
    plugin.stdinWrite = parentStdinWrite;
    plugin.stdoutRead = parentStdoutRead;
    return true;
  }

  bool SendRequest(Plugin& plugin, const std::string& request, std::chrono::milliseconds timeout,
                   std::string& response) {
    std::lock_guard ioLock(plugin.ioMutex);
    if (!EnsureProcess(plugin)) return false;

    const std::string line = request + "\n";
    DWORD written = 0;
    if (!WriteFile(plugin.stdinWrite, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) ||
        written != line.size()) {
      RecordRequestFailure(plugin, L"plugin host write failed");
      return false;
    }

    std::string buffer;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (!ProcessRunning(plugin.process)) {
        RecordRequestFailure(plugin, L"plugin host exited unexpectedly");
        return false;
      }

      DWORD available = 0;
      if (!PeekNamedPipe(plugin.stdoutRead, nullptr, 0, nullptr, &available, nullptr)) {
        RecordRequestFailure(plugin, L"plugin host read failed");
        return false;
      }

      if (available > 0) {
        char chunk[4096]{};
        const DWORD toRead = std::min<DWORD>(available, static_cast<DWORD>(sizeof(chunk)));
        DWORD read = 0;
        if (!ReadFile(plugin.stdoutRead, chunk, toRead, &read, nullptr)) {
          RecordRequestFailure(plugin, L"plugin host read failed");
          return false;
        }
        buffer.append(chunk, chunk + read);
        if (buffer.size() > kMaxResponseBytes) {
          RecordRequestFailure(plugin, L"plugin host response exceeded 1 MiB");
          return false;
        }
        if (const size_t newline = buffer.find('\n'); newline != std::string::npos) {
          response = buffer.substr(0, newline);
          if (!response.empty() && response.back() == '\r') response.pop_back();
          RecordRequestSuccess(plugin);
          return true;
        }
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }

    RecordRequestFailure(plugin, L"plugin host timed out");
    return false;
  }

  void RecordRequestSuccess(Plugin& plugin) {
    plugin.failureStrikes.store(0);
    std::lock_guard healthLock(plugin.healthMutex);
    plugin.lastError.clear();
  }

  void RecordRequestFailure(Plugin& plugin, const std::wstring& reason) {
    const int strikes = plugin.failureStrikes.fetch_add(1) + 1;
    {
      std::lock_guard healthLock(plugin.healthMutex);
      plugin.lastError = reason;
    }
    Log(plugin.manifest.id + L": " + reason + L" (strike " +
        std::to_wstring(strikes) + L"/3)");
    StopProcess(plugin);
    if (strikes >= 3) {
      plugin.available.store(false);
      Log(plugin.manifest.id + L": disabled after repeated plugin host failures");
    }
  }

  void MarkUnavailable(Plugin& plugin, const std::wstring& reason) {
    Log(plugin.manifest.id + L": " + reason);
    plugin.available.store(false);
    plugin.failureStrikes.store(3);
    {
      std::lock_guard healthLock(plugin.healthMutex);
      plugin.lastError = reason;
    }
    StopProcess(plugin);
  }

  void StopProcess(Plugin& plugin) {
    CloseHandleIfSet(plugin.stdinWrite);
    CloseHandleIfSet(plugin.stdoutRead);
    if (plugin.process) {
      if (WaitForSingleObject(plugin.process, 0) == WAIT_TIMEOUT) {
        TerminateProcess(plugin.process, 0);
        WaitForSingleObject(plugin.process, 1000);
      }
      CloseHandleIfSet(plugin.process);
    }
    CloseHandleIfSet(plugin.job);
  }

  void Log(const std::wstring& message) const {
    if (dataDir_.empty()) return;
    std::lock_guard lock(logMutex_);
    std::error_code ec;
    std::filesystem::create_directories(dataDir_, ec);
    std::ofstream file(dataDir_ / L"extension-log.txt", std::ios::binary | std::ios::app);
    if (!file) return;
    const auto now = static_cast<long long>(std::time(nullptr));
    file << now << " " << WideToUtf8(message) << "\n";
  }

  std::filesystem::path dataDir_;
  std::filesystem::path exeDir_;
  HWND notifyHwnd_ = nullptr;
  UINT notifyMessage_ = 0;

  mutable std::mutex logMutex_;
  mutable std::mutex cacheMutex_;
  std::map<std::wstring, std::vector<QueryResultItem>> cache_;

  mutable std::mutex pluginsMutex_;
  std::vector<std::shared_ptr<Plugin>> plugins_;

  std::jthread queryThread_;
  std::mutex queryMutex_;
  std::condition_variable queryCv_;
  std::optional<PendingQuery> pendingQuery_;
  std::wstring runningQuery_;
  unsigned long long runningGeneration_ = 0;
  unsigned long long latestRequestedGeneration_ = 0;
  std::atomic<bool> stop_ = false;
};

}  // namespace feathercast::extensions
