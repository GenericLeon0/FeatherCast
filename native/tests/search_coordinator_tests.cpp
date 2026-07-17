#include "search_coordinator.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

int main() {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<unsigned long long> generations;

  feathercast::search::SearchCoordinator search(
      [&](feathercast::app::ResultsCollection result) {
        {
          std::lock_guard lock(mutex);
          generations.push_back(result.generation);
        }
        cv.notify_all();
      });
  search.Start([](const feathercast::app::QueryRequest& request) {
    if (request.query == L"slow") {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    feathercast::app::ResultsCollection result;
    result.generation = request.generation;
    return result;
  });

  feathercast::app::QueryRequest slow;
  slow.generation = 1;
  slow.query = L"slow";
  assert(search.Query(slow));
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  feathercast::app::QueryRequest newest;
  newest.generation = 2;
  newest.query = L"new";
  assert(search.Query(newest));

  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return !generations.empty(); }));
    assert(generations.size() == 1);
    assert(generations.front() == 2);
  }
  search.Stop();
  assert(!search.Query(newest));

  std::uint64_t publishedRevision = 0;
  feathercast::search::SnapshotCoordinator snapshots(
      [&](feathercast::app::SnapshotBuildResult result) {
        {
          std::lock_guard lock(mutex);
          publishedRevision = result.revision;
        }
        cv.notify_all();
      });
  snapshots.Start([](const feathercast::app::Settings&) {
    return std::make_shared<feathercast::app::SearchSnapshot>();
  });
  assert(snapshots.UpdateCorpus({7, {}}));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return publishedRevision == 7; }));
  }
  snapshots.Stop();
  return 0;
}
