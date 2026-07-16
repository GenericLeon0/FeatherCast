#pragma once

#include "extension_protocol.hpp"

#include <windows.h>
#include <softpub.h>
#include <wintrust.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace feathercast::updater {

struct Version {
  std::array<int, 3> parts{0, 0, 0};
};

struct ReleaseAsset {
  std::wstring name;
  std::wstring browserDownloadUrl;
};

struct ReleaseInfo {
  std::wstring tagName;
  std::wstring name;
  std::wstring htmlUrl;
  bool draft = false;
  bool prerelease = false;
  std::vector<ReleaseAsset> assets;
};

inline std::wstring TrimWide(std::wstring value) {
  auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; });
  auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) { return std::iswspace(ch) != 0; }).base();
  if (first >= last) return L"";
  return std::wstring(first, last);
}

inline std::wstring LowerWide(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return value;
}

inline std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

inline bool EndsWithInsensitive(const std::wstring& value, const std::wstring& suffix) {
  const std::wstring lowerValue = LowerWide(value);
  const std::wstring lowerSuffix = LowerWide(suffix);
  return lowerValue.size() >= lowerSuffix.size() &&
         lowerValue.compare(lowerValue.size() - lowerSuffix.size(), lowerSuffix.size(), lowerSuffix) == 0;
}

inline std::optional<Version> ParseVersion(std::wstring value) {
  value = TrimWide(std::move(value));
  if (!value.empty() && (value.front() == L'v' || value.front() == L'V')) value.erase(value.begin());
  if (value.empty()) return std::nullopt;

  Version version;
  size_t part = 0;
  size_t start = 0;
  while (start <= value.size() && part < version.parts.size()) {
    const size_t dot = value.find(L'.', start);
    const size_t end = dot == std::wstring::npos ? value.size() : dot;
    if (end == start) return std::nullopt;

    int parsed = 0;
    for (size_t i = start; i < end; ++i) {
      if (!std::iswdigit(value[i])) return std::nullopt;
      parsed = parsed * 10 + static_cast<int>(value[i] - L'0');
    }
    version.parts[part++] = parsed;

    if (dot == std::wstring::npos) {
      start = value.size();
      break;
    }
    start = dot + 1;
  }

  if (part == 0) return std::nullopt;
  if (start < value.size()) return std::nullopt;
  return version;
}

inline int CompareVersions(const Version& left, const Version& right) {
  for (size_t i = 0; i < left.parts.size(); ++i) {
    if (left.parts[i] < right.parts[i]) return -1;
    if (left.parts[i] > right.parts[i]) return 1;
  }
  return 0;
}

inline int CompareVersionStrings(const std::wstring& left, const std::wstring& right) {
  const auto parsedLeft = ParseVersion(left);
  const auto parsedRight = ParseVersion(right);
  if (!parsedLeft || !parsedRight) return 0;
  return CompareVersions(*parsedLeft, *parsedRight);
}

inline bool IsNewerVersion(const std::wstring& currentVersion, const std::wstring& candidateTag) {
  const auto current = ParseVersion(currentVersion);
  const auto candidate = ParseVersion(candidateTag);
  return current && candidate && CompareVersions(*candidate, *current) > 0;
}

inline std::wstring AssetVersionFromTag(std::wstring tagName) {
  tagName = TrimWide(std::move(tagName));
  if (!tagName.empty() && (tagName.front() == L'v' || tagName.front() == L'V')) tagName.erase(tagName.begin());
  return tagName;
}

inline std::optional<ReleaseInfo> ParseGitHubReleaseJson(const std::string& json) {
  auto tagName = feathercast::extensions::JsonString(json, "tag_name");
  if (!tagName || tagName->empty()) return std::nullopt;

  ReleaseInfo release;
  release.tagName = feathercast::extensions::Utf8ToWide(*tagName);
  release.draft = feathercast::extensions::JsonBool(json, "draft", false);
  release.prerelease = feathercast::extensions::JsonBool(json, "prerelease", false);
  if (auto name = feathercast::extensions::JsonString(json, "name")) release.name = feathercast::extensions::Utf8ToWide(*name);
  if (auto html = feathercast::extensions::JsonString(json, "html_url")) release.htmlUrl = feathercast::extensions::Utf8ToWide(*html);

  for (const auto& object : feathercast::extensions::JsonObjectArray(json, "assets")) {
    auto name = feathercast::extensions::JsonString(object, "name");
    auto url = feathercast::extensions::JsonString(object, "browser_download_url");
    if (!name || !url || name->empty() || url->empty()) continue;
    release.assets.push_back({feathercast::extensions::Utf8ToWide(*name),
                              feathercast::extensions::Utf8ToWide(*url)});
  }
  return release;
}

inline bool IsEligibleRelease(const ReleaseInfo& release, const std::wstring& currentVersion) {
  return !release.draft && !release.prerelease && IsNewerVersion(currentVersion, release.tagName);
}

