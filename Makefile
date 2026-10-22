.DEFAULT_GOAL := build

CMAKE ?= cmake
CTEST ?= ctest

BUILD_DIR ?= build/release
TEST_BUILD_DIR ?= build/testing
COVERAGE_BUILD_DIR ?= build/coverage

JOBS ?= 2
TEST_FILTER ?= .
CMAKE_FLAGS ?=

.PHONY: build test coverage

build:
	$(CMAKE) -S . -B "$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_TESTING=OFF $(CMAKE_FLAGS)
	$(CMAKE) --build "$(BUILD_DIR)" --parallel "$(JOBS)"

test:
	$(CMAKE) -S . -B "$(TEST_BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTING=ON \
		-DENABLE_COVERAGE=OFF $(CMAKE_FLAGS)
	$(CMAKE) --build "$(TEST_BUILD_DIR)" --parallel "$(JOBS)"
	cd "$(TEST_BUILD_DIR)" && \
		$(CTEST) --output-on-failure --parallel "$(JOBS)" \
			-R "$(TEST_FILTER)"

coverage:
	$(CMAKE) -S . -B "$(COVERAGE_BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTING=ON \
		-DENABLE_COVERAGE=ON \
		-DCMAKE_C_COMPILER=clang $(CMAKE_FLAGS)
	$(CMAKE) --build "$(COVERAGE_BUILD_DIR)" --target coverage --parallel "$(JOBS)"
