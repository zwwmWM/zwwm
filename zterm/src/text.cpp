#include "text.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <hb.h>
#include <cmath>

namespace zterm {
namespace {

void font_options(PangoContext* context, bool cell) {
  auto* options = cairo_font_options_create();
  cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
  cairo_font_options_set_hint_style(options, cell ? CAIRO_HINT_STYLE_NONE : CAIRO_HINT_STYLE_SLIGHT);
  cairo_font_options_set_hint_metrics(options, cell ? CAIRO_HINT_METRICS_OFF : CAIRO_HINT_METRICS_ON);
  pango_cairo_context_set_font_options(context, options);
  pango_context_set_round_glyph_positions(context, !cell);
  cairo_font_options_destroy(options);
}

}  // namespace

Text::Text() {
  map_ = pango_cairo_font_map_new();
  context_ = pango_font_map_create_context(map_);
  pango_context_set_base_dir(context_, PANGO_DIRECTION_LTR);
  font_options(context_, false);
  font_ = pango_font_description_new();
}

Text::~Text() {
  pango_font_description_free(font_);
  g_object_unref(context_); g_object_unref(map_);
}

void Text::configure(const FontFamilies& requested, unsigned size, unsigned weight) {
  font_options(context_, false);
  weight_ = weight;
  PangoFontFamily** families = nullptr; int count = 0;
  pango_font_map_list_families(map_, &families, &count);
  std::vector<std::pair<std::string, std::string>> missing;
  const auto resolve = [&](const std::string& family, const std::string& fallback, const std::string& role) {
    if (family.empty()) return fallback;
    bool available = false;
    char** names = g_strsplit(family.c_str(), ",", -1);
    for (char** name = names; *name; ++name) {
      g_strstrip(*name);
      for (const char* generic : {"monospace", "sans-serif", "sans", "serif", "system-ui", "emoji", "math", "cursive", "fantasy"})
        if (g_ascii_strcasecmp(*name, generic) == 0) available = true;
      for (int i = 0; i < count; ++i)
        if (g_ascii_strcasecmp(*name, pango_font_family_get_name(families[i])) == 0) available = true;
    }
    g_strfreev(names);
    if (!available) missing.emplace_back(role, family);
    return available ? family : fallback;
  };
  families_.mono = resolve(requested.mono, "monospace", "mono");
  families_.bold = resolve(requested.bold, "", "bold");
  families_.italic = resolve(requested.italic, "", "italic");
  families_.bold_italic = resolve(requested.bold_italic, "", "bold-italic");
  families_.symbols = resolve(requested.symbols, "", "symbols");
  families_.emoji = resolve(requested.emoji, "", "emoji");
  g_free(families);
  pango_font_description_set_family(font_, families_.mono.c_str());
  pango_font_description_set_absolute_size(font_, size * PANGO_SCALE);
  pango_font_description_set_weight(font_, static_cast<PangoWeight>(weight_));
  pango_font_description_set_style(font_, PANGO_STYLE_NORMAL);
  auto* loaded = pango_font_map_load_font(map_, context_, font_);
  if (!loaded) throw std::runtime_error("font '" + requested.mono + "' could not be loaded and no fallback font is available");
  auto* actual = pango_font_describe(loaded);
  const char* actual_family = pango_font_description_get_family(actual);
  family_ = actual_family ? actual_family : families_.mono;
  pango_font_description_free(actual); g_object_unref(loaded);
  for (const auto& [role, family] : missing) if (font_warnings_.insert(role + ":" + family).second)
    std::fprintf(stderr, "zterm: font.%s '%s' not found; falling back to '%s'\n", role.c_str(), family.c_str(), family_.c_str());
  auto* metrics = pango_context_get_metrics(context_, font_, pango_language_get_default());
  baseline_ = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(metrics));
  height_ = std::max(1, baseline_ + PANGO_PIXELS_CEIL(pango_font_metrics_get_descent(metrics)));
  pango_font_metrics_unref(metrics);
  auto* layout = pango_layout_new(context_);
  pango_layout_set_font_description(layout, font_); pango_layout_set_text(layout, "M", 1);
  int width = 0; pango_layout_get_pixel_size(layout, &width, nullptr);
  width_ = std::max(1, width); g_object_unref(layout);
}