inline std::wstring ExpectedInstallerAssetName(const std::wstring& tagName) {
  return L"FeatherCast-" + AssetVersionFromTag(tagName) + L"-win64.exe";
}

inline std::optional<ReleaseAsset> SelectInstallerAsset(const ReleaseInfo& release) {
  const std::wstring expected = LowerWide(ExpectedInstallerAssetName(release.tagName));
  const std::wstring version = LowerWide(AssetVersionFromTag(release.tagName));

  for (const auto& asset : release.assets) {
    if (LowerWide(asset.name) == expected) return asset;
  }
  for (const auto& asset : release.assets) {
    const std::wstring name = LowerWide(asset.name);
    if (EndsWithInsensitive(asset.name, L".exe") &&
        name.find(L"feathercast") != std::wstring::npos &&
        name.find(version) != std::wstring::npos &&
        name.find(L"win64") != std::wstring::npos) {
      return asset;
    }
  }
  return std::nullopt;
}

inline std::optional<ReleaseAsset> SelectSha256Asset(const ReleaseInfo& release, const ReleaseAsset& installer) {
  const std::wstring exact = LowerWide(installer.name + L".sha256");
  for (const auto& asset : release.assets) {
    if (LowerWide(asset.name) == exact) return asset;
  }

  const std::wstring installerName = LowerWide(installer.name);
  for (const auto& asset : release.assets) {
    const std::wstring name = LowerWide(asset.name);
    if (EndsWithInsensitive(asset.name, L".sha256") && name.find(installerName) != std::wstring::npos) {
      return asset;
    }
  }
  return std::nullopt;
}

inline bool IsHexChar(char ch) {
  return (ch >= '0' && ch <= '9') ||
         (ch >= 'a' && ch <= 'f') ||
         (ch >= 'A' && ch <= 'F');
}

inline std::optional<std::string> ExtractSha256Hex(std::string_view text) {
  for (size_t i = 0; i + 64 <= text.size(); ++i) {
    bool allHex = true;
    for (size_t j = 0; j < 64; ++j) {
      if (!IsHexChar(text[i + j])) {
        allHex = false;
        break;
      }
    }
    if (allHex) return LowerAscii(std::string(text.substr(i, 64)));
  }
  return std::nullopt;
}

inline std::optional<std::string> Sha256FileHex(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;

  HCRYPTPROV provider = 0;
  if (!CryptAcquireContextW(&provider, nullptr, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
      !CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
    return std::nullopt;
  }

  HCRYPTHASH hash = 0;
  if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
    CryptReleaseContext(provider, 0);
    return std::nullopt;
  }

  std::array<char, 64 * 1024> buffer{};
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize read = file.gcount();
    if (read > 0 && !CryptHashData(hash, reinterpret_cast<const BYTE*>(buffer.data()), static_cast<DWORD>(read), 0)) {
      CryptDestroyHash(hash);
      CryptReleaseContext(provider, 0);
      return std::nullopt;
    }
  }

  BYTE bytes[32]{};
  DWORD size = static_cast<DWORD>(sizeof(bytes));
  if (!CryptGetHashParam(hash, HP_HASHVAL, bytes, &size, 0) || size != sizeof(bytes)) {
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    return std::nullopt;
  }

  CryptDestroyHash(hash);
  CryptReleaseContext(provider, 0);

  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(sizeof(bytes) * 2);
  for (const BYTE byte : bytes) {
    out.push_back(kHex[(byte >> 4) & 0x0F]);
    out.push_back(kHex[byte & 0x0F]);
  }
  return out;
}

inline bool VerifyFileSha256(const std::filesystem::path& path, std::string_view expectedText) {
  const auto expected = ExtractSha256Hex(expectedText);
  const auto actual = Sha256FileHex(path);
  return expected && actual && *expected == *actual;
}

inline std::optional<std::wstring> AuthenticodePublisher(const std::filesystem::path& path) {
  HCERTSTORE store = nullptr;
  HCRYPTMSG message = nullptr;
  DWORD encoding = 0;
  DWORD contentType = 0;
  DWORD formatType = 0;
  if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
                        CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                        CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &contentType,
                        &formatType, &store, &message, nullptr)) {
    return std::nullopt;
  }
  auto close = [&] {
    if (message) CryptMsgClose(message);
    if (store) CertCloseStore(store, 0);
  };

  DWORD signerSize = 0;
  if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerSize) || signerSize == 0) {
    close();
    return std::nullopt;
  }
  std::vector<BYTE> signerBuffer(signerSize);
  if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, signerBuffer.data(), &signerSize)) {
    close();
    return std::nullopt;
  }
  const auto* signer = reinterpret_cast<const CMSG_SIGNER_INFO*>(signerBuffer.data());
  CERT_INFO certificateInfo{};
  certificateInfo.Issuer = signer->Issuer;
  certificateInfo.SerialNumber = signer->SerialNumber;
  PCCERT_CONTEXT certificate = CertFindCertificateInStore(
      store, encoding, 0, CERT_FIND_SUBJECT_CERT, &certificateInfo, nullptr);
  if (!certificate) {
    close();
    return std::nullopt;
  }

  const DWORD chars = CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
  std::wstring publisher;
  if (chars > 1) {
    publisher.resize(chars);
    CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                       publisher.data(), chars);
    if (!publisher.empty() && publisher.back() == L'\0') publisher.pop_back();
  }
  CertFreeCertificateContext(certificate);
  close();
  if (publisher.empty()) return std::nullopt;
  return publisher;
}

