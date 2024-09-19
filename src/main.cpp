#include <time.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
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

// only for nob_cmd_run_async
#include "nob.h"

#include <raylib.h>

#include <opencv2/opencv.hpp>
#include <opencv2/imgcodecs.hpp>

using namespace cv;

#define streq(s1, s2) (strcmp(s1, s2) == 0)
#define eprintf(...) fprintf(stderr, __VA_ARGS__)

#define TILE_COLOR DARKGRAY
#define BACKGROUND_COLOR ((Color) {24, 24, 24, 255})
#define CLICKED_TILE_COLOR ((Color) {40, 40, 40, 255})
#define SEARCH_WINDOW_BACKGROUND_COLOR ((Color) {65, 65, 65, 215})

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

static int tile_width     = DEFAULT_TILE_WIDTH;
static int tile_height    = DEFAULT_TILE_HEIGHT;
static int tile_spacing   = DEFAULT_TILE_SPACING;
static int text_padding   = DEFAULT_TEXT_PADDING;
static int font_size      = DEFAULT_FONT_SIZE;
static float text_spacing = DEFAULT_TEXT_SPACING;
static float scroll_speed = DEFAULT_SCROLL_SPEED;

#define tile_full_height (tile_height + tile_spacing)

static bool search_mode = false;
static char search_string[MAX_PATH_SIZE] = {0};
static size_t search_string_size = 0;

#define key_is_printable(key) (key >= 39 && key <= 96)
#define key_is_alpha(key) (key >= KEY_A && key <= KEY_Z)

typedef enum {
	FILE_POISONED = 0,
	FILE_REGULAR,
	FILE_DIRECTORY,
	FILE_SYMLINK,
	FILE_OTHER,
} File_Type;

static char *curr_dir = ".";

static Font font = {0};

#define DOUBLE_CLICK_THRESHOLD 0.3f
static Vector2 last_click_pos = {0};
static double last_click_time = 0.0;

typedef struct {int x, y;} Vector2i;
static float last_scroll_offset_y = 0.0;
static Vector2i last_selected_tile_pos = {0};
static Vector2i last_selected_tile_pos_before_entering_dir = {0};

static std::vector<Nob_Proc> procs = {};

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

typedef struct { char *str; size_t ino; } path_ino_t;
std::vector<path_ino_t> paths = {};

// in millis
#define PREVIEW_LOADER_SLEEP_TIME 555

static bool stop_flag = false;
static bool idle_flag = false;
static bool new_scale_flag = false;
static bool resizing_now_flag = false;

static void resize_img(Image *image, int tw, int th);
static bool load_preview(const char *ext, const char *file_path, Image *img);

INLINE int pixel_format_to_amount_of_bytes(int pixel_format)
{
	switch (pixel_format) {
	case PIXELFORMAT_UNCOMPRESSED_R8G8B8:	return 3;
	default: return 4;
	}
}

INLINE void *copy_img_data(const Image *img)
{
	const size_t data_size = img->width *
													 img->height *
													 pixel_format_to_amount_of_bytes(img->format);

	uint8_t *data = (uint8_t *) malloc(data_size);
	uint8_t *original_data = (uint8_t *) img->data;
	memcpy(data, original_data, data_size);
	return data;
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

	idle_flag = false;
	new_scale_flag = true;

	if (placeholder_resized) {
		UnloadTexture(placeholder_texture);
		placeholder_texture = LoadTextureFromImage(placeholder_scaled);
		placeholder_resized = false;
	}
}

static void read_dir(void)
{
	DIR *dir = opendir(curr_dir);
	if (dir == NULL) {
		eprintf("could not open directory %s: %s", curr_dir, strerror(errno));
		if (dir) closedir(dir);
		assert(0 && "unreachable");
	}

	errno = 0;
	struct dirent *ent = readdir(dir);
	while (ent != NULL) {
		if (strcmp(ent->d_name, ".") != 0) {
			char *file_path = str_copy(ent->d_name, strlen(ent->d_name));
			size_t ino = (size_t) ent->d_ino;
			paths.emplace_back((path_ino_t) {file_path, ino});
		}
		ent = readdir(dir);
	}

	if (errno != 0) {
		eprintf("could not read directory %s: %s", curr_dir, strerror(errno));
		if (dir) closedir(dir);
		assert(0 && "unreachable");
	}

	closedir(dir);
}