std::string Text::font_family(FontRole role, bool bold, bool italic) const {
  const auto& base = bold && italic && !families_.bold_italic.empty() ? families_.bold_italic :
                     bold && !families_.bold.empty() ? families_.bold :
                     italic && !families_.italic.empty() ? families_.italic : families_.mono;
  std::string family = role == FontRole::Symbols && !families_.symbols.empty() ? families_.symbols :
                       role == FontRole::Emoji && !families_.emoji.empty() ? families_.emoji : base;
  for (const auto* fallback : {&base, &families_.mono, &families_.symbols, &families_.emoji})
    if (!fallback->empty() && *fallback != family) family += ", " + *fallback;
  return family;
}

Text::Bitmap Text::rasterize(std::string_view text, bool bold, bool italic, std::uint32_t rgb, bool grid, bool separator, FontRole role) {
  Bitmap graphic;
  if (grid && cell_graphic(text, graphic)) return graphic;
  const bool cell = grid || separator;
  font_options(context_, cell);
  const auto family = font_family(role, bold, italic);
  pango_font_description_set_family(font_, family.c_str());
  pango_font_description_set_weight(font_, static_cast<PangoWeight>(role == FontRole::Emoji ? 400U : bold ? std::max(weight_, 700U) : weight_));
  pango_font_description_set_style(font_, italic && role != FontRole::Emoji ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
  auto* layout = pango_layout_new(context_);
  pango_layout_set_font_description(layout, font_);
  pango_layout_set_single_paragraph_mode(layout, TRUE);
  pango_layout_set_auto_dir(layout, FALSE);
  pango_layout_set_text(layout, text.data(), text.size());
  bool color = false;
  if (!cell) {
    auto* iter = pango_layout_get_iter(layout);
    do {
      const auto* run = pango_layout_iter_get_run_readonly(iter);
      if (run) for (int i = 0; i < run->glyphs->num_glyphs; ++i) color |= run->glyphs->glyphs[i].attr.is_color;
    } while (pango_layout_iter_next_run(iter));
    pango_layout_iter_free(iter);
  }
  if (pango_layout_get_unknown_glyphs_count(layout)) {
    auto* iter = pango_layout_get_iter(layout);
    do {
      const auto* run = pango_layout_iter_get_run_readonly(iter);
      if (!run) continue;
      for (int i = 0; i < run->glyphs->num_glyphs; ++i) {
        const auto glyph = run->glyphs->glyphs[i].glyph;
        if (!(glyph & PANGO_GLYPH_UNKNOWN_FLAG)) continue;
        const char32_t character = glyph & ~PANGO_GLYPH_UNKNOWN_FLAG;
        if (missing_glyphs_.size() < 16 && missing_glyphs_.insert(character).second)
          std::fprintf(stderr, "zterm: glyph U+%04X not found in '%s' or its fallback fonts\n", unsigned(character), family.c_str());
      }
    } while (pango_layout_iter_next_run(iter));
    pango_layout_iter_free(iter);
  }
  PangoRectangle ink{}; pango_layout_get_pixel_extents(layout, &ink, nullptr);
  PangoRectangle exact_ink{}; pango_layout_get_extents(layout, &exact_ink, nullptr);
  Bitmap bitmap;
  if (grid) {
    auto* iter = pango_layout_get_iter(layout);
    do {
      const auto* run = pango_layout_iter_get_run_readonly(iter);
      if (!run || !run->item->analysis.font || !run->glyphs->num_glyphs) continue;
      auto* font = run->item->analysis.font;
      hb_codepoint_t glyph = 0;
      PangoRectangle reference{}, logical{};
      if (hb_font_get_nominal_glyph(pango_font_get_hb_font(font), 0x2588, &glyph)) {
        pango_font_get_glyph_extents(font, glyph, &reference, nullptr);
      } else {
        pango_font_get_glyph_extents(font, run->glyphs->glyphs[0].glyph, nullptr, &logical);
        auto* metrics = pango_font_get_metrics(font, pango_language_get_default());
        reference = {0, -pango_font_metrics_get_ascent(metrics), logical.width,
                     pango_font_metrics_get_ascent(metrics) + pango_font_metrics_get_descent(metrics)};
        pango_font_metrics_unref(metrics);
      }
      bitmap.reference = {reference.x / float(PANGO_SCALE), baseline_ + reference.y / float(PANGO_SCALE),
                          reference.width / float(PANGO_SCALE), reference.height / float(PANGO_SCALE)};
      break;
    } while (pango_layout_iter_next_run(iter));
    pango_layout_iter_free(iter);
  }
  bitmap.x = ink.x - 1;
  bitmap.y = ink.y - 1 + baseline_ - PANGO_PIXELS(pango_layout_get_baseline(layout));
  bitmap.color = color;
  bitmap.scale = color ? 1 : 2;
  if (ink.width <= 0 || ink.height <= 0) { g_object_unref(layout); return bitmap; }
  bitmap.width = (ink.width + 2) * bitmap.scale;
  bitmap.height = (ink.height + 2) * bitmap.scale;
  const float baseline = baseline_ - pango_layout_get_baseline(layout) / float(PANGO_SCALE);
  auto reference = bitmap.reference;
  if (separator) reference = {exact_ink.x / float(PANGO_SCALE), exact_ink.y / float(PANGO_SCALE) + baseline,
                              exact_ink.width / float(PANGO_SCALE), exact_ink.height / float(PANGO_SCALE)};
  if (cell) {
    if (reference.width <= 0 || reference.height <= 0) reference = {0,0,float(width_),float(height_)};
    bitmap.width = width_ * bitmap.scale; bitmap.height = height_ * bitmap.scale;
    bitmap.x = bitmap.y = 0;
    bitmap.reference = {0,0,float(width_),float(height_)};
  }
  if (bitmap.width > 2046 || bitmap.height > 2046) {
    g_object_unref(layout); throw std::runtime_error("text tile exceeds the GPU atlas");
  }
  auto* surface = cairo_image_surface_create(color ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_A8, bitmap.width, bitmap.height);
  auto* cairo = cairo_create(surface);
  cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR); cairo_paint(cairo);
  cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
  cairo_scale(cairo, bitmap.scale * (cell ? width_ / reference.width : 1),
                     bitmap.scale * (cell ? height_ / reference.height : 1));
  cairo_set_source_rgba(cairo, ((rgb >> 16U) & 255U) / 255., ((rgb >> 8U) & 255U) / 255., (rgb & 255U) / 255., 1);
  cairo_move_to(cairo, cell ? -reference.x : 1 - ink.x, cell ? baseline - reference.y : 1 - ink.y);
  pango_cairo_show_layout(cairo, layout);
  const auto status = cairo_status(cairo);
  cairo_destroy(cairo); g_object_unref(layout); cairo_surface_flush(surface);
  if (status != CAIRO_STATUS_SUCCESS || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface); throw std::runtime_error("cannot rasterize text");
  }
  const auto* source = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  bitmap.pixels.resize(static_cast<size_t>(bitmap.width) * bitmap.height * (color ? 4 : 1));
  for (int row = 0; row < bitmap.height; ++row) {
    if (!color) std::memcpy(bitmap.pixels.data() + static_cast<size_t>(row) * bitmap.width, source + row * stride, bitmap.width);
    else for (int column = 0; column < bitmap.width; ++column) {
      auto* pixel = bitmap.pixels.data() + (static_cast<size_t>(row) * bitmap.width + column) * 4;
      std::uint32_t argb = 0; std::memcpy(&argb, source + row * stride + column * 4, 4);
      pixel[0] = argb >> 16U; pixel[1] = argb >> 8U; pixel[2] = argb; pixel[3] = argb >> 24U;
    }
  }
  cairo_surface_destroy(surface);
  return bitmap;
}

