#include <ftw.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/stat.h>

#include <chrono>
#include <vector>
#include <thread>
#include <optional>

#define SCRATCH_BUFFER_IMPLEMENTATION
#include "scratch_buffer.h"

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// only for nob_cmd_run_async
#include "nob.h"

#include <raylib.h>

#include <opencv2/opencv.hpp>
#include <opencv2/imgcodecs.hpp>

using namespace cv;

#include "taglib/mpegfile.h"
#include "taglib/id3v2tag.h"
#include "taglib/attachedpictureframe.h"

using namespace TagLib;

#define streq(s1, s2) (strcmp(s1, s2) == 0)
#define eprintf(...) fprintf(stderr, __VA_ARGS__)

#define TILE_COLOR DARKGRAY
#define BACKGROUND_COLOR ((Color) {24, 24, 24, 255})
#define CLICKED_TILE_COLOR ((Color) {40, 40, 40, 255})
#define MATCHED_TILE_COLOR ((Color) {40, 160, 150, 255})
#define SEARCH_WINDOW_BACKGROUND_COLOR ((Color) {65, 65, 65, 215})
#define IS_DELETE_WINDOW_BACKGROUND_COLOR ((Color) {50, 50, 50, 225})

#define MAX_PATH_SIZE (256 + 1)

#define RESOURCES_DIR "resources"

#define FONT_PATH (RESOURCES_DIR "/" "Iosevka-Regular.ttf")
#define PLACEHOLDER_PATH (RESOURCES_DIR "/" "placeholder.png")

#define DEFAULT_TILE_WIDTH   120
#define DEFAULT_TILE_HEIGHT  125
#define DEFAULT_TILE_SPACING 21
#define DEFAULT_TEXT_PADDING 6
#define DEFAULT_FONT_SIZE    20
#define DEFAULT_TEXT_SPACING 2.0f

#define SEARCH_TEXT_HEIGHT 25
#define SEARCH_TEXT_SPACING 3.0f

#define DEFAULT_SCROLL_SPEED 50.0f
#define SCROLL_SPEED_BOOST_FACTOR 2.5f
#define BOOSTED_SCROLL_SPEED (DEFAULT_SCROLL_SPEED * SCROLL_SPEED_BOOST_FACTOR)

static int tile_width			= DEFAULT_TILE_WIDTH;
static int tile_height		= DEFAULT_TILE_HEIGHT;
static int tile_spacing		= DEFAULT_TILE_SPACING;
static int text_padding		= DEFAULT_TEXT_PADDING;
static int font_size			= DEFAULT_FONT_SIZE;
static float text_spacing = DEFAULT_TEXT_SPACING;
static float scroll_speed = DEFAULT_SCROLL_SPEED;

#define tile_full_height (tile_height + tile_spacing)

static bool delete_mode = false;
static bool is_delete_rec = false;

static bool search_mode = false;
static bool typing_search = false;
static char search_string[MAX_PATH_SIZE] = {0};
static size_t search_string_size = 0;

#define key_is_printable(key) (key >= 39 && key <= 96)

static char *curr_dir = ".";

static Font font = {0};

#define DOUBLE_CLICK_THRESHOLD 0.3f
static Vector2 last_click_pos = {0};
static double last_click_time = 0.0;

#define DOUBLE_DOT_THRESHOLD 0.3f
static double last_dot_time = 0.0;

typedef struct {int x, y;} Vector2i;

static float last_scroll_offset_y = 0.0;
static Vector2i selected_tile_pos = {0};
static Vector2i selected_tile_pos_before_entering_dir = {0};

static float scroll_offset_y = 0.0;

#define SCALE_THRESHOLD 0.1f
static double last_scale_time = 0.0;

#define DEFAULT_SCALE 1.0f
#define MIN_SCALE DEFAULT_SCALE
#define MAX_SCALE 3.0f
#define SCALE_STEP 0.25f

static float scale = DEFAULT_SCALE;

static Image placeholder_src = {0};
static Image placeholder_scaled = {0};
static Texture2D placeholder_texture = {0};
static bool placeholder_resized = false;

typedef struct path_t path_t;

static std::vector<Nob_Proc> procs = {};
static std::vector<Texture2D> to_unload = {};
static std::vector<path_t> to_load = {};

static size_t last_matched_idx = 1;
static std::vector<size_t> matched_idxs = {};

#define DEFINE_SIZE(what) \
	const size_t what##_SIZE = sizeof(what) / sizeof(*what)

#define DEFINE_IS(what, array) \
	INLINE static bool is_##what(const char *ext) \
	{ \
		for (size_t i = 0; i < array##_SIZE; ++i) { \
			if (streq(ext, array[i])) return true; \
		} \
		return false; \
	}

static const char *const VIDEO_FILE_EXTENSIONS[] = {"mp4", "mov", "mkv"};
DEFINE_SIZE(VIDEO_FILE_EXTENSIONS);
DEFINE_IS(video, VIDEO_FILE_EXTENSIONS);

static const char *const MUSIC_FILE_EXTENSIONS[] = {"mp3", "wav", "flac", "ogg"};
DEFINE_SIZE(MUSIC_FILE_EXTENSIONS);
DEFINE_IS(music, MUSIC_FILE_EXTENSIONS);

