CC := clang++
SRC_DIR := src
BIN_FILE := fe
BUILD_DIR := build
ROOT_FILE := $(SRC_DIR)/main.cpp
SRC_FILES := $(wildcard $(SRC_DIR)/*)
CFLAGS := -g -O3 -std=c++17
WFLAGS := -Wall -Wextra -Wpedantic -Wno-writable-strings -Wno-c99-extensions -Wno-missing-field-initializers -Wno-c++11-narrowing -Wno-reorder-init-list -Wno-c11-extensions
CLIBS := -lraylib -lopencv_core -lopencv_videoio -lopencv_imgcodecs -lopencv_imgproc
INCLUDE_FLAGS := -I/usr/include/opencv4

all: $(BUILD_DIR)/$(BIN_FILE)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/$(BIN_FILE): $(SRC_FILES)
	$(CC) -o $@ $(CFLAGS) $(CLIBS) $(OBJ_FILES) $(WFLAGS) $(INCLUDE_FLAGS) $(ROOT_FILE)
