#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace feathercast::core {

struct SearchItem {
  std::wstring id;
  std::wstring path;
  std::wstring kind;
  std::wstring source;
  std::wstring name;
  std::wstring processName;
  std::wstring targetPath;
  std::wstring launchTarget;
  std::wstring exe;
  std::vector<std::wstring> keywords;
  bool systemEssential = false;
  bool pinned = false;
  int usageCount = 0;
  long long lastUsed = 0;
};

inline std::wstring Trim(std::wstring value) {
  auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; });
  auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) { return std::iswspace(ch) != 0; }).base();
  if (first >= last) return L"";
  return std::wstring(first, last);
}

inline std::wstring Lower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return value;
}

inline std::wstring LowerInvariant(const std::wstring& value) {
  if (value.empty()) return L"";
  const int needed = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                                   value.data(), static_cast<int>(value.size()),
                                   nullptr, 0, nullptr, nullptr, 0);
  if (needed <= 0) return Lower(value);
  std::wstring out(static_cast<size_t>(needed), L'\0');
  if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                    value.data(), static_cast<int>(value.size()),
                    out.data(), needed, nullptr, nullptr, 0) <= 0) {
    return Lower(value);
  }
  return out;
}

inline std::wstring FoldDiacritics(const std::wstring& value) {
  if (value.empty()) return L"";
  const int needed = NormalizeString(NormalizationD, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (needed <= 0) return value;
  std::wstring decomposed(static_cast<size_t>(needed), L'\0');
  const int written = NormalizeString(NormalizationD, value.data(), static_cast<int>(value.size()),
                                      decomposed.data(), needed);
  if (written <= 0) return value;
  decomposed.resize(static_cast<size_t>(written));

  std::wstring out;
  out.reserve(decomposed.size());
  for (const wchar_t ch : decomposed) {
    WORD type = 0;
    if (GetStringTypeW(CT_CTYPE3, &ch, 1, &type) &&
        (type & (C3_NONSPACING | C3_DIACRITIC | C3_VOWELMARK)) != 0) {
      continue;
    }
    out.push_back(ch);
  }
  return out;
}

inline std::wstring Normalize(const std::wstring& value) {
  return Trim(FoldDiacritics(LowerInvariant(value)));
}

inline std::vector<std::wstring> Tokens(const std::wstring& value) {
  std::vector<std::wstring> out;
  std::wstring current;
  for (wchar_t ch : Normalize(value)) {
    if (std::iswalnum(ch)) {
      current.push_back(ch);
    } else if (!current.empty()) {
      out.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

inline std::wstring Acronym(const std::wstring& value) {
  std::wstring out;
  for (const auto& token : Tokens(value)) {
    if (!token.empty()) out.push_back(token.front());
  }
  return out;
}

inline bool BoundaryBefore(const std::wstring& text, size_t index) {
  if (index == 0) return true;
  const wchar_t ch = text[index - 1];
  if (std::iswspace(ch) || ch == L'.' || ch == L'_' || ch == L'-' || ch == L'\\' || ch == L'/') return true;
  return std::iswlower(ch) && std::iswupper(text[index]);
}

inline int DamerauLevenshteinDistance(const std::wstring& a, const std::wstring& b, int maxDistance) {
  const int m = static_cast<int>(a.size());
  const int n = static_cast<int>(b.size());
  if (std::abs(m - n) > maxDistance) return maxDistance + 1;

  std::vector<int> d(static_cast<size_t>((m + 1) * (n + 1)), 0);
  auto at = [&](int row, int col) -> int& {
    return d[static_cast<size_t>(row * (n + 1) + col)];
  };

  for (int i = 0; i <= m; ++i) at(i, 0) = i;
  for (int j = 0; j <= n; ++j) at(0, j) = j;

  for (int i = 1; i <= m; ++i) {
    int rowBest = maxDistance + 1;
    for (int j = 1; j <= n; ++j) {
      const int cost = a[static_cast<size_t>(i - 1)] == b[static_cast<size_t>(j - 1)] ? 0 : 1;
      int value = std::min({
        at(i - 1, j) + 1,
        at(i, j - 1) + 1,
        at(i - 1, j - 1) + cost,
      });
      if (i > 1 && j > 1 &&
          a[static_cast<size_t>(i - 1)] == b[static_cast<size_t>(j - 2)] &&
          a[static_cast<size_t>(i - 2)] == b[static_cast<size_t>(j - 1)]) {
        value = std::min(value, at(i - 2, j - 2) + 1);
      }
      at(i, j) = value;
      rowBest = std::min(rowBest, value);
    }
    if (rowBest > maxDistance && i > maxDistance + 1) return maxDistance + 1;
  }

  return at(m, n);
}

inline double ScoreText(const std::wstring& query, const std::wstring& target) {
  const std::wstring q = Normalize(query);
  const std::wstring t = Normalize(target);
  // Case-preserved copy (same indices as t) so BoundaryBefore can see camelCase.
  const std::wstring raw = Trim(target);
  if (q.empty() || t.empty()) return -1;
  if (t == q) return 5000;
  if (t.rfind(q, 0) == 0) return 3200 - std::min<int>(static_cast<int>(t.size() - q.size()), 200);

  const std::wstring acronym = Acronym(t);
  if (!acronym.empty()) {
    if (acronym == q) return 3000;
    if (acronym.rfind(q, 0) == 0) return 2300 - std::min<int>(static_cast<int>(acronym.size() - q.size()), 100);
  }

  const size_t substring = t.find(q);
  if (substring != std::wstring::npos) {
    return (BoundaryBefore(raw, substring) ? 2400 : 1800) - static_cast<double>(substring);
  }

  if (q.size() > 4) {
    const int maxDistance = q.size() >= 8 ? 2 : 1;
    int bestDistance = maxDistance + 1;
    int bestLengthDelta = maxDistance + 1;
    for (const auto& token : Tokens(t)) {
      const int lengthDelta = std::abs(static_cast<int>(token.size()) - static_cast<int>(q.size()));
      if (lengthDelta > maxDistance) continue;
      const int distance = DamerauLevenshteinDistance(q, token, maxDistance);
      if (distance < bestDistance || (distance == bestDistance && lengthDelta < bestLengthDelta)) {
        bestDistance = distance;
        bestLengthDelta = lengthDelta;
      }
    }
    if (bestDistance <= maxDistance) {
      return 1550.0 - static_cast<double>(bestDistance * 220 + bestLengthDelta * 15);
    }
  }

  size_t qi = 0;
  double score = 0;
  int previous = -2;
  for (size_t ti = 0; ti < t.size() && qi < q.size(); ++ti) {
    if (t[ti] == q[qi]) {
      score += 16;
      if (static_cast<int>(ti) == previous + 1) score += 12;
      if (BoundaryBefore(raw, ti)) score += 10;
      previous = static_cast<int>(ti);
      ++qi;
    }
  }
  if (qi < q.size()) return -1;
  return score - std::max<int>(0, static_cast<int>(t.size() - q.size()));
}

struct WeightedField {
  std::wstring text;
  double weight = 1.0;
};

inline std::wstring JoinKeywords(const std::vector<std::wstring>& keywords);
inline double UsageBoost(int usageCount, long long lastUsed, long long now);

struct PreparedField {
  std::wstring raw;
  std::wstring normalized;
  std::vector<std::wstring> tokens;
  std::wstring acronym;
  double weight = 1.0;
};

struct PreparedSearchItem {
  SearchItem item;
  std::vector<PreparedField> fields;
  std::wstring normalizedName;
  std::wstring lowerName;
};

struct SearchOptions {
  size_t limit = SIZE_MAX;
  long long now = 0;
  unsigned long long generation = 0;
  const std::atomic<unsigned long long>* latestGeneration = nullptr;
};

inline PreparedField PrepareField(std::wstring text, double weight) {
  PreparedField field;
  field.raw = Trim(text);
  field.normalized = Normalize(text);
  field.tokens = Tokens(text);
  field.acronym = Acronym(text);
  field.weight = weight;
  return field;
}

inline PreparedSearchItem PrepareSearchItem(const SearchItem& item) {
  PreparedSearchItem prepared;
  prepared.item = item;
  prepared.normalizedName = Normalize(item.name);
  prepared.lowerName = Lower(item.name);
  prepared.fields = {
    PrepareField(item.name, 1.0),
    PrepareField(JoinKeywords(item.keywords), 0.82),
    PrepareField(item.processName, 0.7),
    PrepareField(!item.targetPath.empty() ? item.targetPath :
                 (!item.launchTarget.empty() ? item.launchTarget : item.exe), 0.45),
  };
  return prepared;
}

inline double ScorePreparedText(const std::wstring& normalizedQuery, const PreparedField& target) {
  const std::wstring& q = normalizedQuery;
  const std::wstring& t = target.normalized;
  const std::wstring& raw = target.raw;
  if (q.empty() || t.empty()) return -1;
  if (t == q) return 5000;
  if (t.rfind(q, 0) == 0) return 3200 - std::min<int>(static_cast<int>(t.size() - q.size()), 200);

  if (!target.acronym.empty()) {
    if (target.acronym == q) return 3000;
    if (target.acronym.rfind(q, 0) == 0) {
      return 2300 - std::min<int>(static_cast<int>(target.acronym.size() - q.size()), 100);
    }
  }

  const size_t substring = t.find(q);
  if (substring != std::wstring::npos) {
    return (BoundaryBefore(raw, substring) ? 2400 : 1800) - static_cast<double>(substring);
  }

  if (q.size() > 4) {
    const int maxDistance = q.size() >= 8 ? 2 : 1;
    int bestDistance = maxDistance + 1;
    int bestLengthDelta = maxDistance + 1;
    for (const auto& token : target.tokens) {
      const int lengthDelta = std::abs(static_cast<int>(token.size()) - static_cast<int>(q.size()));
      if (lengthDelta > maxDistance) continue;
      const int distance = DamerauLevenshteinDistance(q, token, maxDistance);
      if (distance < bestDistance || (distance == bestDistance && lengthDelta < bestLengthDelta)) {
        bestDistance = distance;
        bestLengthDelta = lengthDelta;
      }
    }
    if (bestDistance <= maxDistance) {
      return 1550.0 - static_cast<double>(bestDistance * 220 + bestLengthDelta * 15);
    }
  }

  size_t qi = 0;
  double score = 0;
  int previous = -2;
  for (size_t ti = 0; ti < t.size() && qi < q.size(); ++ti) {
    if (t[ti] == q[qi]) {
      score += 16;
      if (static_cast<int>(ti) == previous + 1) score += 12;
      if (BoundaryBefore(raw, ti)) score += 10;
      previous = static_cast<int>(ti);
      ++qi;
    }
  }
  if (qi < q.size()) return -1;
  return score - std::max<int>(0, static_cast<int>(t.size() - q.size()));
}

inline double ScorePreparedItem(const std::wstring& normalizedQuery,
                                const std::vector<std::wstring>& queryTokens,
                                const PreparedSearchItem& prepared,
                                const std::set<std::wstring>& recentIds,
                                long long now = 0) {
  if (queryTokens.empty()) return 0;
  double total = 0;
  for (const auto& token : queryTokens) {
    double best = -1;
    for (const auto& field : prepared.fields) {
      const double score = ScorePreparedText(token, field);
      if (score >= 0) best = std::max(best, score * field.weight);
    }
    if (best < 0) return -1;
    total += best;
  }

  if (prepared.normalizedName == normalizedQuery) total += 2500;
  else if (prepared.normalizedName.rfind(normalizedQuery, 0) == 0) total += 600;

  const auto& item = prepared.item;
  if (recentIds.contains(item.id) || recentIds.contains(item.path)) total += 260;
  if (item.pinned) total += 1000;
  total += UsageBoost(item.usageCount, item.lastUsed, now);
  if (item.kind == L"window") total += 120;
  if (item.source == L"alias") total += 90;
  if (item.systemEssential) total += 70;
  return total;
}

inline std::vector<size_t> SearchPrepared(const std::wstring& query,
                                          const std::vector<PreparedSearchItem>& items,
                                          const std::set<std::wstring>& recentIds = {},
                                          SearchOptions options = {}) {
  if (Trim(query).empty()) {
    const size_t count = std::min(items.size(), options.limit);
    std::vector<size_t> all;
    all.reserve(count);
    for (size_t i = 0; i < count; ++i) all.push_back(i);
    return all;
  }

  struct Scored {
    size_t index = 0;
    double score = 0;
    const std::wstring* lowerName = nullptr;
  };

  const std::wstring normalizedQuery = Normalize(query);
  const std::vector<std::wstring> queryTokens = Tokens(query);
  const unsigned hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
  const size_t workerCount = items.size() >= 20000
      ? std::min<size_t>(4, hardwareThreads)
      : 1;
  std::vector<std::vector<Scored>> buckets(workerCount);
  auto scoreRange = [&](size_t worker, size_t begin, size_t end) {
    auto& bucket = buckets[worker];
    bucket.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
      if ((i & 63u) == 0 && options.latestGeneration &&
          options.latestGeneration->load(std::memory_order_acquire) != options.generation) {
        bucket.clear();
        return;
      }
      const double score = ScorePreparedItem(normalizedQuery, queryTokens, items[i], recentIds, options.now);
      if (score >= 0) bucket.push_back({i, score, &items[i].lowerName});
    }
  };

  if (workerCount == 1) {
    scoreRange(0, 0, items.size());
  } else {
    std::vector<std::jthread> workers;
    workers.reserve(workerCount);
    const size_t chunk = (items.size() + workerCount - 1) / workerCount;
    for (size_t worker = 0; worker < workerCount; ++worker) {
      const size_t begin = worker * chunk;
      const size_t end = std::min(items.size(), begin + chunk);
      workers.emplace_back([&, worker, begin, end] { scoreRange(worker, begin, end); });
    }
    for (auto& worker : workers) worker.join();
  }
  if (options.latestGeneration &&
      options.latestGeneration->load(std::memory_order_acquire) != options.generation) {
    return {};
  }

  size_t totalMatches = 0;
  for (const auto& bucket : buckets) totalMatches += bucket.size();
  std::vector<Scored> scored;
  scored.reserve(totalMatches);
  for (auto& bucket : buckets) {
    scored.insert(scored.end(),
                  std::make_move_iterator(bucket.begin()),
                  std::make_move_iterator(bucket.end()));
  }

  auto better = [](const Scored& a, const Scored& b) {
    if (a.score != b.score) return a.score > b.score;
    return *a.lowerName < *b.lowerName;
  };
  if (options.limit < scored.size()) {
    std::nth_element(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(options.limit),
                     scored.end(), better);
    scored.resize(options.limit);
  }
  std::sort(scored.begin(), scored.end(), better);

  std::vector<size_t> out;
  out.reserve(scored.size());
  for (const auto& item : scored) out.push_back(item.index);
  return out;
}

inline double ScoreTokenAgainstFields(const std::wstring& token, const std::vector<WeightedField>& fields) {
  double best = -1;
  for (const auto& field : fields) {
    const double score = ScoreText(token, field.text);
    if (score >= 0) best = std::max(best, score * field.weight);
  }
  return best;
}

inline std::wstring JoinKeywords(const std::vector<std::wstring>& keywords) {
  std::wstring out;
  for (const auto& keyword : keywords) {
    if (!out.empty()) out.push_back(L' ');
    out += keyword;
  }
  return out;
}

// Log-scaled frequency + exponentially decayed recency (half-life 7 days).
// Ceiling ~2470 (500 launches, used just now) — enough to lift an actively used
// boundary-substring match (~2400) past a cold name-prefix match (~3200), while
// exact-name matches (5000 + 2500) stay out of reach. now == 0 means "no clock":
// any lastUsed > 0 then counts as fully fresh.
inline double UsageBoost(int usageCount, long long lastUsed, long long now) {
  double boost = 220.0 * std::log2(1.0 + std::min(usageCount, 500));
  if (lastUsed > 0) {
    const double days = now > lastUsed ? static_cast<double>(now - lastUsed) / 86400.0 : 0.0;
    boost += 500.0 * std::exp2(-days / 7.0);
  }
  return boost;
}

// Scores an item against an already-normalized query and its tokens. Hoisting the
// query normalization/tokenization to the caller avoids recomputing it for every
// item on every keystroke; the scoring is otherwise identical to the public overload.
inline double ScoreItem(const std::wstring& normalizedQuery, const std::vector<std::wstring>& queryTokens,
                        const SearchItem& item, const std::set<std::wstring>& recentIds, long long now = 0) {
  if (queryTokens.empty()) return 0;

  const std::vector<WeightedField> fields = {
    {item.name, 1.0},
    {JoinKeywords(item.keywords), 0.82},
    {item.processName, 0.7},
    {!item.targetPath.empty() ? item.targetPath : (!item.launchTarget.empty() ? item.launchTarget : item.exe), 0.45},
  };

  double total = 0;
  for (const auto& token : queryTokens) {
    const double best = ScoreTokenAgainstFields(token, fields);
    if (best < 0) return -1;
    total += best;
  }

  const std::wstring name = Normalize(item.name);
  if (name == normalizedQuery) total += 2500;
  else if (name.rfind(normalizedQuery, 0) == 0) total += 600;

  if (recentIds.contains(item.id) || recentIds.contains(item.path)) total += 260;
  if (item.pinned) total += 1000;
  total += UsageBoost(item.usageCount, item.lastUsed, now);
  if (item.kind == L"window") total += 120;
  if (item.source == L"alias") total += 90;
  if (item.systemEssential) total += 70;
  return total;
}

inline double ScoreItem(const std::wstring& query, const SearchItem& item, const std::set<std::wstring>& recentIds, long long now = 0) {
  return ScoreItem(Normalize(query), Tokens(query), item, recentIds, now);
}

inline std::vector<size_t> Search(const std::wstring& query, const std::vector<SearchItem>& items, const std::set<std::wstring>& recentIds = {}, long long now = 0) {
  if (Trim(query).empty()) {
    std::vector<size_t> all;
    all.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) all.push_back(i);
    return all;
  }

  struct Scored {
    size_t index = 0;
    double score = 0;
    std::wstring lowerName;
  };

  // Normalize/tokenize the query once instead of per item.
  const std::wstring normalizedQuery = Normalize(query);
  const std::vector<std::wstring> queryTokens = Tokens(query);

  std::vector<Scored> scored;
  scored.reserve(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    const double score = ScoreItem(normalizedQuery, queryTokens, items[i], recentIds, now);
    if (score >= 0) scored.push_back({i, score, Lower(items[i].name)});
  }

  std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.lowerName < b.lowerName;
  });

  std::vector<size_t> out;
  out.reserve(scored.size());
  for (const auto& item : scored) out.push_back(item.index);
  return out;
}

}  // namespace feathercast::core