static const char *const IMAGE_FILE_EXTENSIONS[] = {"png", "jpg", "bmp", "tga", "psd", "gif", "hdr", "pic", "pnm"};
DEFINE_SIZE(IMAGE_FILE_EXTENSIONS);
DEFINE_IS(image, IMAGE_FILE_EXTENSIONS);

typedef struct {
	Image src_img;
	Image scaled_img;
	bool is_placeholder;
	std::optional<Texture2D> loaded_texture;
} img_value_t;

typedef struct {
	size_t key;
	img_value_t value;
} img_map_t;

static img_map_t *img_map = NULL;

struct path_t {
	char *str;
	size_t ino;
	uint8_t type;
	bool abs, deleted;
};

std::vector<path_t> paths = {};

// in millis
#define PREVIEW_LOADER_SLEEP_TIME 256

// Flags to communicate with the thread that loads previews
static bool stop_flag = false;
static bool idle_flag = false;
static bool new_scale_flag = false;

INLINE static int pixel_format_to_amount_of_bytes(int pixel_format)
{
	switch (pixel_format) {
	case PIXELFORMAT_UNCOMPRESSED_R8G8B8:	return 3;
	default: return 4;
	}
}

INLINE static void *copy_img_data(const Image *img)
{
	const size_t data_size = img->width *
													 img->height *
													 pixel_format_to_amount_of_bytes(img->format);

	uint8_t *data = (uint8_t *) malloc(data_size);
	uint8_t *original_data = (uint8_t *) img->data;
	memcpy(data, original_data, data_size);
	return data;
}

INLINE static void update_selected_tile_if_its_not_visible(void)
{
	const int tile_row = selected_tile_pos.y;
	const int visible_rows = GetScreenHeight() / (tile_height + tile_spacing);
	const int first_visible_row = scroll_offset_y / (tile_height + tile_spacing);
	const int last_visible_row = first_visible_row + visible_rows - 1;

	if (tile_row < first_visible_row || tile_row > last_visible_row) {
		scroll_offset_y = tile_row * (tile_height + tile_spacing);
	}
}

static void set_new_scale(float new_scale)
{
	scale				 = new_scale;
	tile_width	 = DEFAULT_TILE_WIDTH   * scale;
	tile_height	 = DEFAULT_TILE_HEIGHT  * scale;
	tile_spacing = DEFAULT_TILE_SPACING * scale;
	text_padding = DEFAULT_TEXT_PADDING * scale;
	font_size		 = DEFAULT_FONT_SIZE    * scale;
	text_spacing = DEFAULT_TEXT_SPACING * scale;

	UnloadFont(font);
	font = LoadFontEx(FONT_PATH, font_size, NULL, 0);

	new_scale_flag = true;
	idle_flag = false;

	if (placeholder_resized) {
		UnloadTexture(placeholder_texture);
		placeholder_texture = LoadTextureFromImage(placeholder_scaled);
		placeholder_resized = false;
	}

	update_selected_tile_if_its_not_visible();
}

static void read_dir(void)
{
	DIR *dir = opendir(curr_dir);
	if (dir == NULL) {
		eprintf("could not open directory %s: %s", curr_dir, strerror(errno));
		assert(0 && "unreachable");
	}

	errno = 0;
	struct dirent *e = readdir(dir);
	while (e != NULL) {
		if (!streq(e->d_name, ".")) {
			char *file_path = str_copy(e->d_name, strlen(e->d_name));
			size_t ino = (size_t) e->d_ino;
			uint8_t type = (uint8_t) e->d_type;
			paths.emplace_back((path_t) {
				.str = file_path,
				.ino = ino,
				.type = type,
				.abs = false,
				.deleted = false
			});
		}
		e = readdir(dir);
	}

	if (errno != 0) {
		eprintf("could not read directory %s: %s", curr_dir, strerror(errno));
		if (dir) closedir(dir);
		assert(0 && "unreachable");
	}

	closedir(dir);
}

INLINE static size_t get_tiles_count(void)
{
	return paths.size();
}

INLINE static int get_tiles_per_col(void)
{
	return GetScreenHeight() / (tile_height + tile_spacing);
}

INLINE static int get_tiles_per_row(void)
{
	return GetScreenWidth() / (tile_width + tile_spacing);
}

INLINE static char *join_dir(const char *dir_path)
{
	scratch_buffer_clear();
	scratch_buffer_append(curr_dir);
	scratch_buffer_append_char('/');
	scratch_buffer_append(dir_path);
	return scratch_buffer_copy();
}

INLINE static void preserve_tile_pos(size_t tile_idx)
{
	if (streq(paths[tile_idx].str, "..")) {
		scroll_offset_y = last_scroll_offset_y;
		selected_tile_pos.x = selected_tile_pos_before_entering_dir.x;
		selected_tile_pos.y = selected_tile_pos_before_entering_dir.y;
	} else {
		selected_tile_pos_before_entering_dir.x = selected_tile_pos.x;
		selected_tile_pos_before_entering_dir.y = selected_tile_pos.y;
		memset(&selected_tile_pos, 0, sizeof(selected_tile_pos));
		last_scroll_offset_y = scroll_offset_y;
		scroll_offset_y = 0.0;
	}
}

