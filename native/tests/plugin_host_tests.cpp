#include <windows.h>

#include <algorithm>
#include <cassert>
#include <chrono>
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
    assert(CreatePipe(&childStdinRead, &parentStdinWrite, &inheritable, 0));
    assert(CreatePipe(&parentStdoutRead, &childStdoutWrite, &inheritable, 0));
    SetHandleInformation(parentStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentStdoutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childStdinRead;
    startup.hStdOutput = childStdoutWrite;
    startup.hStdError = childStdoutWrite;

    std::wstring command = Quote(hostPath) + L" " + Quote(dllPath);
    assert(CreateProcessW(hostPath.c_str(), command.data(), nullptr, nullptr, TRUE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process_));

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
    assert(WriteFile(stdinWrite_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr));
    assert(written == line.size());

    std::string buffer;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      DWORD available = 0;
      assert(PeekNamedPipe(stdoutRead_, nullptr, 0, nullptr, &available, nullptr));
      if (available > 0) {
        char chunk[4096]{};
        DWORD read = 0;
        assert(ReadFile(stdoutRead_, chunk, std::min<DWORD>(available, sizeof(chunk)), &read, nullptr));
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
  assert(CreateProcessW(hostPath.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
  CloseHandleIfSet(process.hThread);
  WaitForSingleObject(process.hProcess, 5000);
  DWORD exitCode = 0;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandleIfSet(process.hProcess);
  return exitCode;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  assert(argc == 4);
  const std::wstring hostPath = argv[1];
  const std::wstring pluginPath = argv[2];
  const std::wstring badApiPath = argv[3];

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
  }

  {
    HostSession session(hostPath, pluginPath);
    const auto slow = session.Send("{\"apiVersion\":1,\"type\":\"query\",\"query\":\"slow\",\"limit\":20}",
                                   std::chrono::milliseconds(100));
    assert(slow.empty());
  }

  assert(RunAndWait(hostPath, badApiPath) == 5);
  return 0;
}
