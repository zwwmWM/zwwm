#pragma once

#include <zterm/terminal.hpp>
#include "font.hpp"
#include <array>
#include <string>
#include <vector>
#include <stdexcept>
#include <utility>
#include <optional>

namespace zterm {

struct Options {
  enum class Action { Run, Help, Daemon, ListThemes };
  FontFamilies fonts;
  unsigned size = 16, weight = 400, history_lines = 10000;
  float text_rounding = 0.5f, icon_scale = 1.3f;
  float background_opacity = 1.f;
  bool standalone = false;
  bool mouse_reporting = true, alternate_scroll = true;
  unsigned scroll_lines = 3, touchpad_scroll_per_mille = 1000;
  CursorStyle cursor{};
  unsigned width = 960, height = 600;
  unsigned padding_x = 10, padding_y = 10;
  Color foreground{220,225,232}, background{16,19,25};
  std::array<float, 4> selection{0.22f,0.48f,0.9f,0.45f};
  std::optional<Color> selection_foreground, cursor_color, cursor_text;
  std::string theme;
  std::vector<std::string> command, shell;
  std::vector<Color> palette;
  std::string config_path;
  std::vector<std::string> config_sources;
  Action action = Action::Run;
};

struct ConfigError : std::runtime_error {
  ConfigError(std::string message, std::vector<std::string> paths)
      : std::runtime_error(std::move(message)), sources(std::move(paths)) {}
  std::vector<std::string> sources;
};

Options load_options(int argc, char** argv);
void print_help();

}  // namespace zterm