INLINE static Vector2 path_idx_to_tile_pos(size_t path_idx)
{
	const size_t tpr = get_tiles_per_row();
	return (Vector2) {
		.x = path_idx % tpr,
		.y = path_idx / tpr
	};
}

INLINE static Vector2 get_tile_pos(size_t tile_pos_x, size_t tile_pos_y)
{
	return (Vector2) {
		tile_pos_x * (tile_width + tile_spacing) + tile_spacing,
		tile_pos_y * (tile_height + tile_spacing) + tile_spacing - scroll_offset_y
	};
}

INLINE static char *get_top_file_path(char *src)
{
	for (int i = strlen(src) - 1; i >= 0; i--) {
		if (src[i] == '/') {
			return src + i + 1;
		}
	}
	return NULL;
}

INLINE static char *get_extension(char *src)
{
	for (int i = strlen(src) - 1; i >= 0; i--) {
		if (src[i] == '.') {
			char *ret = src + i + 1;
			if (src[i + 1] == '/') ret++;
			return ret;
		}
	}
	return NULL;
}

static void fill_img_map(void);

INLINE static void enter_dir(char *dir, size_t tile_idx)
{
	curr_dir = dir;
	idle_flag = true;
	paths.clear();
	read_dir();
	fill_img_map();
	idle_flag = false;
	preserve_tile_pos(tile_idx);
}

INLINE static void stop_search_mode(void)
{
	search_mode = false;
	typing_search = false;
	matched_idxs.clear();
	memset(search_string, 0, search_string_size);
	search_string_size = 0;
}

INLINE static void stop_delete_mode(void)
{
	delete_mode = false;
	is_delete_rec = false;
}

INLINE static void str_tolower(char *str)
{
	for (char *p = str; p != NULL && *p != '\0'; p++, *p = tolower(*p));
}

INLINE static size_t get_tile_idx_from_tile_pos(Vector2i pos)
{
	return pos.x + pos.y * get_tiles_per_row();
}

static void draw_text_truncated(const char *text,
																Vector2 pos,
																float max_text_width,
																Color color);

static size_t draw_is_delete(void)
{
	const int w = GetScreenWidth();
	const int h = GetScreenHeight();

	const int rw = w / 3;
	const int rh = h / 6;
	const int rx = (w - rw) / 2;
	const int ry = (h - rh) / 2;

	DrawRectangle(rx, ry, rw, rh, IS_DELETE_WINDOW_BACKGROUND_COLOR);

	const size_t tile_idx = get_tile_idx_from_tile_pos(selected_tile_pos);
	char *file_path = paths[tile_idx].str;

	if (is_delete_rec) {
		scratch_buffer_clear();
		scratch_buffer_printf("delete %s recursively? [y/n]", file_path);
	} else {
		scratch_buffer_clear();
		scratch_buffer_printf("delete %s? [y/n]", file_path);
	}

	char *text = scratch_buffer_to_string();

	const Vector2 ts = MeasureTextEx(font, text, font_size, text_spacing);

	const Vector2 tp = {
		rx + (rw - ts.x) / 2,
		ry + (rh - ts.y) / 2
	};

	draw_text_truncated(text, tp, rw, RAYWHITE);
	return tile_idx;
}

static void handle_enter(char *file_path);

static int rm_file_callback(const char *file_path,
														const struct stat *,
														int ,
														struct FTW *)
{
	if (remove(file_path) < 0) {
		perror("ERROR: remove");
		return -1;
	}
	return 0;
}

