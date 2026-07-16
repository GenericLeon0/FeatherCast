#include "extension_manager.hpp"
#include "test_framework.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

void CloseHandleIfSet(HANDLE& handle) {
  if (handle && handle != INVALID_HANDLE_VALUE) {
    CloseHandle(handle);
    handle = nullptr;
  }
}

std::wstring Quote(const std::wstring& value) {
  std::wstring out = L"\"";
  for (const wchar_t ch : value) {
    if (ch == L'"') out += L"\\\"";
    else out.push_back(ch);
  }
  out.push_back(L'"');
  return out;
}

class HostSession {
 public:
  HostSession(const std::wstring& hostPath, const std::wstring& dllPath) {
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE childStdinRead = nullptr;
    HANDLE parentStdinWrite = nullptr;
    HANDLE parentStdoutRead = nullptr;
    HANDLE childStdoutWrite = nullptr;
    const BOOL inputPipeCreated = CreatePipe(&childStdinRead, &parentStdinWrite, &inheritable, 0);
    assert(inputPipeCreated);
    const BOOL outputPipeCreated = CreatePipe(&parentStdoutRead, &childStdoutWrite, &inheritable, 0);
    assert(outputPipeCreated);
    SetHandleInformation(parentStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentStdoutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childStdinRead;
    startup.hStdOutput = childStdoutWrite;
    startup.hStdError = childStdoutWrite;

    std::wstring command = Quote(hostPath) + L" " + Quote(dllPath);
    const BOOL processCreated =
        CreateProcessW(hostPath.c_str(), command.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process_);
    assert(processCreated);

    CloseHandleIfSet(childStdinRead);
    CloseHandleIfSet(childStdoutWrite);
    CloseHandleIfSet(process_.hThread);
    stdinWrite_ = parentStdinWrite;
    stdoutRead_ = parentStdoutRead;
  }

  ~HostSession() {
    CloseHandleIfSet(stdinWrite_);
    CloseHandleIfSet(stdoutRead_);
    if (process_.hProcess) {
      if (WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT) {
        TerminateProcess(process_.hProcess, 0);
        WaitForSingleObject(process_.hProcess, 1000);
      }
      CloseHandle(process_.hProcess);
    }
  }

  std::string Send(const std::string& request, std::chrono::milliseconds timeout) {
    const std::string line = request + "\n";
    DWORD written = 0;
    const BOOL writeSucceeded =
        WriteFile(stdinWrite_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    assert(writeSucceeded);
    assert(written == line.size());

    std::string buffer;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      DWORD available = 0;
      const BOOL peekSucceeded = PeekNamedPipe(stdoutRead_, nullptr, 0, nullptr, &available, nullptr);
      assert(peekSucceeded);
      if (available > 0) {
        char chunk[4096]{};
        DWORD read = 0;
        const BOOL readSucceeded =
            ReadFile(stdoutRead_, chunk, std::min<DWORD>(available, sizeof(chunk)), &read, nullptr);
        assert(readSucceeded);
        buffer.append(chunk, chunk + read);
        if (const size_t newline = buffer.find('\n'); newline != std::string::npos) {
          std::string response = buffer.substr(0, newline);
          if (!response.empty() && response.back() == '\r') response.pop_back();
          return response;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return "";
  }

 private:
  PROCESS_INFORMATION process_{};
  HANDLE stdinWrite_ = nullptr;
  HANDLE stdoutRead_ = nullptr;
};

DWORD RunAndWait(const std::wstring& hostPath, const std::wstring& dllPath) {
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::wstring command = Quote(hostPath) + L" " + Quote(dllPath);
  const BOOL processCreated =
      CreateProcessW(hostPath.c_str(), command.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  assert(processCreated);
  CloseHandleIfSet(process.hThread);
  WaitForSingleObject(process.hProcess, 5000);
  DWORD exitCode = 0;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandleIfSet(process.hProcess);
  return exitCode;
}

void WriteUtf8(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
}

bool WaitForPluginResult(feathercast::extensions::ExtensionManager& manager,
                         const std::wstring& query,
                         const std::wstring& title,
                         std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    for (const auto& item : manager.CachedResultsFor(query)) {
      if (item.title == title) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  assert(argc == 5);
  const std::wstring hostPath = argv[1];
  const std::wstring pluginPath = argv[2];
  const std::wstring v1PluginPath = argv[3];
  const std::wstring badApiPath = argv[4];

  {
    HostSession session(hostPath, pluginPath);
    const auto query = session.Send("{\"apiVersion\":1,\"type\":\"query\",\"query\":\"demo\",\"limit\":20}",
                                    std::chrono::seconds(2));
    assert(query.find("\"Demo Result\"") != std::string::npos);

    const auto activation = session.Send("{\"apiVersion\":1,\"type\":\"activate\",\"itemId\":\"demo\",\"payload\":{}}",
                                         std::chrono::seconds(2));
    assert(activation.find("\"copyText\"") != std::string::npos);

    const auto thrown = session.Send("{\"apiVersion\":1,\"type\":\"query\",\"query\":\"throw\",\"limit\":20}",
                                     std::chrono::seconds(2));
    assert(thrown.find("plugin-call-failed") != std::string::npos);

    const auto stillAlive = session.Send("{\"apiVersion\":1,\"type\":\"query\",\"query\":\"demo\",\"limit\":20}",
                                         std::chrono::seconds(2));
    assert(stillAlive.find("\"Demo Result\"") != std::string::npos);

    const auto huge = session.Send("{\"apiVersion\":1,\"type\":\"query\",\"query\":\"huge\",\"limit\":20}",
                                   std::chrono::seconds(2));
    assert(huge.find("plugin-call-failed") != std::string::npos);

    const auto malformed = session.Send("{\"apiVersion\":1,\"type\":\"query\",\"query\":\"malformed\",\"limit\":20}",
                                        std::chrono::seconds(2));
    assert(malformed == "{\"items\":[");

    const auto v2 = session.Send("{\"apiVersion\":2,\"type\":\"query\",\"query\":\"version\",\"limit\":20}",
                                 std::chrono::seconds(2));
    assert(v2.find("\"API v2\"") != std::string::npos);

    const auto detail = session.Send("{\"apiVersion\":2,\"type\":\"query\",\"query\":\"detail\",\"limit\":20}",
                                     std::chrono::seconds(2));
    assert(detail.find("\"detail\"") != std::string::npos);
    assert(detail.find("\"markdown\"") != std::string::npos);

    const auto setQuery = session.Send("{\"apiVersion\":2,\"type\":\"activate\",\"itemId\":\"set-query\",\"payload\":{}}",
                                       std::chrono::seconds(2));
    assert(setQuery.find("\"setQuery\"") != std::string::npos);
  }

  {
    HostSession session(hostPath, v1PluginPath);
    const auto v1 = session.Send("{\"apiVersion\":2,\"type\":\"query\",\"query\":\"version\",\"limit\":20}",
                                 std::chrono::seconds(2));
    assert(v1.find("\"API v1\"") != std::string::npos);
  }

  {
    HostSession session(hostPath, pluginPath);
    const auto slow = session.Send("{\"apiVersion\":1,\"type\":\"query\",\"query\":\"slow\",\"limit\":20}",
                                   std::chrono::milliseconds(100));
    assert(slow.empty());
  }

  {
    const auto tempRoot = std::filesystem::temp_directory_path() / L"FeatherCastParallelExtensionTests";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
    const auto dataDir = tempRoot / L"data";
    for (const auto& id : {L"fast", L"slow-one", L"slow-two", L"slow-three"}) {
      const auto pluginDir = dataDir / L"plugins" / id;
      std::filesystem::create_directories(pluginDir, ec);
      const auto dll = pluginDir / L"plugin.dll";
      const BOOL copied = CopyFileW(pluginPath.c_str(), dll.c_str(), FALSE);
      assert(copied);
      WriteUtf8(pluginDir / L"plugin.json",
                "{\"id\":\"" + feathercast::extensions::WideToUtf8(id) + "\",\"name\":\"Parallel Test\","
                "\"version\":\"1.0\",\"dll\":\"plugin.dll\"}");
    }

    feathercast::extensions::ExtensionManager manager;
    manager.Initialize(dataDir, std::filesystem::path(hostPath).parent_path(), nullptr, 0);
    const auto start = std::chrono::steady_clock::now();
    manager.RequestQuery(L"parallel", 1);
    assert(WaitForPluginResult(manager, L"parallel", L"Demo Result", std::chrono::seconds(2)));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    assert(elapsed < std::chrono::milliseconds(700));
    manager.Shutdown();
    std::filesystem::remove_all(tempRoot, ec);
  }

  {
    const auto tempRoot = std::filesystem::temp_directory_path() / L"FeatherCastExtensionManagerTests";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);

    const auto dataDir = tempRoot / L"data";
    const auto pluginDir = dataDir / L"plugins" / L"manager";
    std::filesystem::create_directories(pluginDir, ec);
    const auto managerDll = pluginDir / L"manager.dll";
    const BOOL pluginCopied = CopyFileW(pluginPath.c_str(), managerDll.c_str(), FALSE);
    assert(pluginCopied);
    WriteUtf8(pluginDir / L"plugin.json",
              "{\"id\":\"manager\",\"name\":\"Manager Test\",\"version\":\"1.0\",\"dll\":\"manager.dll\"}");

    feathercast::extensions::ExtensionManager manager;
    manager.Initialize(dataDir, std::filesystem::path(hostPath).parent_path(), nullptr, 0);

    manager.RequestQuery(L"slow-one", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    manager.RequestQuery(L"demo-after-timeout", 2);
    assert(WaitForPluginResult(manager, L"demo-after-timeout", L"Demo Result", std::chrono::seconds(3)));

    manager.RequestQuery(L"slow-disable-one", 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    manager.RequestQuery(L"slow-disable-two", 4);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    manager.RequestQuery(L"slow-disable-three", 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    manager.RequestQuery(L"demo-after-disable", 6);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    assert(manager.CachedResultsFor(L"demo-after-disable").empty());

    manager.Shutdown();
    std::filesystem::remove_all(tempRoot, ec);
  }

  assert(RunAndWait(hostPath, badApiPath) == 5);
  return 0;
}
