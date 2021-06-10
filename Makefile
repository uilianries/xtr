-include conanbuildinfo.mak

PREFIX ?= /usr/local
EXCEPTIONS ?= 1
COVERAGE ?= 0
DEBUG ?= 0
PIC ?= 0
RTTI ?= 0

GOOGLE_BENCH_CPPFLAGS = $(addprefix -isystem, $(CONAN_INCLUDE_DIRS_BENCHMARK) $(GOOGLE_BENCH_INCLUDE_DIR))
GOOGLE_BENCH_LDFLAGS = $(addprefix -L, $(CONAN_LIB_DIRS_BENCHMARK) $(GOOGLE_BENCH_LIB_DIR))
CATCH2_CPPFLAGS = $(addprefix -isystem, $(CONAN_INCLUDE_DIRS_CATCH2) $(CATCH2_INCLUDE_DIR))
FMT_CPPFLAGS = $(addprefix -isystem, $(CONAN_INCLUDE_DIRS_FMT) $(FMT_INCLUDE_DIR))
FMT_LDFLAGS = $(addprefix -L, $(CONAN_LIB_DIRS_FMT) $(FMT_LIB_DIR))

BUILD_DIR := build/$(CXX)

CXXFLAGS = \
	-std=c++20 -Wall -Wextra -Wconversion -Wshadow -Wcast-qual -Wformat=2 \
	-pedantic -pipe -pthread
CPPFLAGS = -MMD -MP -I include $(FMT_CPPFLAGS) -DXTR_FUNC=
LDFLAGS = -fuse-ld=gold
LDLIBS = -lxtr

DEBUG_CXXFLAGS = -O0 -ggdb -ftrapv
DEBUG_CPPFLAGS = -DXTR_ENABLE_TEST_STATIC_ASSERTIONS

OPT_CXXFLAGS = -O3 -march=native -flto
OPT_CPPFLAGS = -DNDEBUG

TEST_CPPFLAGS = $(CATCH2_CPPFLAGS) 
TEST_LDFLAGS = -L $(BUILD_DIR) $(FMT_LDFLAGS)

BENCH_CPPFLAGS = $(GOOGLE_BENCH_CPPFLAGS)
BENCH_LDFLAGS = -L $(BUILD_DIR) $(GOOGLE_BENCH_LDFLAGS) $(FMT_LDFLAGS)
BENCH_LDLIBS = -lbenchmark

XTRCTL_LDFLAGS = -L $(BUILD_DIR)

COVERAGE_CXXFLAGS = --coverage -DNDEBUG

# Use the libfmt submodule if it is present and no include directory for
# libfmt has been configured (including via Conan).
ifeq ($(FMT_CPPFLAGS),)
	ifneq ($(wildcard third_party/fmt/include),)
		SUBMODULES_FLAG := 1
	endif
endif

ifneq ($(SUBMODULES_FLAG),)
	FMT_CPPFLAGS += -DFMT_HEADER_ONLY
	CPPFLAGS += -isystem third_party/include
else
	LDLIBS += -lfmt
endif

ifneq (,$(findstring clang,$(CXX)))
	RANLIB ?= llvm-ranlib
	AR ?= llvm-ar
else
	RANLIB ?= gcc-ranlib
	AR ?= gcc-ar
endif

ifeq ($(PIC), 1)
	CXXFLAGS += -fPIC
	BUILD_DIR := $(BUILD_DIR)-pic
endif

ifeq ($(RTTI), 1)
	BUILD_DIR := $(BUILD_DIR)-rtti
else
	CXXFLAGS += -fno-rtti
endif

ifeq ($(COVERAGE), 1)
	CXXFLAGS += $(COVERAGE_CXXFLAGS)
	BUILD_DIR := $(BUILD_DIR)-coverage
	COVERAGE_DATA = \
		$(SRCS:%=$(BUILD_DIR)/%.gcno) $(SRCS:%=$(BUILD_DIR)/%.gcda) \
		$(TEST_SRCS:%=$(BUILD_DIR)/%.gcno) $(TEST_SRCS:%=$(BUILD_DIR)/%.gcda)
endif

ifeq ($(DEBUG), 1)
	CXXFLAGS += $(DEBUG_CXXFLAGS)
	CPPFLAGS += $(DEBUG_CPPFLAGS)
	BUILD_DIR := $(BUILD_DIR)-debug
else
	CXXFLAGS += $(OPT_CXXFLAGS)
	CPPFLAGS += $(OPT_CPPFLAGS)
	BUILD_DIR := $(BUILD_DIR)-release
endif

ifneq ($(SANITIZER),)
	CXXFLAGS += -fno-omit-frame-pointer -fsanitize=$(SANITIZER)
	BUILD_DIR := $(BUILD_DIR)-$(SANITIZER)-sanitizer