static void handle_keyboard_input(void)
{
	int key = GetKeyPressed();
	if (delete_mode) {
		if (key == KEY_ESCAPE) {
			stop_delete_mode();
			return;
		}

		const size_t tile_idx = draw_is_delete();

		if (key == KEY_Y) {
			if (is_delete_rec) {
				stop_delete_mode();

				nftw(paths[tile_idx].str,
						 rm_file_callback,
						 10,
						 FTW_DEPTH | FTW_MOUNT|FTW_PHYS);

				paths[tile_idx].deleted = true;
			} else if (paths[tile_idx].type == DT_DIR) {
				stop_delete_mode();
				return;
			}

			stop_delete_mode();

			errno = 0;

			if (remove(paths[tile_idx].str) == -1) {
				eprintf("failed to remove %s: %s\n",
								paths[tile_idx].str,
								strerror(errno));
				return;
			}

			paths[tile_idx].deleted = true;
		} else if (key == KEY_N) {
			is_delete_rec = false;
			delete_mode = false;
		}

		return;
	}

	const int tpr = get_tiles_per_row();

	if (search_mode) {
		const int w = GetScreenWidth();
		const int h = GetScreenHeight();
		const int rh = SEARCH_TEXT_HEIGHT + SEARCH_TEXT_SPACING * 2;
		const Vector2 text_pos = (Vector2) {
			SEARCH_TEXT_SPACING,
			h - SEARCH_TEXT_HEIGHT
		};

		DrawRectangle(0, h - rh, w, rh, SEARCH_WINDOW_BACKGROUND_COLOR);

		if (typing_search) goto draw_search;

		scratch_buffer_clear();

		scratch_buffer_printf("%d/%d match",
													last_matched_idx,
													matched_idxs.size());

		DrawTextEx(font,
							 scratch_buffer_to_string(),
							 text_pos,
							 font_size,
							 text_spacing,
							 RAYWHITE);

		if (key == KEY_ESCAPE || key == KEY_ENTER) {
			stop_search_mode();
			return;
		} else if (key == KEY_N) {
			if (last_matched_idx >= matched_idxs.size()) return;

			const size_t idx = matched_idxs[last_matched_idx++];
			const Vector2 new_selected_tile_pos = path_idx_to_tile_pos(idx);

			selected_tile_pos.x = new_selected_tile_pos.x;
			selected_tile_pos.y = new_selected_tile_pos.y;

			update_selected_tile_if_its_not_visible();
			return;
		} else if (key == KEY_P) {
			if (last_matched_idx == 0) return;

			const size_t idx = matched_idxs[last_matched_idx--];
			const Vector2 new_selected_tile_pos = path_idx_to_tile_pos(idx);

			selected_tile_pos.x = new_selected_tile_pos.x;
			selected_tile_pos.y = new_selected_tile_pos.y;

			update_selected_tile_if_its_not_visible();
			return;
		}

		if (!typing_search) return;

draw_search:

		if (key == KEY_ESCAPE) {
			stop_search_mode();
			return;
		}

		if (search_string_size > 0) {
			DrawTextEx(font,
								 search_string,
								 text_pos,
								 font_size,
								 text_spacing,
								 RAYWHITE);
		}

		if (key == KEY_ENTER) {
			typing_search = false;
			if (search_string_size < 1) return;

			str_tolower(search_string);

			for (size_t i = 0; i < paths.size(); ++i) {
				if (paths[i].deleted) continue;

				scratch_buffer_clear();
				scratch_buffer_append(paths[i].str);

				str_tolower(scratch_buffer.str);

				if (strstr(scratch_buffer.str, search_string) != NULL) {
					if (matched_idxs.empty()) {
						const Vector2 new_selected_tile_pos = path_idx_to_tile_pos(i);
						selected_tile_pos.x = new_selected_tile_pos.x;
						selected_tile_pos.y = new_selected_tile_pos.y;
						update_selected_tile_if_its_not_visible();
					}
					matched_idxs.emplace_back(i);
				}
			}

			if (matched_idxs.size() <= 1) {
				stop_search_mode();
			}

			memset(search_string, 0, search_string_size);
			search_string_size = 0;
		} else if (key == KEY_SPACE || key_is_printable(key)) {
			if (search_string_size == MAX_PATH_SIZE) return;
			if (!IsKeyDown(KEY_LEFT_SHIFT)) {
				key = tolower(key);
			}
			search_string[search_string_size++] = key;
		} else if (key == KEY_BACKSPACE) {
			search_string[--search_string_size] = '\0';
		}

		return;
	}

	const double curr_time = GetTime();

	if ((curr_time - last_scale_time) < SCALE_THRESHOLD) return;

	if (IsKeyDown(KEY_LEFT_CONTROL)) {
		if (IsKeyPressed(KEY_EQUAL)
		&& !IsKeyPressed(KEY_MINUS)
		&& scale < MAX_SCALE)
		{
			set_new_scale(scale + SCALE_STEP);
			last_scale_time = curr_time;
		} else if (IsKeyPressed(KEY_MINUS)
					 && !IsKeyPressed(KEY_EQUAL)
					 && scale > MIN_SCALE)
		{
			set_new_scale(scale - SCALE_STEP);
			last_scale_time = curr_time;
		}
	}

	if (IsKeyDown(KEY_LEFT_SHIFT)) {
		scroll_speed = BOOSTED_SCROLL_SPEED;
	} else {
		scroll_speed = DEFAULT_SCROLL_SPEED;
	}

	const int tpc = get_tiles_per_col();
	const int total_tiles = get_tiles_count();
	const int total_rows = (total_tiles + tpr - 1) / tpr;
	const int top_visible_tile = scroll_offset_y / (tile_height + tile_spacing);
	const int tiles_in_last_row = total_tiles % tpr == 0 ? tpr : total_tiles % tpr;

	switch (key) {
	case KEY_SLASH: {
		search_mode = true;
		typing_search = true;
		return;
	} break;

	case KEY_ENTER: {
		size_t idx = get_tile_idx_from_tile_pos(selected_tile_pos);
		char *tmp = join_dir(paths[idx].str);
		switch (paths[idx].type) {
		case DT_DIR: {
			enter_dir(tmp, idx);
		} break;

		case DT_REG: {
			handle_enter(tmp);
		} break;

		default: break;
		}
	} break;

	case KEY_PERIOD: {
		const double dot_time = GetTime();
		if ((dot_time - last_dot_time) <= DOUBLE_DOT_THRESHOLD) {
			size_t idx = selected_tile_pos.x +
									 selected_tile_pos.y *
									 tpr;

			enter_dir(join_dir(".."), idx);
			last_dot_time = 0.0;
		} else {
			last_dot_time = dot_time;
		}

		return;
	} break;

	case KEY_W: case KEY_UP:
	if (selected_tile_pos.y > 0) {
		selected_tile_pos.y--;
		const int relative_tile_y = selected_tile_pos.y - top_visible_tile;
		if (relative_tile_y == -1) {
			scroll_offset_y -= (float) tile_full_height;
		}
	} break;

	case KEY_A: case KEY_LEFT:
	if (selected_tile_pos.x > 0) {
		selected_tile_pos.x--;
	} break;

	case KEY_S: case KEY_DOWN:
	if (selected_tile_pos.y < total_rows - 1) {
		if (!(selected_tile_pos.y == total_rows - 2
		&& selected_tile_pos.x >= tiles_in_last_row))
		{
			selected_tile_pos.y++;
		}

		const int relative_tile_y = selected_tile_pos.y - top_visible_tile;
		if (tpc == relative_tile_y) {
			scroll_offset_y += (float) tile_full_height;
		}
	} break;

	case KEY_D: case KEY_RIGHT:
	if (IsKeyDown(KEY_LEFT_SHIFT)) {
		delete_mode = true;
		return;
	}

	if (selected_tile_pos.y == total_rows - 1) {
		if (selected_tile_pos.x < tiles_in_last_row - 1) {
			selected_tile_pos.x++;
		}
	} else if (selected_tile_pos.x < tpr - 1) {
		selected_tile_pos.x++;
	} break;
	}
}

