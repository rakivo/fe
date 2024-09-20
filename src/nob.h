#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>

#define NOB_INVALID_PROC (-1)

typedef int Nob_Proc;

typedef struct {
	const char **items;
	size_t count;
	size_t capacity;
} Nob_Cmd;

typedef struct {
	char *items;
	size_t count;
	size_t capacity;
} Nob_String_Builder;

typedef enum {
	NOB_INFO,
	NOB_WARNING,
	NOB_ERROR,
} Nob_Log_Level;

// Append a sized buffer to a string builder
#define nob_sb_append_buf(sb, buf, size) nob_da_append_many(sb, buf, size)

// Append a NULL-terminated string to a string builder
#define nob_sb_append_cstr(sb, cstr)																												\
	do {																																											\
		const char *s = (cstr);																																	\
		size_t n = strlen(s);																																		\
		nob_da_append_many(sb, s, n);																														\
	} while (0)

// Append a single NULL character at the end of a string builder. So then you can
// use it a NULL-terminated C string
#define nob_sb_append_null(sb) nob_da_append_many(sb, "", 1)

// Free the memory allocated by a string builder
#define nob_sb_free(sb) free((sb).items)

#define nob_da_append(da, item)																															\
	do {																																											\
		if ((da)->count >= (da)->capacity) {																										\
			(da)->capacity = (da)->capacity == 0 ? NOB_DA_INIT_CAP : (da)->capacity*2;						\
			(da)->items = realloc((da)->items, (da)->capacity*sizeof(*(da)->items));							\
			assert((da)->items != NULL && "Buy more RAM lol");																		\
		}																																												\
																																														\
		(da)->items[(da)->count++] = (item);																										\
	} while (0)

#define nob_da_free(da) free((da).items)

#define nob_cmd_append(cmd, ...)																														\
	nob_da_append_many(cmd,																																		\
					   ((const char*[]){__VA_ARGS__}),																								\
					   (sizeof((const char*[]){__VA_ARGS__})/sizeof(const char*)))

// Free all the memory allocated by command arguments
#define nob_cmd_free(cmd) free(cmd.items)

#define nob_cmd_append(cmd, ...)																														\
	nob_da_append_many(cmd,																																		\
					   ((const char*[]){__VA_ARGS__}),																								\
					   (sizeof((const char*[]){__VA_ARGS__})/sizeof(const char*)))

#define NOB_DA_INIT_CAP 256

#ifdef __cplusplus
#define nob_da_append_many(da, new_items, new_items_count)																	\
	do {																																											\
		if ((da)->count + (new_items_count) > (da)->capacity) {																	\
			if ((da)->capacity == 0) {																														\
				(da)->capacity = NOB_DA_INIT_CAP;																										\
			}																																											\
			while ((da)->count + (new_items_count) > (da)->capacity) {														\
				(da)->capacity *= 2;																																\
			}																																											\
			(da)->items = (decltype((da)->items)) realloc((da)->items, (da)->capacity*sizeof(*(da)->items)); \
			assert((da)->items != NULL && "Buy more RAM lol");																		\
		}																																												\
		memcpy((da)->items + (da)->count, (new_items), (new_items_count)*sizeof(*(da)->items)); \
		(da)->count += (new_items_count);																												\
	} while (0)
#else
#define nob_da_append_many(da, new_items, new_items_count)																	\
	do {																																											\
		if ((da)->count + (new_items_count) > (da)->capacity) {																	\
			if ((da)->capacity == 0) {																														\
				(da)->capacity = NOB_DA_INIT_CAP;																										\
			}																																											\
			while ((da)->count + (new_items_count) > (da)->capacity) {														\
				(da)->capacity *= 2;																																\
			}																																											\
			(da)->items = realloc((da)->items, (da)->capacity*sizeof(*(da)->items)); \
			assert((da)->items != NULL && "Buy more RAM lol");																		\
		}																																												\
		memcpy((da)->items + (da)->count, (new_items), (new_items_count)*sizeof(*(da)->items)); \
		(da)->count += (new_items_count);																												\
	} while (0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

bool nob_proc_kill(Nob_Proc proc, bool echo);
Nob_Proc nob_cmd_run_async(Nob_Cmd cmd, bool echo);

void nob_log(Nob_Log_Level level, const char *fmt, ...);
void nob_cmd_render(Nob_Cmd cmd, Nob_String_Builder *render);

#ifdef __cplusplus
}
#endif