endif

ifeq ($(EXCEPTIONS), 0)
	CXXFLAGS += -fno-exceptions
	BUILD_DIR := $(BUILD_DIR)-no-exceptions
endif

TARGET = $(BUILD_DIR)/libxtr.a
SRCS := \
	src/command_dispatcher.cpp src/command_path.cpp src/file_descriptor.cpp \
	src/logger.cpp src/matcher.cpp src/memory_mapping.cpp \
	src/mirrored_memory_mapping.cpp src/pagesize.cpp src/regex_matcher.cpp \
	src/throw.cpp src/tsc.cpp src/wildcard_matcher.cpp
OBJS = $(SRCS:%=$(BUILD_DIR)/%.o)

TEST_TARGET = $(BUILD_DIR)/test/test
TEST_SRCS := \
	test/align.cpp test/command_client.cpp test/command_dispatcher.cpp \
	test/file_descriptor.cpp test/llvm_mca.cpp test/logger.cpp \
	test/main.cpp test/memory_mapping.cpp  test/mirrored_memory_mapping.cpp \
	test/pagesize.cpp test/synchronized_ring_buffer.cpp	test/throw.cpp
TEST_OBJS = $(TEST_SRCS:%=$(BUILD_DIR)/%.o)

BENCH_TARGET = $(BUILD_DIR)/benchmark/benchmark
BENCH_SRCS := benchmark/logger.cpp benchmark/main.cpp
BENCH_OBJS = $(BENCH_SRCS:%=$(BUILD_DIR)/%.o)

XTRCTL_TARGET = $(BUILD_DIR)/xtrctl
XTRCTL_SRCS := src/xtrctl/main.cpp
XTRCTL_OBJS = $(XTRCTL_SRCS:%=$(BUILD_DIR)/%.o)

DEPS = $(OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(BENCH_OBJS:.o=.d) $(XTRCTL_OBJS:.o=.d)

$(TARGET): $(OBJS)
	$(AR) rc $@ $^
	$(RANLIB) $@

$(TEST_TARGET): $(TARGET) $(TEST_OBJS)
	$(LINK.cc) -o $@ $(TEST_LDFLAGS) $(TEST_OBJS) $(LDLIBS)

$(BENCH_TARGET): $(TARGET) $(BENCH_OBJS)
	$(LINK.cc) -o $@ $(BENCH_LDFLAGS) $(BENCH_OBJS) $(LDLIBS) $(BENCH_LDLIBS)

$(XTRCTL_TARGET): $(XTRCTL_OBJS)
	$(LINK.cc) -o $@ $(XTRCTL_LDFLAGS) $(XTRCTL_OBJS) $(LDLIBS)

$(OBJS): $(BUILD_DIR)/%.cpp.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) -o $@ -c $(CPPFLAGS) $(CXXFLAGS) $<

$(TEST_OBJS): $(BUILD_DIR)/%.cpp.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) -o $@ -c $(CPPFLAGS) $(TEST_CPPFLAGS) $(CXXFLAGS) $<

$(BENCH_OBJS): $(BUILD_DIR)/%.cpp.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) -o $@ -c $(CPPFLAGS) $(BENCH_CPPFLAGS) $(CXXFLAGS) $<

$(XTRCTL_OBJS): $(BUILD_DIR)/%.cpp.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) -o $@ -c $(CPPFLAGS) $(CXXFLAGS) $<

all: $(TARGET)

check: $(TEST_TARGET)
	$< --order rand

benchmark: $(BENCH_TARGET)
	$<

xtrctl: $(XTRCTL_TARGET)

single_include:
	scripts/make_single_include.sh

install: $(TARGET)
	mkdir -p $(PREFIX)/lib $(PREFIX)/include/xtr/detail
	install $< $(PREFIX)/lib
	install include/xtr/logger.hpp $(PREFIX)/include/xtr/logger.hpp
	install include/xtr/detail/*.hpp $(PREFIX)/include/xtr/detail/

clean:
	$(RM) $(TARGET) $(TEST_TARGET) $(OBJS) $(TEST_OBJS) $(DEPS) $(COVERAGE_DATA)

coverage_report: $(BUILD_DIR)/coverage_report/index.html

$(BUILD_DIR)/coverage_report/index.html: $(TEST_TARGET)
ifeq ($(COVERAGE), 0)
	$(error COVERAGE=1 option required)
endif
	$<
	@mkdir -p $(@D)
	gcovr --exclude test --exclude third_party --html-detail $@ -r .

-include $(DEPS)

.PHONY: all check benchmark single_include install clean coverage_report
