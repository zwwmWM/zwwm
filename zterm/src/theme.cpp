#include "theme.hpp"
#include "config.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>

namespace zterm {
namespace lang = zwwm::lang;
namespace {

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return value.substr(first,value.find_last_not_of(" \t\r\n")-first+1);
}

std::vector<std::filesystem::path> directories() {
  std::vector<std::filesystem::path> result;
  const auto* home = std::getenv("HOME");
  const auto* xdg = std::getenv("XDG_CONFIG_HOME");
  const std::filesystem::path config = xdg && *xdg ? xdg : home ? std::filesystem::path(home)/".config" : std::filesystem::path{};
  if (!config.empty()) { result.push_back(config/"zterm/themes"); result.push_back(config/"ghostty/themes"); }
  if (const auto* data = std::getenv("XDG_DATA_HOME")) result.push_back(std::filesystem::path(data)/"zterm/themes");
  else if (home) result.push_back(std::filesystem::path(home)/".local/share/zterm/themes");
  if (const auto* ghostty = std::getenv("GHOSTTY_RESOURCES_DIR")) result.push_back(std::filesystem::path(ghostty)/"themes");
  const auto* environment = std::getenv("XDG_DATA_DIRS");
  const std::string data = environment ? environment : "/usr/local/share:/usr/share";
  for (size_t start = 0; start <= data.size();) {
    const auto end = data.find(':',start);
    const auto directory = data.substr(start,end == std::string::npos ? end : end-start);
    if (!directory.empty()) {
      result.push_back(std::filesystem::path(directory)/"zterm/themes");
      result.push_back(std::filesystem::path(directory)/"ghostty/themes");
    }
    if (end == std::string::npos) break;
    start = end+1;
  }
  result.emplace_back(ZTERM_THEME_DIR); result.emplace_back(ZTERM_SOURCE_THEME_DIR);
  return result;
}

std::string color(std::string value) {
  value = trim(std::move(value));
  if (!value.empty() && value.front() != '#') value.insert(value.begin(),'#');
  return value;
}

}  // namespace

ThemeDocument read_theme(const std::string& name, const std::filesystem::path& relative, std::vector<std::string>& sources) {
  std::vector<std::filesystem::path> candidates;
  const std::filesystem::path requested(name);
  const auto add = [&](const std::filesystem::path& path) {
    candidates.push_back(path);
    if (!path.has_extension()) { candidates.emplace_back(path.string()+".conf"); candidates.emplace_back(path.string()+".zw"); }
  };
  if (requested.is_absolute()) add(requested);
  else {
    if (!relative.empty()) { add(relative/"themes"/requested); add(relative/requested); }
    for (const auto& directory : directories()) add(directory/requested);
  }
  std::filesystem::path path;
  for (const auto& candidate : candidates) {
    sources.push_back(std::filesystem::absolute(candidate).lexically_normal().string());
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate,error)) { path = candidate; break; }
  }
  if (path.empty()) throw ConfigError("theme '"+name+"' not found",sources);
  path = std::filesystem::absolute(path).lexically_normal();
  sources.push_back(std::filesystem::weakly_canonical(path).string());
  ThemeDocument result{path,{}};
  if (path.extension() == ".zw") {
    auto parsed = lang::parse_config_file(path.string());
    sources.insert(sources.end(),parsed.source_paths.begin(),parsed.source_paths.end());
    if (!parsed.ok()) {
      std::string error;
      for (const auto& diagnostic : parsed.diagnostics) if (diagnostic.level == lang::DiagnosticLevel::error)
        error += path.string()+":"+std::to_string(diagnostic.location.line)+": "+diagnostic.message+"\n";
      throw ConfigError(error,sources);
    }
    result.config = std::move(parsed.config); return result;
  }
  std::ifstream file(path);
  if (!file) throw ConfigError("cannot open theme: "+path.string(),sources);
  constexpr std::array<const char*,16> palette{"black","red","green","yellow","blue","magenta","cyan","white",
    "bright-black","bright-red","bright-green","bright-yellow","bright-blue","bright-magenta","bright-cyan","bright-white"};
  size_t line_number = 0;
  const auto field = [&](const std::string& section, const std::string& key, lang::Value value) {
    lang::Value::Object object; object.add(key,std::move(value));
    result.config.assignments.push_back({{line_number,1},section,{std::move(object)}});
  };
  for (std::string line; std::getline(file,line);) {
    ++line_number; line = trim(std::move(line));
    if (line.empty() || line.front() == '#') continue;
    const auto separator = line.find('=');
    if (separator == std::string::npos) throw ConfigError(path.string()+":"+std::to_string(line_number)+": expected key = value",sources);
    const auto key = trim(line.substr(0,separator));
    auto value = trim(line.substr(separator+1));
    if (const auto comment = value.find(" #"); comment != std::string::npos) value = trim(value.substr(0,comment));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1,value.size()-2);
    if (key == "theme") result.config.assignments.push_back({{line_number,1},"theme",{value}});
    else if (key == "foreground" || key == "background") field("colors",key,{color(value)});
    else if (key == "selection-background") field("colors","selection",{color(value)});
    else if (key == "selection-foreground") field("colors","selection-foreground",{color(value)});
    else if (key == "cursor-color") field("cursor","color",{color(value)});
    else if (key == "cursor-text") field("cursor","text-color",{color(value)});
    else if (key == "background-opacity") {
      float opacity = 0; const auto [end,error] = std::from_chars(value.data(),value.data()+value.size(),opacity);
      if (error != std::errc{} || end != value.data()+value.size() || !std::isfinite(opacity) || opacity < 0 || opacity > 1)
        throw ConfigError(path.string()+":"+std::to_string(line_number)+": invalid background opacity",sources);
      field("rendering","background-opacity-per-mille",{std::int64_t(std::lround(opacity*1000))});
    } else if (key == "palette") {
      const auto equals = value.find('='); unsigned index = 0;
      const auto number = trim(value.substr(0,equals));
      const auto [end,error] = std::from_chars(number.data(),number.data()+number.size(),index);
      if (equals == std::string::npos || error != std::errc{} || end != number.data()+number.size() || index > 255)
        throw ConfigError(path.string()+":"+std::to_string(line_number)+": invalid palette entry",sources);
      if (index < palette.size()) field("colors",palette[index],{color(value.substr(equals+1))});
    }
  }
  return result;
}

std::vector<std::string> theme_names() {
  std::set<std::string> names;
  for (const auto& directory : directories()) {
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory,error),end; !error && it != end; it.increment(error)) {
      if (!it->is_regular_file(error)) continue;
      const auto path = it->path();
      if (path.extension().empty() || path.extension() == ".conf" || path.extension() == ".zw") names.insert(path.stem().string());
    }
  }
  return {names.begin(),names.end()};
}

}  // namespace zterm