INLINE static Vector2 get_text_pos(const Vector2 *tile_pos)
{
	return (Vector2) {
		tile_pos->x + text_padding,
		tile_pos->y + tile_height - font_size - text_padding
	};
}

INLINE static Rectangle get_tile_rect(const Vector2 *tile_pos)
{
	return (Rectangle) {
		.x = tile_pos->x + text_padding,
		.width = tile_width - text_padding * 2,
		.y = tile_pos->y + text_padding,
		.height = tile_height - text_padding
	};
}

static void handle_mouse_input(void)
{
	scroll_offset_y -= GetMouseWheelMove() * scroll_speed;

	const int tpr = get_tiles_per_row();

	if (scroll_offset_y < 0) scroll_offset_y = 0;
	if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;

	const double curr_time = GetTime();
	Vector2 mouse_pos = GetMousePosition();

	for (size_t i = 0; i < paths.size(); ++i) {
		if (paths[i].deleted) continue;

		const int tile_pos_x = i % tpr;
		const int tile_pos_y = i / tpr;

		const Vector2 tile_pos = get_tile_pos(tile_pos_x, tile_pos_y);
		const Rectangle tile_rect = get_tile_rect(&tile_pos);

		if (!CheckCollisionPointRec(mouse_pos, tile_rect)) continue;
		if ((curr_time - last_click_time) <= DOUBLE_CLICK_THRESHOLD
		&& CheckCollisionPointRec(last_click_pos, tile_rect))
		{
			char *tmp = join_dir(paths[i].str);
			switch (paths[i].type) {
			case DT_DIR: {
				enter_dir(tmp, i);
				last_click_time = 0.0;
			} break;

			case DT_REG: {
				handle_enter(tmp);
			} break;

			default: break;
			}
		}

		last_click_pos = mouse_pos;
		last_click_time = curr_time;
		selected_tile_pos.x = tile_pos_x;
		selected_tile_pos.y = tile_pos_y;
		break;
	}
}

static void draw_text_truncated(const char *text,
																Vector2 pos,
																float max_text_width,
																Color color)
{
	const float text_width = MeasureTextEx(font, text, font_size, text_spacing).x;

	if (text_width > max_text_width) {
		int len = strlen(text);
		char truncated[MAX_PATH_SIZE] = {0};
		strncpy(truncated, text, sizeof(truncated));

		while (MeasureTextEx(font, truncated, font_size, text_spacing).x +
					 MeasureTextEx(font, "...", font_size, text_spacing).x > max_text_width
			&& len > 0)
		{
			truncated[--len] = '\0';
		}

		strcat(truncated, "...");
		DrawTextEx(font, truncated, pos, font_size, text_spacing, color);
	} else {
		DrawTextEx(font, text, pos, font_size, text_spacing, color);
	}
}

