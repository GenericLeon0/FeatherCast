#pragma once

#include "app_types.hpp"

#include <string>
#include <vector>

namespace feathercast::capabilities {

struct CapabilityDescriptor {
  std::wstring stableId;
  std::wstring category;
  std::wstring title;
  std::wstring summary;
  std::vector<std::wstring> keywords;
  std::wstring example;
  app::CapabilityAction action;
};

const std::vector<CapabilityDescriptor>& Catalog();
std::vector<const CapabilityDescriptor*> Search(const std::wstring& query);
app::DisplayItem Display(const CapabilityDescriptor& descriptor);
bool ValidateCatalog(std::wstring* error = nullptr);

}  // namespace feathercast::capabilities
