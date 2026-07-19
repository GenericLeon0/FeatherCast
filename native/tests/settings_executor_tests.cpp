#include "background_executor.hpp"
#include "settings.hpp"
#include "settings_io.hpp"
#include "test_framework.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <stdexcept>
#include <string>

int main() {
  namespace settings = feathercast::settings;

  {
    const auto missing = settings::ParseSettingsDocument("");
    assert(missing.status == settings::ParseStatus::Missing);
    assert(missing.value.shortcut == L"Alt+Space");

    const auto valid =
        settings::ParseSettingsDocument(R"({"shortcut":"Ctrl+Space","maxResults":42})");
    assert(valid.status == settings::ParseStatus::Valid);
    assert(valid.value.shortcut == L"Ctrl+Space");
    assert(valid.value.maxResults == 42);
    assert(valid.documentVersion == 0);

    const auto versioned = settings::ParseSettingsDocument(
        R"({"schemaVersion":1,"shortcut":"Ctrl+Alt+Space"})");
    assert(versioned.status == settings::ParseStatus::Valid);
    assert(versioned.documentVersion == 1);
    assert(versioned.value.shortcut == L"Ctrl+Alt+Space");

    const auto current = settings::ParseSettingsDocument(
        R"({"schemaVersion":2,"fileContentIndexEnabled":true})");
    assert(current.status == settings::ParseStatus::Valid);
    assert(current.value.fileContentIndexEnabled);

    const auto future = settings::ParseSettingsDocument(
        R"({"schemaVersion":3,"shortcut":"DoNotLoad"})");
    assert(future.status == settings::ParseStatus::UnsupportedVersion);
    assert(future.documentVersion == 3);
    assert(future.value.shortcut == L"Alt+Space");

    assert(settings::ParseSettingsDocument(R"({"schemaVersion":1.5})").status ==
           settings::ParseStatus::Invalid);

    const auto malformed = settings::ParseSettingsDocument(R"({"shortcut":)");
    assert(malformed.status == settings::ParseStatus::Invalid);
    assert(malformed.value.shortcut == L"Alt+Space");

    const auto wrongRoot = settings::ParseSettingsDocument("[]");
    assert(wrongRoot.status == settings::ParseStatus::Invalid);

    std::string invalidUtf8 = R"({"shortcut":")";
    invalidUtf8.push_back(static_cast<char>(0xC3));
    invalidUtf8 += R"("})";
    assert(settings::ParseSettingsDocument(invalidUtf8).status ==
           settings::ParseStatus::Invalid);

    assert(settings::ParseSettings(R"({"shortcut":)").shortcut == L"Alt+Space");
    const auto serialized = settings::SerializeSettings(settings::Settings{});
    assert(serialized.find("\"schemaVersion\": 2") != std::string::npos);
  }

  {
    wchar_t tempPath[MAX_PATH]{};
    assert(GetTempPathW(MAX_PATH, tempPath) > 0);
    const auto root =
        std::filesystem::path(tempPath) /
        (L"FeatherCastSettingsRecovery-" +
         std::to_wstring(GetCurrentProcessId()));
    const auto path = root / L"settings.json";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);

    const std::string original = R"({"shortcut":)";
    {
      std::ofstream file(path, std::ios::binary);
      file.write(original.data(), static_cast<std::streamsize>(original.size()));
    }

    const auto recovered = feathercast::settings_io::LoadSettingsFile(path);
    assert(recovered.status == settings::ParseStatus::Invalid);
    assert(recovered.persistenceAllowed);
    assert(!recovered.preservedPath.empty());
    assert(std::filesystem::exists(recovered.preservedPath));
    assert(!recovered.value.clipboardHistoryEnabled);
    assert(!recovered.value.fileIndexEnabled);

    {
      std::ifstream backup(recovered.preservedPath, std::ios::binary);
      std::ostringstream bytes;
      bytes << backup.rdbuf();
      assert(bytes.str() == original);
    }

    const auto emptyPath = root / L"empty-settings.json";
    {
      std::ofstream empty(emptyPath, std::ios::binary);
    }
    const auto emptyRecovered =
        feathercast::settings_io::LoadSettingsFile(emptyPath);
    assert(emptyRecovered.status == settings::ParseStatus::Invalid);
    assert(emptyRecovered.persistenceAllowed);
    assert(std::filesystem::exists(emptyRecovered.preservedPath));

    const auto futurePath = root / L"future-settings.json";
    {
      std::ofstream futureFile(futurePath, std::ios::binary);
      futureFile << R"({"schemaVersion":99,"shortcut":"Ctrl+Q"})";
    }
    const auto futureLoaded =
        feathercast::settings_io::LoadSettingsFile(futurePath);
    assert(futureLoaded.status == settings::ParseStatus::UnsupportedVersion);
    assert(!futureLoaded.persistenceAllowed);
    assert(futureLoaded.preservedPath.empty());
    assert(std::filesystem::exists(futurePath));

    std::filesystem::remove_all(root, ec);
    assert(!ec);
  }

  {
    feathercast::background::Executor executor;
    std::atomic<int> errorCount = 0;
    executor.Start(1, [&](std::exception_ptr failure) {
      ++errorCount;
      assert(failure);
      throw std::runtime_error("error handler failure");
    });

    std::promise<void> continued;
    auto continuedFuture = continued.get_future();
    assert(executor.Submit([](std::stop_token) {
      throw std::runtime_error("task failure");
    }));
    assert(executor.Submit([&](std::stop_token) {
      continued.set_value();
    }));

    assert(continuedFuture.wait_for(std::chrono::seconds(2)) ==
           std::future_status::ready);
    executor.Shutdown(true);
    assert(errorCount.load() == 1);
  }

  return 0;
}