static void draw_text_boxed_selectable(Font font,
																			 const char *text,
																			 Rectangle rec,
																			 bool word_wrap,
																			 Color tint,
																			 int selectStart,
																			 int selectLength,
																			 Color selectTint,
																			 Color selectBackTint)
{
	int length = TextLength(text);

	float textOffsetY = 0;
	float textOffsetX = 0.0f;

	float scaleFactor = font_size / (float) font.baseSize;

	enum { MEASURE_STATE = 0, DRAW_STATE = 1 };
	int state = word_wrap? MEASURE_STATE : DRAW_STATE;

	int startLine = -1;
	int endLine = -1;
	int lastk = -1;

	for (int i = 0, k = 0; i < length; i++, k++) {
		int codepointByteCount = 0;
		int codepoint = GetCodepoint(&text[i], &codepointByteCount);
		int index = GetGlyphIndex(font, codepoint);

		if (codepoint == 0x3f) codepointByteCount = 1;
		i += (codepointByteCount - 1);

		float glyphWidth = 0;
		if (codepoint != '\n') {
			glyphWidth = (font.glyphs[index].advanceX == 0) ?
				font.recs[index].width*scaleFactor :
				font.glyphs[index].advanceX*scaleFactor;

			if (i + 1 < length) glyphWidth = glyphWidth + text_spacing;
		}

		if (state == MEASURE_STATE) {
			if ((codepoint == ' ') || (codepoint == '\t') || (codepoint == '\n')) endLine = i;
			if ((textOffsetX + glyphWidth) > rec.width) {
				endLine = (endLine < 1)? i : endLine;
				if (i == endLine) endLine -= codepointByteCount;
				if ((startLine + codepointByteCount) == endLine) endLine = (i - codepointByteCount);

				state = !state;
			} else if ((i + 1) == length) {
				endLine = i;
				state = !state;
			}
			else if (codepoint == '\n') state = !state;
			if (state == DRAW_STATE) {
				textOffsetX = 0;
				i = startLine;
				glyphWidth = 0;

				int tmp = lastk;
				lastk = k - 1;
				k = tmp;
			}
		}
		else {
			if (codepoint == '\n') {
				if (!word_wrap) {
					textOffsetY += (font.baseSize + font.baseSize/2)*scaleFactor;
					textOffsetX = 0;
				}
			}
			else {
				if (!word_wrap && ((textOffsetX + glyphWidth) > rec.width)) {
					textOffsetY += (font.baseSize + font.baseSize/2)*scaleFactor;
					textOffsetX = 0;
				}

				if ((textOffsetY + font.baseSize*scaleFactor) > rec.height) break;

				bool isGlyphSelected = false;
				if ((selectStart >= 0)
				&& (k >= selectStart)
				&& (k < (selectStart + selectLength)))
				{
					DrawRectangleRec((Rectangle) {
														 rec.x + textOffsetX - 1,
														 rec.y + textOffsetY,
														 glyphWidth,
														 (float)font.baseSize*scaleFactor
													 },
													 selectBackTint);
					isGlyphSelected = true;
				}

				if ((codepoint != ' ') && (codepoint != '\t')) {
					DrawTextCodepoint(font, codepoint,
														(Vector2){rec.x + textOffsetX, rec.y + textOffsetY},
														font_size,
														isGlyphSelected? selectTint : tint);
				}
			}

			if (word_wrap && (i == endLine)) {
				textOffsetY += (font.baseSize + font.baseSize/2)*scaleFactor;
				textOffsetX = 0;
				startLine = endLine;
				endLine = -1;
				glyphWidth = 0;
				selectStart += lastk - k;
				k = lastk;

				state = !state;
			}
		}

		if ((textOffsetX != 0) || (codepoint != ' ')) textOffsetX += glyphWidth;
	}
}

INLINE static void draw_text_boxed(Font font,
																	 const char *text,
																	 Rectangle rec,
																	 bool word_wrap,
																	 Color tint)
{
	draw_text_boxed_selectable(font, text, rec, word_wrap, tint, 0, 0, WHITE, WHITE);
}

static uint8_t *load_first_frame(const char *file_path, Mat *frame)
{
	VideoCapture cap(file_path);
	if (!cap.isOpened()) {
		eprintf("could not open video file.\n");
		return NULL;
	}

	if (!cap.read(*frame)) {
		eprintf("could not read the first frame.\n");
		return NULL;
	}

	uint8_t *data = (uint8_t*) malloc(frame->rows * frame->cols * 3);
	Mat buf(frame->rows, frame->cols, CV_8UC3, data);

	cvtColor(*frame, buf, COLOR_BGR2RGB);
	buf.release();
	cap.release();
	return data;
}

static void resize_img(Image *img, int tw, int th)
{
	const int w = img->width;
	const int h = img->height;

	const float original_aspect = (float) w / h;
	const float target_aspect = (float) tw / th;

	int nw, nh;
	if (original_aspect > target_aspect) {
		nw = tw;
		nh = tw / original_aspect;
	} else {
		nh = th;
		nw = th * original_aspect;
	}

	ImageResize(img, nw, nh);
}

INLINE static void resize_img_to_size_of_tile(Image *img)
{
	resize_img(img, tile_width	 - text_padding, tile_height - text_padding);
}

static void handle_enter(char *file_path)
{
	char *ext = get_extension(file_path);
	if (is_video(ext) || is_music(ext)) {
		Nob_Cmd cmd = {0};
		nob_cmd_append(&cmd, "mpv", file_path);
		const Nob_Proc proc = nob_cmd_run_async(cmd, true);
		procs.emplace_back(proc);
	} else if (is_image(ext)) {
		Nob_Cmd cmd = {0};
		nob_cmd_append(&cmd, "mpv", "--loop", file_path);
		const Nob_Proc proc = nob_cmd_run_async(cmd, true);
		procs.emplace_back(proc);
	}
}

uint8_t *try_get_album_cover(const char *file_path, size_t *data_size)
{
	MPEG::File file = MPEG::File(file_path);
	ID3v2::Tag *id3v2_tag = file.ID3v2Tag();

	if (!id3v2_tag) return NULL;
	const auto &frames = id3v2_tag->frameListMap()["APIC"];
	if (frames.isEmpty()) return NULL;

	const auto *picture_frame = (ID3v2::AttachedPictureFrame *) (frames.front());
	if (!picture_frame) return NULL;

	const ByteVector &image_data = picture_frame->picture();

	const size_t size = image_data.size();

	*data_size = size;

	uint8_t *data = (uint8_t*) malloc(size);
	memcpy(data, image_data.data(), size);

	return data;
}

