/* herbe for Windows - configuration.
 * Copy to config.h (build.bat / CMake do it for you) and edit.
 * Mirrors upstream config.def.h; X11-only knobs were translated:
 *   font_pattern "monospace:size=10"  -> font_face + font_size (points)
 *   DISMISS_BUTTON Button1            -> HERBE_BTN_LEFT
 *   ACTION_BUTTON  Button3            -> HERBE_BTN_RIGHT
 */
static const char *background_color = "#3e3e3e";
static const char *border_color = "#ececec";
static const char *font_color = "#ececec";
static const wchar_t *font_face = L"Consolas";
static const int font_size = 10; /* points, scaled by monitor DPI */
static const unsigned line_spacing = 5;
static const unsigned int padding = 15;

static const unsigned int width = 450;
static const unsigned int border_size = 2;
static const unsigned int pos_x = 30;
static const unsigned int pos_y = 60;

enum corners { TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT };
enum corners corner = TOP_RIGHT;

static const unsigned int duration = 5; /* in seconds, 0 = until clicked */

#define DISMISS_BUTTON HERBE_BTN_LEFT
#define ACTION_BUTTON HERBE_BTN_RIGHT