bool Text::cell_graphic(std::string_view text, Bitmap& bitmap) const {
  const auto character = g_utf8_get_char_validated(text.data(),text.size());
  if (!((character >= 0x2580 && character <= 0x259f) ||
        (character >= 0x1fb00 && character <= 0x1fb3b) || character == 0x1fb83)) return false;
  bitmap.width = width_ * 2; bitmap.height = height_ * 2; bitmap.scale = 2;
  if (bitmap.width > 2046 || bitmap.height > 2046) throw std::runtime_error("cell glyph exceeds the GPU atlas");
  bitmap.reference = {0,0,float(width_),float(height_)};
  auto* surface = cairo_image_surface_create(CAIRO_FORMAT_A8,bitmap.width,bitmap.height);
  auto* cairo = cairo_create(surface);
  cairo_set_operator(cairo,CAIRO_OPERATOR_CLEAR); cairo_paint(cairo);
  cairo_set_operator(cairo,CAIRO_OPERATOR_SOURCE); cairo_set_source_rgba(cairo,1,1,1,1);
  cairo_scale(cairo,2,2);
  const auto rectangle = [&](float left, float top, float right, float bottom) {
    const double x1 = std::round(width_ * left), y1 = std::round(height_ * top);
    const double x2 = std::round(width_ * right), y2 = std::round(height_ * bottom);
    cairo_rectangle(cairo,x1,y1,x2-x1,y2-y1); cairo_fill(cairo);
  };
  if (character == 0x2580) rectangle(0,0,1,0.5f);
  else if (character >= 0x2581 && character <= 0x2588) rectangle(0,1-(character-0x2580)/8.f,1,1);
  else if (character >= 0x2589 && character <= 0x258f) rectangle(0,0,(0x2590-character)/8.f,1);
  else if (character == 0x2590) rectangle(0.5f,0,1,1);
  else if (character >= 0x2591 && character <= 0x2593) {
    for (unsigned y = 0; y < height_; ++y) for (unsigned x = 0; x < width_; ++x) {
      const auto phase = (x & 1U) + ((y & 1U) << 1U);
      const bool filled = character == 0x2591 ? phase == 0 : character == 0x2592 ? ((x+y)&1U) == 0 : phase != 0;
      if (filled) { cairo_rectangle(cairo,x,y,1,1); cairo_fill(cairo); }
    }
  } else if (character == 0x2594) rectangle(0,0,1,0.125f);
  else if (character == 0x2595) rectangle(0.875f,0,1,1);
  else if (character <= 0x259f) {
    constexpr unsigned masks[]{4,8,1,13,9,7,11,2,6,14};
    const auto mask = masks[character-0x2596];
    for (unsigned quadrant = 0; quadrant < 4; ++quadrant) if (mask & (1U<<quadrant)) {
      const float x = (quadrant%2)*0.5f, y = (quadrant/2)*0.5f;
      rectangle(x,y,x+0.5f,y+0.5f);
    }
  } else if (character == 0x1fb83) rectangle(0,0,1,3/8.f);
  else {
    unsigned mask = character-0x1fb00+1;
    if (mask >= 21) ++mask;
    if (mask >= 42) ++mask;
    for (unsigned sextant = 0; sextant < 6; ++sextant) if (mask & (1U<<sextant)) {
      const float x = (sextant%2)*0.5f, y = (sextant/2)/3.f;
      rectangle(x,y,x+0.5f,(sextant/2+1)/3.f);
    }
  }
  const auto status = cairo_status(cairo); cairo_destroy(cairo); cairo_surface_flush(surface);
  if (status != CAIRO_STATUS_SUCCESS || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface); throw std::runtime_error("cannot rasterize cell glyph");
  }
  const auto* pixels = cairo_image_surface_get_data(surface);
  const auto stride = cairo_image_surface_get_stride(surface);
  bitmap.pixels.resize(static_cast<size_t>(bitmap.width)*bitmap.height);
  for (int y = 0; y < bitmap.height; ++y)
    std::memcpy(bitmap.pixels.data()+static_cast<size_t>(y)*bitmap.width,pixels+y*stride,bitmap.width);
  cairo_surface_destroy(surface); return true;
}