static bool get_preview(const char *ext, const char *file_path, Image *img)
{
	if (is_video(ext)) {
		Mat frame = {};
		uint8_t *data = load_first_frame(file_path, &frame);

		img->data = data;
		img->width = frame.cols;
		img->height = frame.rows;
		img->mipmaps = 1;
		img->format = PIXELFORMAT_UNCOMPRESSED_R8G8B8;

		frame.release();
		return true;
	} else if (is_music(ext)) {
		size_t data_size = 0;
		uint8_t *data = try_get_album_cover(file_path, &data_size);

		if (data == NULL || data_size == 0) return false;

		int comp = 0;
		img->data = stbi_load_from_memory(data, data_size, &img->width, &img->height, &comp, 0);
		if (img->data == NULL) {
			free(data);
			return false;
		}

		if			(comp == 1)	img->format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;
		else if (comp == 2)	img->format = PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA;
		else if (comp == 3)	img->format = PIXELFORMAT_UNCOMPRESSED_R8G8B8;
		else if (comp == 4)	img->format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
		else {
			free(data);
			return false;
		}

		img->mipmaps = 1;
		return true;
	} else if (is_image(ext)) {
		*img = LoadImage(file_path);
		return true;
	}

	return false;
}

// fill img map with placeholder textures
static void fill_img_map(void)
{
	Image scaled_img = placeholder_src;
	scaled_img.data = copy_img_data(&placeholder_src);

	resize_img_to_size_of_tile(&scaled_img);

	img_value_t img = {
		.src_img = placeholder_src,
		.scaled_img = scaled_img,
		.is_placeholder = true,
		.loaded_texture = LoadTextureFromImage(scaled_img)
	};

	for (const auto &path_ino: paths) {
		hmput(img_map, path_ino.ino, img);
	}
}

INLINE static bool tile_is_match(size_t tile_idx)
{
	for (const auto &idx: matched_idxs) {
		if (tile_idx == idx) return true;
	}
	return false;
}

static void render_files(void)
{
	const int tpr = get_tiles_per_row();
	for (size_t i = 0; i < paths.size(); ++i) {
		if (paths[i].deleted) continue;

		const int tile_pos_x = i % tpr;
		const int tile_pos_y = i / tpr;

		const Vector2 tile_pos = get_tile_pos(tile_pos_x, tile_pos_y);

		if (tile_pos.y + tile_height < 0
		|| tile_pos.y > GetScreenHeight())
		{
			continue;
		}

		Color tile_color = TILE_COLOR;

		if (selected_tile_pos.x == tile_pos_x
		&&  selected_tile_pos.y == tile_pos_y)
		{
			tile_color = CLICKED_TILE_COLOR;
		} else if (tile_is_match(i)) {
			tile_color = MATCHED_TILE_COLOR;
		}

		DrawRectangleV(tile_pos, (Vector2){tile_width, tile_height}, tile_color);

		if (selected_tile_pos.x == tile_pos_x
		&&	selected_tile_pos.y == tile_pos_y)
		{
			const Rectangle tile_rect =	get_tile_rect(&tile_pos);
			draw_text_boxed(font, paths[i].str, tile_rect, true, WHITE);
		} else {
			const size_t idx = hmgeti(img_map, paths[i].ino);
			Texture2D texture = {0};
			if (img_map[idx].value.loaded_texture) {
				texture = *img_map[idx].value.loaded_texture;
			} else if (img_map[idx].value.is_placeholder) {
				texture = placeholder_texture;
			} else {
				texture = LoadTextureFromImage(img_map[idx].value.scaled_img);
				img_map[idx].value.loaded_texture = texture;
			}

			const float centered_x = tile_pos.x			+
															 text_padding		+
															 (tile_width		-
																texture.width -
																2 * text_padding) / 2;

			const float centered_y = tile_pos.y			 +
															 text_padding		 +
															 (tile_height		 -
																texture.height -
																2 * text_padding) / 2;

			DrawTexture(texture, centered_x, centered_y, WHITE);

			const Vector2 text_pos = get_text_pos(&tile_pos);
			draw_text_truncated(paths[i].str, text_pos, tile_width - 2 * text_padding, WHITE);
		}
	}
}

INLINE static Image scale_img(Image src_img)
{
	Image scaled_img = src_img;
	scaled_img.data = copy_img_data(&src_img);
	resize_img_to_size_of_tile(&scaled_img);
	return scaled_img;
}

static void handle_dropped_files(void)
{
	if (!IsFileDropped()) return;

	FilePathList files = LoadDroppedFiles();

	for (size_t i = 0; i < files.count; i++) {
		struct stat info = {0};
		stat(files.paths[i], &info);

		char *top_level_file_path = get_top_file_path(files.paths[i]);

		scratch_buffer_clear();
		scratch_buffer_append(top_level_file_path);
		char *top_level_file_path_copy = scratch_buffer_copy();

		Nob_Cmd cmd = {0};
		nob_cmd_append(&cmd, "cp", files.paths[i], top_level_file_path);

		uint8_t type = 0;
		if (S_ISDIR(info.st_mode)) {
			type = DT_DIR;
			nob_cmd_append(&cmd, "-r");
		} else if (S_ISREG(info.st_mode)) {
			type = DT_REG;
		} else {
			eprintf("naah bruh");
			continue;
		}

		Nob_Proc proc = nob_cmd_run_async(cmd, true);
		procs.emplace_back(proc);

		path_t path = {
			.ino = (size_t) info.st_ino,
			.str = top_level_file_path_copy,
			.type = type,
			.abs = true,
			.deleted = false,
		};

		paths.emplace_back(path);

		scratch_buffer_clear();
		scratch_buffer_append(files.paths[i]);

		path.str = scratch_buffer_copy();

		to_load.emplace_back(path);
		idle_flag = false;

		img_value_t value = {
			.src_img = placeholder_src,
			.scaled_img = scale_img(placeholder_scaled),
			.is_placeholder = true,
			.loaded_texture = std::nullopt,
		};

		hmput(img_map, path.ino, value);
	}

	UnloadDroppedFiles(files);
}

