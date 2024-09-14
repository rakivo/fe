#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define NOB_IMPLEMENTATION
#include "nob.h"

#define SCRATCH_BUFFER_IMPLEMENTATION
#include "scratch_buffer.h"

#include <raylib.h>

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

#define TILE_COLOR DARKGRAY
#define BACKGROUND_COLOR ((Color) {24, 24, 24, 255})
#define CLICKED_TILE_COLOR ((Color) { 40, 40, 40, 255 })

#define MAX_PATH_SIZE (256 + 1)

#define RESOURCES_DIR "resources"
#define FONT_PATH (RESOURCES_DIR "/" "Iosevka-Regular.ttf")

#define DEFAULT_TILE_WIDTH   120
#define DEFAULT_TILE_HEIGHT  125
#define DEFAULT_TILE_SPACING 21
#define DEFAULT_TEXT_PADDING 10
#define DEFAULT_FONT_SIZE    20
#define DEFAULT_TEXT_SPACING 2.0f

static int tile_width     = DEFAULT_TILE_WIDTH;
static int tile_height    = DEFAULT_TILE_HEIGHT;
static int tile_spacing   = DEFAULT_TILE_SPACING;
static int text_padding   = DEFAULT_TEXT_PADDING;
static int font_size      = DEFAULT_FONT_SIZE;
static float text_spacing = DEFAULT_TEXT_SPACING;

static char *curr_dir = ".";
static Nob_File_Paths paths = {0};

static Font font = {0};

#define DOUBLE_CLICK_THRESHOLD 0.3f
static Vector2 last_click_pos = {0};
static double last_click_time = 0.0;

#define SCROLL_SPEED 50.0f
static float scroll_offset_y = 0.0;

#define SCALE_THRESHOLD 0.1f
static double last_scale_time = 0.0;

#define MIN_SCALE 0.5f
#define MAX_SCALE 3.0f
#define SCALE_STEP 0.25f

static float scale = 1.0;

static void set_new_scale(float new_scale)
{
	scale = new_scale;
	tile_width   = DEFAULT_TILE_WIDTH   * scale;
	tile_height  = DEFAULT_TILE_HEIGHT  * scale;
	tile_spacing = DEFAULT_TILE_SPACING * scale;
	text_padding = DEFAULT_TEXT_PADDING * scale;
	font_size    = DEFAULT_FONT_SIZE    * scale;
	text_spacing = DEFAULT_TEXT_SPACING * scale;
	UnloadFont(font);
	font = LoadFontEx(FONT_PATH, font_size, NULL, 0);
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
			nob_da_append(&paths, nob_temp_strdup(ent->d_name));
		}
		ent = readdir(dir);
	}
	
	if (errno != 0) {
		eprintf("could not read directory %s: %s", curr_dir, strerror(errno));
		if (dir) closedir(dir);
		assert(0 && "unreachable");
	}
}

static char *enter_dir(const char *dir_path)
{
	scratch_buffer_clear();
	scratch_buffer_append(curr_dir);
	scratch_buffer_append_char('/');
	scratch_buffer_append(dir_path);
	return scratch_buffer_copy();
}

void handle_keyboard_input(void)
{
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
}

static struct {
	int x, y; 
} last_clicked_tile_pos = {-1, -1};

