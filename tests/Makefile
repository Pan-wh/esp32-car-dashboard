# 逻辑单元测试（PC 上运行，不需要硬件/LVGL）
CC      ?= cc
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

clean:
	rm -f $(TESTS)

.PHONY: all run clean