void Text::append_utf8(std::string& text, char32_t character) {
  if (character > 0x10ffff || (character >= 0xd800 && character <= 0xdfff)) character = 0xfffd;
  if (character < 0x80) text.push_back(character);
  else if (character < 0x800) { text.push_back(0xc0 | (character >> 6)); text.push_back(0x80 | (character & 63)); }
  else if (character < 0x10000) {
    text.push_back(0xe0 | (character >> 12)); text.push_back(0x80 | ((character >> 6) & 63)); text.push_back(0x80 | (character & 63));
  } else {
    text.push_back(0xf0 | (character >> 18)); text.push_back(0x80 | ((character >> 12) & 63));
    text.push_back(0x80 | ((character >> 6) & 63)); text.push_back(0x80 | (character & 63));
  }
}

unsigned Text::parse_weight(std::string_view value) {
  std::string name;
  for (const unsigned char character : value) if (character != '-' && character != '_' && character != ' ')
    name.push_back(static_cast<char>(std::tolower(character)));
  constexpr std::pair<std::string_view, unsigned> names[] = {
    {"thin",100},{"extralight",200},{"ultralight",200},{"light",300},{"regular",400},{"normal",400},
    {"medium",500},{"semibold",600},{"demibold",600},{"bold",700},{"extrabold",800},{"ultrabold",800},
    {"black",900},{"heavy",900},{"extrablack",1000}};
  for (const auto& [label, weight] : names) if (name == label) return weight;
  unsigned weight = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), weight);
  if (error == std::errc{} && end == value.data() + value.size() && weight >= 100 && weight <= 1000) return weight;
  throw std::runtime_error("font weight must be a named weight or an integer in 100..1000");
}

}  // namespace zterm
