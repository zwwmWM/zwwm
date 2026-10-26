#pragma once

#include <zwwm/lang/config.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace zterm {

struct ThemeDocument { std::filesystem::path path; zwwm::lang::Config config; };
ThemeDocument read_theme(const std::string& name, const std::filesystem::path& relative,
                         std::vector<std::string>& sources);
std::vector<std::string> theme_names();

}  // namespace zterm
