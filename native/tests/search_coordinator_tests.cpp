#include "search_coordinator.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <atomic>
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
    } else if (request.query.starts_with(L"rapid")) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
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


  {
    std::lock_guard lock(mutex);
    generations.clear();
  }
  for (unsigned long long generation = 3; generation <= 27; ++generation) {
    feathercast::app::QueryRequest rapid;
    rapid.generation = generation;
    rapid.query = L"rapid" + std::to_wstring(generation);
    assert(search.Query(std::move(rapid)));
  }
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return !generations.empty() && generations.back() == 27;
    }));
    assert(generations.size() == 1);
    assert(generations.front() == 27);
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

  std::atomic<int> searchErrors = 0;
  std::uint64_t recoveredGeneration = 0;
  feathercast::search::SearchCoordinator resilientSearch(
      [&](feathercast::app::ResultsCollection result) {
        {
          std::lock_guard lock(mutex);
          recoveredGeneration = result.generation;
        }
        cv.notify_all();
      },
      [&](std::exception_ptr) {
        ++searchErrors;
        cv.notify_all();
      });
  resilientSearch.Start([](const feathercast::app::QueryRequest& request) {
    if (request.query == L"throw") throw std::runtime_error("search failure");
    feathercast::app::ResultsCollection result;
    result.generation = request.generation;
    return result;
  });
  feathercast::app::QueryRequest failingQuery;
  failingQuery.generation = 100;
  failingQuery.query = L"throw";
  assert(resilientSearch.Query(failingQuery));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return searchErrors == 1; }));
  }
  feathercast::app::QueryRequest recoveredQuery;
  recoveredQuery.generation = 101;
  recoveredQuery.query = L"works";
  assert(resilientSearch.Query(recoveredQuery));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return recoveredGeneration == 101; }));
  }
  resilientSearch.Stop();

  std::atomic<int> snapshotErrors = 0;
  std::uint64_t recoveredRevision = 0;
  feathercast::search::SnapshotCoordinator resilientSnapshots(
      [&](feathercast::app::SnapshotBuildResult result) {
        {
          std::lock_guard lock(mutex);
          recoveredRevision = result.revision;
        }
        cv.notify_all();
      },
      [&](std::exception_ptr) {
        ++snapshotErrors;
        cv.notify_all();
      });
  resilientSnapshots.Start([](const feathercast::app::Settings& settings) {
    if (settings.overlayWidth == 999) {
      throw std::runtime_error("snapshot failure");
    }
    return std::make_shared<feathercast::app::SearchSnapshot>();
  });
  feathercast::app::SnapshotBuildRequest failingSnapshot;
  failingSnapshot.revision = 200;
  failingSnapshot.settings.overlayWidth = 999;
  assert(resilientSnapshots.UpdateCorpus(failingSnapshot));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return snapshotErrors == 1; }));
  }
  assert(resilientSnapshots.UpdateCorpus({201, {}}));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return recoveredRevision == 201; }));
  }
  resilientSnapshots.Stop();
  return 0;
}
