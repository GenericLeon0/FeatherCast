#pragma once

#include "core.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace feathercast::search {

struct SearchRequest {
  std::wstring query;
  size_t limit = SIZE_MAX;
  long long now = 0;
  unsigned long long generation = 0;
  const std::atomic<unsigned long long>* cancellation = nullptr;
  std::set<std::wstring> recentIds;
};

class SearchIndex {
 public:
  struct PublishedCorpus {
    std::vector<core::PreparedSearchItem> items;
    uint64_t revision = 0;
  };

  void Publish(std::vector<core::PreparedSearchItem> corpus, uint64_t revision) {
    auto published = std::make_shared<PublishedCorpus>();
    published->items = std::move(corpus);
    published->revision = revision;
    std::lock_guard lock(mutex_);
    corpus_ = std::move(published);
  }

  std::shared_ptr<const PublishedCorpus> Snapshot() const {
    std::lock_guard lock(mutex_);
    return corpus_;
  }

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<const PublishedCorpus> corpus_ = std::make_shared<PublishedCorpus>();
};

class SearchEngine {
 public:
  explicit SearchEngine(const SearchIndex& index) : index_(index) {}

  std::vector<size_t> Query(const SearchRequest& request) const {
    const auto corpus = index_.Snapshot();
    core::SearchOptions options;
    options.limit = request.limit;
    options.now = request.now;
    options.generation = request.generation;
    options.latestGeneration = request.cancellation;
    return core::SearchPrepared(request.query, corpus->items, request.recentIds, options);
  }

 private:
  const SearchIndex& index_;
};

}  // namespace feathercast::search
