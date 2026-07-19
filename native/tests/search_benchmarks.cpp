#include "core.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double RunCorpus(size_t count, int iterations,
                 const std::vector<std::wstring>& queries) {
  std::vector<feathercast::core::PreparedSearchItem> items;
  items.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    feathercast::core::SearchItem item;
    item.id = L"app:" + std::to_wstring(i);
    item.name = L"Application " + std::to_wstring(i) + (i % 7 == 0 ? L" Visual Studio Code" : L"");
    item.processName = L"process" + std::to_wstring(i % 113);
    item.targetPath = L"C:\\Programs\\Application" + std::to_wstring(i) + L"\\app.exe";
    item.keywords = {L"application", L"tool", i % 7 == 0 ? L"code editor" : L"utility"};
    item.usageCount = static_cast<int>(i % 100);
    items.push_back(feathercast::core::PrepareSearchItem(item));
  }

  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(iterations));
  for (int i = 0; i < iterations; ++i) {
    feathercast::core::SearchOptions options;
    options.limit = 100;
    options.now = 1750000000;
    const auto start = Clock::now();
    const auto results = feathercast::core::SearchPrepared(
        queries[static_cast<size_t>(i) % queries.size()], items, {}, options);
    const auto end = Clock::now();
    if (results.empty()) return -1.0;
    samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
  }
  std::sort(samples.begin(), samples.end());
  const size_t p95Index = std::min(samples.size() - 1, (samples.size() * 95) / 100);
  return samples[p95Index];
}

}  // namespace

int main() {
  const std::vector<std::wstring> normalQueries = {
      L"visual code", L"application 42"};
  const std::vector<std::wstring> typoQueries = {
      L"aplication", L"visual studoi"};
  const double p95At5k = RunCorpus(5000, 30, normalQueries);
  const double p95At50k = RunCorpus(50000, 20, normalQueries);
  const double typoP95At5k = RunCorpus(5000, 30, typoQueries);
  const double typoP95At50k = RunCorpus(50000, 20, typoQueries);
  std::cout << "search_p95_5k_ms=" << p95At5k << "\n";
  std::cout << "search_p95_50k_ms=" << p95At50k << "\n";
  std::cout << "search_typo_p95_5k_ms=" << typoP95At5k << "\n";
  std::cout << "search_typo_p95_50k_ms=" << typoP95At50k << "\n";
  if (p95At5k < 0 || p95At50k < 0 || typoP95At5k < 0 ||
      typoP95At50k < 0) {
    return 1;
  }
#ifdef NDEBUG
  if (p95At5k > 10.0 || p95At50k > 50.0 || typoP95At5k > 10.0 ||
      typoP95At50k > 50.0) {
    return 2;
  }
#endif
  return 0;
}