static void load_preview(size_t i, path_t path_ino, size_t size)
{
	bool prev_scale_flag = new_scale_flag;
	if (i == size - 1) {
		if (new_scale_flag) new_scale_flag = false;
		idle_flag = true;
	}

	int idx = hmgeti(img_map, path_ino.ino);

	if (prev_scale_flag) {
		img_map_t *p = hmgetp(img_map, path_ino.ino);

		if (img_map[idx].value.is_placeholder) {
			UnloadImage(placeholder_scaled);
			to_unload.emplace_back(placeholder_texture);

			placeholder_scaled = scale_img(placeholder_src);
			placeholder_resized = true;
			return;
		}

		if (p->value.scaled_img.data != NULL) {
			UnloadImage(p->value.scaled_img);
		}

		p->value.scaled_img = scale_img(p->value.src_img);

		if (p->value.loaded_texture) {
			to_unload.emplace_back(*p->value.loaded_texture);
		}

		p->value.loaded_texture = std::nullopt;
		return;
	} else if (!img_map[idx].value.is_placeholder) return;

	scratch_buffer_clear();
	if (!path_ino.abs) {
		scratch_buffer_append(curr_dir);
		scratch_buffer_append_char('/');
	}

	scratch_buffer_append(path_ino.str);

	char *file_path = scratch_buffer_to_string();
	char *ext = get_extension(path_ino.str);

	if (ext == NULL || *ext == '\0') return;

	Image src_img = {0};
	if (!get_preview(ext, file_path, &src_img)) return;

	Image scaled_img = src_img;
	scaled_img.data = copy_img_data(&src_img);

	resize_img_to_size_of_tile(&scaled_img);

	img_value_t value = {
		.src_img = src_img,
		.scaled_img = scaled_img,
		.is_placeholder = false,
		.loaded_texture = std::nullopt,
	};

	hmput(img_map, path_ino.ino, value);
	return;
}

static void load_previews(void)
{
	while (!stop_flag) {
		if (idle_flag) continue;

		for (int i = to_load.size() - 1; i >= 0; --i) {
			if (idle_flag) break;
			if (to_load[i].deleted) continue;
			load_preview(i, to_load[i], to_load.size());
			to_load.pop_back();
		}

		for (size_t i = 0; i < paths.size(); ++i) {
			if (idle_flag) break;
			if (paths[i].deleted) continue;
			load_preview(i, paths[i], paths.size());
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(PREVIEW_LOADER_SLEEP_TIME));
	}
}

INLINE static uint8_t is_dir(const char *file_path)
{
	struct stat info = {0};
	stat(file_path, &info);
	return S_ISDIR(info.st_mode);
}

int main(const int argc, char *argv[])
{
	SetTargetFPS(60);
	SetConfigFlags(FLAG_WINDOW_RESIZABLE);
	InitWindow(1000, 600, "fe");
	SetExitKey(0);

	font = LoadFontEx(FONT_PATH, font_size, NULL, 0);

	if (argc > 1 &&	is_dir(argv[1])) {
		curr_dir = argv[1];
	}

	memory_init(3);

	read_dir();

	placeholder_src = LoadImage(PLACEHOLDER_PATH);
	placeholder_texture =	LoadTextureFromImage(placeholder_src);

	fill_img_map();

	std::thread preview_loader = std::thread(load_previews);

	while (!WindowShouldClose()) {
		handle_dropped_files();
		BeginDrawing();
			ClearBackground(BACKGROUND_COLOR);
			render_files();
			handle_keyboard_input();
			handle_mouse_input();
		EndDrawing();
	}

	for (const auto& proc: procs) {
		nob_proc_kill(proc, true);
	}

	stop_flag = true;
	preview_loader.join();

	for (long i = 0; i < hmlen(img_map); ++i) {
		if (!img_map[i].value.is_placeholder) {
			UnloadImage(img_map[i].value.scaled_img);
			UnloadImage(img_map[i].value.src_img);
		}

		if (img_map[i].value.loaded_texture) {
			UnloadTexture(*img_map[i].value.loaded_texture);
		}
	}

	for (const auto &texture: to_unload) {
		UnloadTexture(texture);
	}

	UnloadImage(placeholder_src);
	UnloadTexture(placeholder_texture);

	memory_release();
	hmfree(img_map);
	UnloadFont(font);
	CloseWindow();
	return 0;
}

/* TODO:
	3. Implement auto-completion in the search mode
	12. Fix resize of placeholders of drag & dropped files
*/
