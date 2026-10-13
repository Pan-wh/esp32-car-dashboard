# 逻辑单元测试（PC 上运行，不需要硬件/LVGL）
CC      ?= cc
# UI 回归需要带 Pillow 的解释器；可用 PYTHON=/path/to/python3 make run 覆盖
PYTHON  ?= python3
CFLAGS  := -O1 -std=gnu99 -Wall -Wextra -I../simulator/ui
SRC     := ../simulator/ui/telemetry.c

TESTS   := test_telemetry test_control test_ui_face test_nav test_nav_parse test_img_indexed

# iOS 侧导航核心：直接编译 App 的真实源文件（不复制逻辑，避免与实现漂移）
NAV_SRC      := ../ios/CarDashboardPhone/CarDashboardPhone/Nav/NavCore.swift \
                ../ios/CarDashboardPhone/CarDashboardPhone/Nav/Coords.swift
COORD_VECTORS := ../gateway/test/fixtures/coord-vectors.json
NAV_BIN      := test_nav_core

# Mac 模拟器（纯逻辑部分；BLE 只能在真机上验证）
SIM_SRC      := $(NAV_SRC) ../tools/nav_sim/NavSimSupport.swift
SIM_BIN      := test_nav_sim
SIM_FIXTURE  := $(abspath ../tools/nav_sim/testdata/route-bundle.json)

all: $(TESTS)

test_telemetry: test_telemetry.c ../simulator/ui/telemetry.c
	$(CC) $(CFLAGS) -o $@ $^

test_control: test_control.c ../simulator/ui/control.c
	$(CC) $(CFLAGS) -o $@ $^

test_ui_face: test_ui_face.c ../simulator/ui/ui_face.c
	$(CC) $(CFLAGS) -o $@ $^

test_nav: test_nav.c ../simulator/ui/nav.c
	$(CC) $(CFLAGS) -o $@ $^

test_nav_parse: test_nav_parse.c ../simulator/ui/nav_parse.c ../simulator/ui/nav.c
	$(CC) $(CFLAGS) -o $@ $^

test_img_indexed: test_img_indexed.c ../simulator/ui/img_indexed.c
	$(CC) $(CFLAGS) -o $@ $^

run: test_telemetry test_control test_ui_face test_nav test_nav_parse test_img_indexed
	./test_telemetry
	./test_control
	./test_ui_face
	./test_nav
	./test_nav_parse
	./test_img_indexed
	@echo "--- iOS 导航核心（Swift，编译 App 真实源文件）---"
	@$(MAKE) --no-print-directory test-nav-core
	@echo "--- Mac 模拟器（Swift，纯逻辑，不需设备）---"
	@$(MAKE) --no-print-directory test-nav-sim
	@echo "--- 导航网关（Node，零依赖，全部离线）---"
	@if command -v node >/dev/null 2>&1; then \
		(cd ../gateway && node --test "test/**/*.test.js") ; \
	else \
		echo "  跳过：未找到 node（网关测试需要 Node >= 18）"; \
	fi
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

# 导航核心（需要 swiftc，即 Xcode 命令行工具）
# ⚠️ 构建与运行必须拆成两个目标：
#    若让 test_nav_core 既编译又运行，产物与目标同名，
#    第二次 make 会认为"已是最新"而**静默跳过测试**（已踩过）。
$(NAV_BIN): $(NAV_SRC) ../tools/test_nav_core.swift
	@if ! command -v swiftc >/dev/null 2>&1; then \
		echo "  跳过：未找到 swiftc（需 Xcode 命令行工具）"; \
		exit 0; \
	fi
	swiftc -O -o $(NAV_BIN) $(NAV_SRC) ../tools/test_nav_core.swift

test-nav-core: $(NAV_BIN)
	@COORD_VECTORS=$(COORD_VECTORS) ./$(NAV_BIN)

$(SIM_BIN): $(SIM_SRC) ../tools/test_nav_sim.swift
	@if ! command -v swiftc >/dev/null 2>&1; then \
		echo "  跳过：未找到 swiftc"; \
		exit 0; \
	fi
	swiftc -O -o $(SIM_BIN) $(SIM_SRC) ../tools/test_nav_sim.swift

test-nav-sim: $(SIM_BIN)
	@NAV_SIM_FIXTURE=$(SIM_FIXTURE) ./$(SIM_BIN)

clean:
	rm -f $(TESTS) $(NAV_BIN) $(SIM_BIN)

.PHONY: all run clean test-nav-core test-nav-sim
