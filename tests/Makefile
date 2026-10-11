# 逻辑单元测试（PC 上运行，不需要硬件/LVGL）
CC      ?= cc
CFLAGS  := -O1 -std=gnu99 -Wall -Wextra -I../simulator/ui
SRC     := ../simulator/ui/gnss_nmea.c

TESTS   := test_nmea

all: $(TESTS)

test_nmea: test_nmea.c $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

run: test_nmea
	./test_nmea

clean:
	rm -f $(TESTS)

.PHONY: all run clean