inline std::optional<std::wstring> AuthenticodeSignerSha256(
    const std::filesystem::path& path) {
  HCERTSTORE store = nullptr;
  HCRYPTMSG message = nullptr;
  DWORD encoding = 0;
  DWORD contentType = 0;
  DWORD formatType = 0;
  if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
                        CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                        CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &contentType,
                        &formatType, &store, &message, nullptr)) {
    return std::nullopt;
  }
  auto close = [&] {
    if (message) CryptMsgClose(message);
    if (store) CertCloseStore(store, 0);
  };

  DWORD signerSize = 0;
  if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerSize) ||
      signerSize == 0) {
    close();
    return std::nullopt;
  }
  std::vector<BYTE> signerBuffer(signerSize);
  if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, signerBuffer.data(),
                        &signerSize)) {
    close();
    return std::nullopt;
  }
  const auto* signer =
      reinterpret_cast<const CMSG_SIGNER_INFO*>(signerBuffer.data());
  CERT_INFO certificateInfo{};
  certificateInfo.Issuer = signer->Issuer;
  certificateInfo.SerialNumber = signer->SerialNumber;
  PCCERT_CONTEXT certificate = CertFindCertificateInStore(
      store, encoding, 0, CERT_FIND_SUBJECT_CERT, &certificateInfo, nullptr);
  if (!certificate) {
    close();
    return std::nullopt;
  }

  BYTE hash[32]{};
  DWORD hashSize = static_cast<DWORD>(sizeof(hash));
  const bool hashed =
      CryptHashCertificate(0, CALG_SHA_256, 0, certificate->pbCertEncoded,
                           certificate->cbCertEncoded, hash, &hashSize) != FALSE;
  CertFreeCertificateContext(certificate);
  close();
  if (!hashed || hashSize != sizeof(hash)) return std::nullopt;

  static constexpr wchar_t kHex[] = L"0123456789abcdef";
  std::wstring out;
  out.reserve(hashSize * 2);
  for (DWORD i = 0; i < hashSize; ++i) {
    out.push_back(kHex[(hash[i] >> 4) & 0x0F]);
    out.push_back(kHex[hash[i] & 0x0F]);
  }
  return out;
}

inline std::vector<std::wstring> ParseSignerThumbprints(
    const std::wstring& configured) {
  std::vector<std::wstring> out;
  std::wstring current;
  auto finish = [&] {
    std::wstring normalized;
    for (const wchar_t ch : current) {
      if (std::iswxdigit(ch)) {
        normalized.push_back(static_cast<wchar_t>(std::towlower(ch)));
      }
    }
    if (normalized.size() == 64) out.push_back(std::move(normalized));
    current.clear();
  };
  for (const wchar_t ch : configured) {
    if (ch == L';' || ch == L',') finish();
    else current.push_back(ch);
  }
  finish();
  return out;
}

inline bool VerifyAuthenticodePublisher(const std::filesystem::path& path,
                                        const std::wstring& expectedPublisher) {
  WINTRUST_FILE_INFO fileInfo{};
  fileInfo.cbStruct = sizeof(fileInfo);
  fileInfo.pcwszFilePath = path.c_str();

  WINTRUST_DATA trustData{};
  trustData.cbStruct = sizeof(trustData);
  trustData.dwUIChoice = WTD_UI_NONE;
  trustData.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
  trustData.dwUnionChoice = WTD_CHOICE_FILE;
  trustData.pFile = &fileInfo;
  trustData.dwStateAction = WTD_STATEACTION_VERIFY;
  trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG status = WinVerifyTrust(nullptr, &policy, &trustData);
  trustData.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(nullptr, &policy, &trustData);
  if (status != ERROR_SUCCESS) return false;
  if (expectedPublisher.empty()) return true;
  const auto publisher = AuthenticodePublisher(path);
  return publisher && LowerWide(*publisher) == LowerWide(expectedPublisher);
}

inline bool VerifyAuthenticodeSigner(
    const std::filesystem::path& path, const std::wstring& expectedPublisher,
    const std::wstring& allowedThumbprints) {
  const auto pins = ParseSignerThumbprints(allowedThumbprints);
  if (pins.empty()) return false;
  if (!VerifyAuthenticodePublisher(path, expectedPublisher)) return false;
  const auto thumbprint = AuthenticodeSignerSha256(path);
  return thumbprint &&
         std::find(pins.begin(), pins.end(), LowerWide(*thumbprint)) != pins.end();
}

}  // namespace feathercast::updater
