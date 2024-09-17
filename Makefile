CC := clang
CXX := clang++

SRC_DIR := src
BIN_FILE := fe
BUILD_DIR := build
ROOT_FILE := $(SRC_DIR)/main.cpp
SRC_FILES := $(wildcard $(SRC_DIR)/*)
C_FILES := $(wildcard $(SRC_DIR)/*.c)
OBJ_FILES := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_FILES))

CFLAGS := -g -O0 -std=c11
CXXFLAGS := -g -O0 -std=c++17
WFLAGS := -Wall -Wextra -Wpedantic -Wno-writable-strings -Wno-c99-extensions -Wno-missing-field-initializers -Wno-c++11-narrowing -Wno-reorder-init-list -Wno-c11-extensions -Wdeprecated
CXXWFLAGS := -Wno-deprecated-dynamic-exception-spec -Wno-deprecated-dynamic-exception-spec -Wno-deprecated-copy-with-user-provided-dtor
CLIBS := -lraylib -lopencv_core -lopencv_videoio -lopencv_imgcodecs -lopencv_imgproc
INCLUDE_FLAGS := -I/usr/include/opencv4

all: $(BUILD_DIR)/$(BIN_FILE)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_FILES)
	$(CC) $< -c -o $@ $(CFLAGS) $(WFLAGS) $(INCLUDE_FLAGS)

$(BUILD_DIR)/$(BIN_FILE): $(SRC_FILES) $(OBJ_FILES)
	$(CXX) -o $@ $(CXXFLAGS) $(CLIBS) $(WFLAGS) $(CXXWFLAGS) $(INCLUDE_FLAGS) $(ROOT_FILE) $(OBJ_FILES)
