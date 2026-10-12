# 逻辑单元测试（PC 上运行，不需要硬件/LVGL）
CC      ?= cc
# UI 回归需要带 Pillow 的解释器；可用 PYTHON=/path/to/python3 make run 覆盖
PYTHON  ?= python3
CFLAGS  := -O1 -std=gnu99 -Wall -Wextra -I../simulator/ui
SRC     := ../simulator/ui/gnss_nmea.c ../simulator/ui/telemetry.c

TESTS   := test_nmea test_telemetry

all: $(TESTS)

test_nmea: test_nmea.c ../simulator/ui/gnss_nmea.c
	$(CC) $(CFLAGS) -o $@ $^

test_telemetry: test_telemetry.c ../simulator/ui/telemetry.c
	$(CC) $(CFLAGS) -o $@ $^

run: test_nmea test_telemetry
	./test_nmea
	./test_telemetry
	@echo "--- iOS 侧纯逻辑（无需 iOS 运行时）---"
	@swift ../tools/test_ios_logic.swift
	@echo "--- UI 像素级回归（需已构建 simulator/dashboard_sim）---"
	@if [ ! -x ../simulator/dashboard_sim ]; then \
		echo "  跳过：未构建 dashboard_sim（cd ../simulator && make）"; \
	elif ! $(PYTHON) -c "import PIL" 2>/dev/null; then \
		echo "  跳过：$(PYTHON) 缺少 Pillow（用 PYTHON=<带Pillow的解释器> make run）"; \
	else \
		$(PYTHON) ../tools/ui_regression.py; \
	fi

clean:
	rm -f $(TESTS)

.PHONY: all run clean