void handle_mouse_input(void)
{
	scroll_offset_y -= GetMouseWheelMove() * SCROLL_SPEED;

	const int tiles_per_row = GetScreenWidth() / (tile_width + tile_spacing);

	const float max_scroll_offset = (paths.count / tiles_per_row) *
																	(tile_height + tile_spacing) -
																	GetScreenHeight();

	if (scroll_offset_y < 0) scroll_offset_y = 0;
	if (scroll_offset_y > max_scroll_offset) scroll_offset_y = max_scroll_offset;
	if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;

	const double curr_time = GetTime();
	Vector2 mouse_pos = GetMousePosition();
	mouse_pos.y += scroll_offset_y;
	
	for (size_t i = 0; i < paths.count; ++i) {
	  const int tile_pos_x = i % tiles_per_row;
	  const int tile_pos_y = i / tiles_per_row;
	
	  const Vector2 tile_pos = {
      tile_pos_x * (tile_width + tile_spacing) + tile_spacing, 
      tile_pos_y * (tile_height + tile_spacing) + tile_spacing
	  };
	
	  const Rectangle tile_rect = {tile_pos.x, tile_pos.y, tile_width, tile_height};

	  if (CheckCollisionPointRec(mouse_pos, tile_rect)) {
			if ((curr_time - last_click_time) <= DOUBLE_CLICK_THRESHOLD
			&& CheckCollisionPointRec(last_click_pos, tile_rect))
	  	{
				char *tmp = enter_dir(paths.items[i]);
				if (nob_get_file_type(tmp) == NOB_FILE_DIRECTORY) {
					curr_dir = tmp;
					paths.count = 0;
					scroll_offset_y = 0.0;
					read_dir();
				}
	  	  last_click_time = 0.0;
				last_clicked_tile_pos.x = -1;
				last_clicked_tile_pos.y = -1;
	  	  break;
	  	}
	
	  	last_click_time = curr_time;
	  	last_click_pos = mouse_pos;
			last_clicked_tile_pos.x = tile_pos_x;
		 	last_clicked_tile_pos.y = tile_pos_y;
	  	break;
		}
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
	int length = TextLength(text);  // Total length in bytes of the text, scanned by codepoints in loop

	float textOffsetY = 0;		  // Offset between lines (on line break '\n')
	float textOffsetX = 0.0f;	   // Offset X to next character to draw

	float scaleFactor = font_size/(float)font.baseSize;	 // Character rectangle scaling factor

	// Word/character wrapping mechanism variables
	enum { MEASURE_STATE = 0, DRAW_STATE = 1 };
	int state = word_wrap? MEASURE_STATE : DRAW_STATE;

	int startLine = -1;		 // Index where to begin drawing (where a line begins)
	int endLine = -1;		   // Index where to stop drawing (where a line ends)
	int lastk = -1;			 // Holds last value of the character position

	for (int i = 0, k = 0; i < length; i++, k++)
	{
		// Get next codepoint from byte string and glyph index in font
		int codepointByteCount = 0;
		int codepoint = GetCodepoint(&text[i], &codepointByteCount);
		int index = GetGlyphIndex(font, codepoint);

		// NOTE: Normally we exit the decoding sequence as soon as a bad byte is found (and return 0x3f)
		// but we need to draw all of the bad bytes using the '?' symbol moving one byte
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

		// NOTE: When word_wrap is ON we first measure how much of the text we can draw before going outside of the rec container
		// We store this info in startLine and endLine, then we change states, draw the text between those two variables
		// and change states again and again recursively until the end of the text (or until we get outside of the container).
		// When word_wrap is OFF we don't need the measure state so we go to the drawing state immediately
		// and begin drawing on the next line before we can get outside the container.
		if (state == MEASURE_STATE)
		{
			// TODO: There are multiple types of spaces in UNICODE, maybe it's a good idea to add support for more
			// Ref: http://jkorpela.fi/chars/spaces.html
			if ((codepoint == ' ') || (codepoint == '\t') || (codepoint == '\n')) endLine = i;

			if ((textOffsetX + glyphWidth) > rec.width)
			{
				endLine = (endLine < 1)? i : endLine;
				if (i == endLine) endLine -= codepointByteCount;
				if ((startLine + codepointByteCount) == endLine) endLine = (i - codepointByteCount);

				state = !state;
			}
			else if ((i + 1) == length)
			{
				endLine = i;
				state = !state;
			}
			else if (codepoint == '\n') state = !state;

			if (state == DRAW_STATE)
			{
				textOffsetX = 0;
				i = startLine;
				glyphWidth = 0;

				// Save character position when we switch states
				int tmp = lastk;
				lastk = k - 1;
				k = tmp;
			}
		}
		else
		{
			if (codepoint == '\n')
			{
				if (!word_wrap)
				{
					textOffsetY += (font.baseSize + font.baseSize/2)*scaleFactor;
					textOffsetX = 0;
				}
			}
			else
			{
				if (!word_wrap && ((textOffsetX + glyphWidth) > rec.width))
				{
					textOffsetY += (font.baseSize + font.baseSize/2)*scaleFactor;
					textOffsetX = 0;
				}

				// When text overflows rectangle height limit, just stop drawing
				if ((textOffsetY + font.baseSize*scaleFactor) > rec.height) break;

				// Draw selection background
				bool isGlyphSelected = false;
				if ((selectStart >= 0) 
				&& (k >= selectStart) 
				&& (k < (selectStart + selectLength)))
				{
					DrawRectangleRec((Rectangle){ 
														 rec.x + textOffsetX - 1, 
														 rec.y + textOffsetY, 
														 glyphWidth, 
														 (float)font.baseSize*scaleFactor 
													 }, 
													 selectBackTint);
					isGlyphSelected = true;
				}

				// Draw current character glyph
				if ((codepoint != ' ') && (codepoint != '\t'))
				{
					DrawTextCodepoint(font, codepoint, 
														(Vector2){ rec.x + textOffsetX, rec.y + textOffsetY }, 
														font_size, 
														isGlyphSelected? selectTint : tint);
				}
			}

			if (word_wrap && (i == endLine))
			{
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

inline static void draw_text_boxed(Font font, 
						 											 const char *text, 
						 											 Rectangle rec, 
						 											 bool word_wrap, 
						 											 Color tint)
{
	draw_text_boxed_selectable(font, text, rec, word_wrap, tint, 0, 0, WHITE, WHITE);
}

void render_files(void)
{
	const int tiles_per_row = GetScreenWidth() / (tile_width + tile_spacing);
	for (size_t i = 0; i < paths.count; ++i) {
		const int tile_pos_x = i % tiles_per_row;
		const int tile_pos_y = i / tiles_per_row;

		const Vector2 tile_pos = {
			tile_pos_x * (tile_width + tile_spacing) + tile_spacing, 
		  tile_pos_y * (tile_height + tile_spacing) + tile_spacing - scroll_offset_y
		};

		if (tile_pos.y + tile_height < 0
		|| tile_pos.y > GetScreenHeight()) 
		{
			continue;
		}

		Color tile_color = TILE_COLOR;
		if (last_clicked_tile_pos.x == tile_pos_x
		&&  last_clicked_tile_pos.y == tile_pos_y)
		{
			tile_color = CLICKED_TILE_COLOR;
		}

		DrawRectangleV(tile_pos, (Vector2){tile_width, tile_height}, tile_color);

		if (last_clicked_tile_pos.x == tile_pos_x
		&&  last_clicked_tile_pos.y == tile_pos_y)
		{
		  Rectangle tile_rect = {
				// apply spacing from left and right
				.x = tile_pos.x + text_padding,
				.width = tile_width - text_padding * 2, 
				.y = tile_pos.y + text_padding,
				.height = tile_height - text_padding
			};
			draw_text_boxed(font, paths.items[i], tile_rect, true, WHITE);
		} else {
			Vector2 text_pos = {
				tile_pos.x + text_padding, 
			  tile_pos.y + tile_height - font_size - text_padding
			};
			draw_truncated_text(paths.items[i], text_pos, tile_width - 2 * text_padding, WHITE);
		}
	}
}

int main(const int argc, char *argv[]) 
{
	SetTargetFPS(60);
	SetConfigFlags(FLAG_WINDOW_RESIZABLE); 
	InitWindow(1000, 600, "fe");

	font = LoadFontEx(FONT_PATH, font_size, NULL, 0);

	if (argc > 1 
	&& nob_get_file_type(argv[1]) == NOB_FILE_DIRECTORY) 
	{
		curr_dir = argv[1];
	}

	read_dir();
	memory_init(1);

	while (!WindowShouldClose()) {
		BeginDrawing();
			ClearBackground(BACKGROUND_COLOR);
			handle_keyboard_input();
			handle_mouse_input();
			render_files();
		EndDrawing();
	}

	memory_release();
	UnloadFont(font);
	CloseWindow();
	return 0;
}
