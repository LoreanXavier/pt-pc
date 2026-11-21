#pragma once

#include <string>
#include <vector>

namespace pt::third_party {

std::string LoadedTools(const std::vector<std::string>& own_proxies = {});

std::string ToolNames(const std::string& loaded_tools);

std::string ProxyToolForPdb(const std::string& pdb);

}