File_Type get_file_type(const char *path)
{
	struct stat statbuf;
	if (stat(path, &statbuf) < 0) {
		eprintf("could not get stat of %s: %s", path, strerror(errno));
		return FILE_POISONED;
	}

	switch (statbuf.st_mode & S_IFMT) {
		case S_IFDIR:	 return FILE_DIRECTORY;
		case S_IFREG:	 return FILE_REGULAR;
		case S_IFLNK:	 return FILE_SYMLINK;
		default:			 return FILE_OTHER;
	}
}

INLINE size_t get_tiles_count(void)
{
	return paths.size();
}

INLINE int get_tiles_per_col(void)
{
	return GetScreenHeight() / (tile_height + tile_spacing);
}

INLINE int get_tiles_per_row(void)
{
	return GetScreenWidth() / (tile_width + tile_spacing);
}

static char *enter_dir(const char *dir_path)
{
	scratch_buffer_clear();
	scratch_buffer_append(curr_dir);
	scratch_buffer_append_char('/');
	scratch_buffer_append(dir_path);
	return scratch_buffer_copy();
}

static void preserve_tile_pos(size_t tile_idx)
{
	if (streq(paths[tile_idx].str, "..")) {
		scroll_offset_y = last_scroll_offset_y;
		last_selected_tile_pos.x = last_selected_tile_pos_before_entering_dir.x;
		last_selected_tile_pos.y = last_selected_tile_pos_before_entering_dir.y;
	} else {
		last_selected_tile_pos_before_entering_dir.x = last_selected_tile_pos.x;
		last_selected_tile_pos_before_entering_dir.y = last_selected_tile_pos.y;
		memset(&last_selected_tile_pos, 0, sizeof(last_selected_tile_pos));
		last_scroll_offset_y = scroll_offset_y;
		scroll_offset_y = 0.0;
	}
}

