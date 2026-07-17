#pragma once

#include "app_types.hpp"

namespace feathercast::search_pipeline {

app::ResultsCollection ComputeResults(const app::QueryRequest& request);

}  // namespace feathercast::search_pipeline
