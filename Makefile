.PHONY: build

# Keep the default build independent of test dependencies.
BUILD_DIR := build/release
BUILD_TYPE := Release
BUILD_TESTING := OFF
CMAKE_OPTIONS :=

# Targets that run tests share a debug build.
ifneq ($(filter tests coverage all,$(MAKECMDGOALS)),)
    BUILD_DIR := build/testing
    BUILD_TESTING := ON
    BUILD_TYPE := Debug
    CMAKE_OPTIONS := -DENABLE_COVERAGE=OFF
endif

# Use a separate build directory for coverage.
ifneq ($(filter coverage all,$(MAKECMDGOALS)),)
    BUILD_DIR := build/coverage
    CMAKE_OPTIONS := \
        -DENABLE_COVERAGE=ON \
        -DCMAKE_C_COMPILER=clang
endif

default: build

all: coverage

# Configure and compile once for all requested targets.
build:
	@cmake -S . -B "$(BUILD_DIR)" $(CMAKE_OPTIONS) \
		-DBUILD_TESTING=$(BUILD_TESTING) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

	@cmake --build "$(BUILD_DIR)" --parallel 2

# Collect fresh profiles so previous runs cannot hide coverage gaps.
tests: build
	@rm -rf "$(BUILD_DIR)/profiles" \
		"$(BUILD_DIR)/coverage.lcov" \
		"$(BUILD_DIR)/coverage.profdata"
	@mkdir -p "$(BUILD_DIR)/profiles"

	@cd "$(BUILD_DIR)" && \
		LLVM_PROFILE_FILE="$(CURDIR)/$(BUILD_DIR)/profiles/%m-%p.profraw" \
		ctest --output-on-failure --parallel 2

# Generate the report from those profiles without running tests again.
coverage: tests
	@cmake --build "$(BUILD_DIR)" \
		--target coverage_report \
		--parallel 2