INLINE Vector2 get_tile_pos(size_t tile_pos_x, size_t tile_pos_y)
{
	return (Vector2) {
		tile_pos_x * (tile_width + tile_spacing) + tile_spacing,
		tile_pos_y * (tile_height + tile_spacing) + tile_spacing - scroll_offset_y
	};
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

void handle_enter(char *file_path)
{
	char *ext = get_extension(file_path);
	if (streq(ext, "mp4")
	||	streq(ext, "mov")
	||	streq(ext, "mkv"))
	{
		Nob_Cmd cmd = {0};
		printf("FILE_PATH: %s\n", file_path);
		nob_cmd_append(&cmd, "mpv", file_path);
		const Nob_Proc proc = nob_cmd_run_async(cmd, true);
		procs.emplace_back(proc);
	} else if (streq(ext, "png")
				 ||	 streq(ext, "jpg")
				 ||	 streq(ext, "jpeg"))
	{
		Nob_Cmd cmd = {0};
		nob_cmd_append(&cmd, "mpv", "--loop", file_path);
		const Nob_Proc proc = nob_cmd_run_async(cmd, true);
		procs.emplace_back(proc);
	}
}

void handle_keyboard_input(void)
{
	const int tpr = get_tiles_per_row();

	if (search_mode) {
		const int w = GetScreenWidth();
		const int h = GetScreenHeight();
		const int rh = SEARCH_TEXT_HEIGHT + SEARCH_TEXT_SPACING * 2;

		DrawRectangle(0, h - rh, w, rh, SEARCH_WINDOW_BACKGROUND_COLOR);

		if (search_string_size > 0) {
			DrawTextEx(font,
								 search_string,
								 (Vector2) {
									 SEARCH_TEXT_SPACING,
									 h - SEARCH_TEXT_HEIGHT,
								 },
								 font_size,
								 text_spacing,
								 RAYWHITE);
		}

		int key = GetKeyPressed();
		if (key == KEY_ENTER) {
			search_mode = false;
			if (search_string_size < 1) return;

			for (size_t i = 0; i < paths.size(); ++i) {
				scratch_buffer_clear();
				scratch_buffer_append(paths[i].str);

				for (char *p = scratch_buffer.str; p != NULL && *p != '\0'; *p = tolower(*p), p++);

				if (strstr(scratch_buffer.str, search_string) != NULL) {
					last_selected_tile_pos.x = i % tpr;
					last_selected_tile_pos.y = i / tpr;

					const int tile_row = last_selected_tile_pos.y;
					const int visible_rows = GetScreenHeight() / (tile_height + tile_spacing);
					const int first_visible_row = scroll_offset_y / (tile_height + tile_spacing);
					const int last_visible_row = first_visible_row + visible_rows - 1;

					if (tile_row < first_visible_row || tile_row > last_visible_row) {
						scroll_offset_y = tile_row * (tile_height + tile_spacing);
					}

					break;
				}
			}

			memset(search_string, 0, search_string_size);
			search_string_size = 0;
		} else if (key == KEY_SPACE || key_is_printable(key)) {
			if (search_string_size == MAX_PATH_SIZE) return;
			if (key_is_alpha(key)) {
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

	switch (GetKeyPressed()) {
	case KEY_SLASH: {
		search_mode = true;
		return;
	} break;

	case KEY_ENTER: {
		size_t idx = last_selected_tile_pos.x;
		if (last_selected_tile_pos.y > 0) {
			idx += last_selected_tile_pos.y * tpr;
		}

		char *tmp = enter_dir(paths[idx].str);
		switch (get_file_type(tmp)) {
		case FILE_DIRECTORY: {
			curr_dir = tmp;
			idle_flag = true;
			paths.clear();
			read_dir();
			idle_flag = false;
			preserve_tile_pos(idx);
		} break;

		case FILE_REGULAR: {
			handle_enter(tmp);
		} break;

		default: break;
		}
	} break;

	case KEY_W: case KEY_UP: if (last_selected_tile_pos.y > 0) {
		last_selected_tile_pos.y--;
		const int relative_tile_y = last_selected_tile_pos.y - top_visible_tile;
		if (relative_tile_y == -1) {
			scroll_offset_y -= (float) tile_full_height;
		}
	} break;

	case KEY_A: case KEY_LEFT: if (last_selected_tile_pos.x > 0) {
		last_selected_tile_pos.x--;
	} break;

	case KEY_S: case KEY_DOWN:
	if (last_selected_tile_pos.y < total_rows - 1) {
		if (!(last_selected_tile_pos.y == total_rows - 2
		&& last_selected_tile_pos.x >= tiles_in_last_row))
		{
			last_selected_tile_pos.y++;
		}

		const int relative_tile_y = last_selected_tile_pos.y - top_visible_tile;
		if (tpc == relative_tile_y) {
			scroll_offset_y += (float) tile_full_height;
		}
	} break;

	case KEY_D: case KEY_RIGHT: if (last_selected_tile_pos.y == total_rows - 1) {
		if (last_selected_tile_pos.x < tiles_in_last_row - 1) {
			last_selected_tile_pos.x++;
		}
	} else if (last_selected_tile_pos.x < tpr - 1) {
		last_selected_tile_pos.x++;
	} break;
	}
}

INLINE Vector2 get_text_pos(const Vector2 *tile_pos)
{
	return (Vector2) {
		tile_pos->x + text_padding,
		tile_pos->y + tile_height - font_size - text_padding
	};
}

INLINE Rectangle get_tile_rect(const Vector2 *tile_pos)
{
	return (Rectangle) {
		.x = tile_pos->x + text_padding,
		.width = tile_width - text_padding * 2,
		.y = tile_pos->y + text_padding,
		.height = tile_height - text_padding
	};
}

void handle_mouse_input(void)
{
	scroll_offset_y -= GetMouseWheelMove() * scroll_speed;

	const int tpr = get_tiles_per_row();

	if (scroll_offset_y < 0) scroll_offset_y = 0;
	if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;

	const double curr_time = GetTime();
	Vector2 mouse_pos = GetMousePosition();

	for (size_t i = 0; i < paths.size(); ++i) {
		const int tile_pos_x = i % tpr;
		const int tile_pos_y = i / tpr;

		const Vector2 tile_pos = get_tile_pos(tile_pos_x, tile_pos_y);
		const Rectangle tile_rect = get_tile_rect(&tile_pos);

		if (!CheckCollisionPointRec(mouse_pos, tile_rect)) continue;
		if ((curr_time - last_click_time) <= DOUBLE_CLICK_THRESHOLD
		&& CheckCollisionPointRec(last_click_pos, tile_rect))
		{
			char *tmp = enter_dir(paths[i].str);
			switch (get_file_type(tmp)) {
			case FILE_DIRECTORY: {
				curr_dir = tmp;
				paths.clear();
				idle_flag = true;
				read_dir();
				idle_flag = false;
				preserve_tile_pos(i);
			} break;

			case FILE_REGULAR: {
				handle_enter(tmp);
			} break;

			default: break;
			}
		}

		last_click_pos = mouse_pos;
		last_click_time = curr_time;
		last_selected_tile_pos.x = tile_pos_x;
		last_selected_tile_pos.y = tile_pos_y;
		break;
	}
}

void draw_truncated_text(const char *text,
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

	float scaleFactor = font_size/(float)font.baseSize;

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
		if (codepoint != '\n')
		{
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

				if ((codepoint != ' ') && (codepoint != '\t'))
				{
					DrawTextCodepoint(font, codepoint,
														(Vector2){ rec.x + textOffsetX, rec.y + textOffsetY },
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

INLINE void draw_text_boxed(Font font,
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

static bool load_preview(const char *ext, const char *file_path, Image *img)
{
	if (strcmp(ext, "mp4") == 0) {
		// clock_t start = clock();
		// clock_t end = clock();
		// double elapsed = (double) (end - start) / CLOCKS_PER_SEC;

		Mat frame = {};
		uint8_t *data = load_first_frame(file_path, &frame);

		img->data = data;
		img->width = frame.cols;
		img->height = frame.rows;
		img->mipmaps = 1;
		img->format = PIXELFORMAT_UNCOMPRESSED_R8G8B8;

		frame.release();
		return true;
	} else if (strcmp(ext, "png") == 0) {
		*img = LoadImage(file_path);
		return true;
	}

	return false;
}

static void init_img_map(void)
{
	Image scaled_img = placeholder_src;
	scaled_img.data = copy_img_data(&placeholder_src);
	resize_img(&scaled_img,
						 tile_width - text_padding,
						 tile_height - text_padding);

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

void render_files(void)
{
	if (resizing_now_flag) return;

	const int tpr = get_tiles_per_row();
	for (size_t i = 0; i < paths.size(); ++i) {
		const int tile_pos_x = i % tpr;
		const int tile_pos_y = i / tpr;

		const Vector2 tile_pos = get_tile_pos(tile_pos_x, tile_pos_y);

		if (tile_pos.y + tile_height < 0
		|| tile_pos.y > GetScreenHeight())
		{
			continue;
		}

		Color tile_color = TILE_COLOR;
		if (last_selected_tile_pos.x == tile_pos_x
		&&  last_selected_tile_pos.y == tile_pos_y)
		{
			tile_color = CLICKED_TILE_COLOR;
		}

		DrawRectangleV(tile_pos, (Vector2){tile_width, tile_height}, tile_color);

		if (last_selected_tile_pos.x == tile_pos_x
		&&	last_selected_tile_pos.y == tile_pos_y)
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
			draw_truncated_text(paths[i].str, text_pos, tile_width - 2 * text_padding, WHITE);
		}
	}
}

void load_previews(void)
{
	while (!stop_flag) {
		if (idle_flag) continue;

		for (size_t i = 0; i < paths.size(); ++i) {
			if (idle_flag) break;

			bool prev_scale_flag = new_scale_flag;
			if (i == paths.size() - 1) {
				if (new_scale_flag) new_scale_flag = false;
				idle_flag = true;
			}

			const path_ino_t path_ino = paths[i];

			int idx = hmgeti(img_map, path_ino.ino);

			if (prev_scale_flag) {
				resizing_now_flag = true;

				img_map_t *p = hmgetp(img_map, path_ino.ino);

				if (img_map[idx].value.is_placeholder) {
					UnloadImage(placeholder_scaled);

					Image scaled_placeholder = placeholder_src;
					scaled_placeholder.data = copy_img_data(&placeholder_src);

					resize_img(&scaled_placeholder,
										 tile_width - text_padding,
										 tile_height - text_padding);

					placeholder_scaled = scaled_placeholder;

					resizing_now_flag = false;
					placeholder_resized = true;
					continue;
				}

				Image new_scaled_img = p->value.src_img;
				new_scaled_img.data = copy_img_data(&p->value.src_img);

				resize_img(&new_scaled_img,
									 tile_width - text_padding,
									 tile_height - text_padding);

				p->value.scaled_img = new_scaled_img;
				resizing_now_flag = false;
				continue;
			} else if (!img_map[idx].value.is_placeholder) continue;

			scratch_buffer_clear();
			scratch_buffer_append(curr_dir);
			scratch_buffer_append_char('/');
			scratch_buffer_append(path_ino.str);

			char *file_path = scratch_buffer_to_string();
			char *ext = get_extension(path_ino.str);

			if (ext == NULL || *ext == '\0') continue;

			Image src_img = {0};
			if (!load_preview(ext, file_path, &src_img)) continue;

			Image scaled_img = src_img;
			scaled_img.data = copy_img_data(&src_img);

			resize_img(&scaled_img,
								 tile_width - text_padding,
								 tile_height - text_padding);

			img_value_t value = {
				.src_img = src_img,
				.scaled_img = scaled_img,
				.is_placeholder = false,
				.loaded_texture = std::nullopt,
			};

			hmput(img_map, path_ino.ino, value);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(PREVIEW_LOADER_SLEEP_TIME));
	}
}

int main(const int argc, char *argv[])
{
	SetTargetFPS(60);
	SetConfigFlags(FLAG_WINDOW_RESIZABLE);
	InitWindow(1000, 600, "fe");

	font = LoadFontEx(FONT_PATH, font_size, NULL, 0);

	if (argc > 1
	&&	get_file_type(argv[1]) == FILE_DIRECTORY)
	{
		curr_dir = argv[1];
	}

	memory_init(3);

	read_dir();

	placeholder_src = LoadImage(PLACEHOLDER_PATH);
	placeholder_texture =	LoadTextureFromImage(placeholder_src);

	init_img_map();

	std::thread preview_loader = std::thread(load_previews);

	while (!WindowShouldClose()) {
		BeginDrawing();
			ClearBackground(BACKGROUND_COLOR);
			render_files();
			handle_keyboard_input();
			handle_mouse_input();
		EndDrawing();
	}

	stop_flag = true;
	preview_loader.join();

	for (long i = 0; i < hmlen(img_map); ++i) {
		if (img_map[i].value.loaded_texture) {
			UnloadTexture(*img_map[i].value.loaded_texture);
		}
	}

	for (const auto& proc: procs) {
		nob_proc_wait(proc, true);
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
	2. Implement search
	3. Implement auto-completion in the search mode
*/
