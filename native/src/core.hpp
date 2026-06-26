#pragma once

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <set>
#include <string>
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

inline std::wstring Normalize(const std::wstring& value) {
  return Trim(Lower(value));
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
  return std::iswspace(ch) || ch == L'.' || ch == L'_' || ch == L'-' || ch == L'\\' || ch == L'/';
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
    return (BoundaryBefore(t, substring) ? 2400 : 1800) - static_cast<double>(substring);
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
      if (BoundaryBefore(t, ti)) score += 10;
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

// Scores an item against an already-normalized query and its tokens. Hoisting the
// query normalization/tokenization to the caller avoids recomputing it for every
// item on every keystroke; the scoring is otherwise identical to the public overload.
inline double ScoreItem(const std::wstring& normalizedQuery, const std::vector<std::wstring>& queryTokens,
                        const SearchItem& item, const std::set<std::wstring>& recentIds) {
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
  total += std::min(item.usageCount, 20) * 35;
  if (item.lastUsed > 0) total += 150;
  if (item.kind == L"window") total += 120;
  if (item.source == L"alias") total += 90;
  if (item.systemEssential) total += 70;
  return total;
}

inline double ScoreItem(const std::wstring& query, const SearchItem& item, const std::set<std::wstring>& recentIds) {
  return ScoreItem(Normalize(query), Tokens(query), item, recentIds);
}

inline std::vector<size_t> Search(const std::wstring& query, const std::vector<SearchItem>& items, const std::set<std::wstring>& recentIds = {}) {
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
    const double score = ScoreItem(normalizedQuery, queryTokens, items[i], recentIds);
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
