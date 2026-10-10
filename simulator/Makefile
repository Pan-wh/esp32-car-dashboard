# ESP32 车载仪表 —— macOS LVGL 模拟器
# 依赖：macOS + Xcode CLT（make/clang）+ Homebrew SDL2 + deps/lvgl（v8.4.0，见 fetch_deps.sh）

SHELL  := /bin/sh
CC     := cc

LVGL_DIR := deps/lvgl
BUILD    := build
BIN      := dashboard_sim

SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

CFLAGS := -O2 -std=gnu99 -Wall -Wextra -Wno-unused-parameter \
          -I. -Iui -I$(LVGL_DIR) \
          -DLV_CONF_INCLUDE_SIMPLE \
          -DLV_LVGL_H_INCLUDE_SIMPLE \
          $(SDL_CFLAGS)

# UI 源码：ui/ 下全部（ui_clock.c + 生成的字体 font_*.c）
UI_SRC := $(wildcard ui/*.c)

LVGL_SRC := $(shell find $(LVGL_DIR)/src -name '*.c')
APP_SRC  := main.c $(UI_SRC)
SRCS     := $(LVGL_SRC) $(APP_SRC)
OBJS     := $(addprefix $(BUILD)/,$(SRCS:=.o))

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(SDL_LIBS)
	@echo "构建完成: ./$(BIN)（运行: make run）"

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN)

# 验证用：无窗口自动截图（依赖 Pillow 转 PNG 可用 tools/sim_preview.sh）
shot: $(BIN)
	./$(BIN) --offscreen --ms 1500 --shot shots/preview.bmp

clean:
	rm -rf $(BUILD) $(BIN)

.PHONY: all run shot clean
