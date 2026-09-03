CXX ?= aarch64-linux-gnu-g++
OUT ?= build

CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra
CPPFLAGS += -I. -I../../src/properties

TARGET := $(OUT)/tb330fu-graph-executor

.PHONY: all clean validate test

all: $(TARGET)

$(OUT):
	mkdir -p $@

$(TARGET): graph_executor.cpp jsoncpp.cpp | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

validate: $(TARGET)
	$(TARGET) --validate --graph tests/graph-executor-synthetic.json

test: validate
	$(TARGET) --validate --graph tests/graph-executor-invalid.json >/dev/null 2>&1; test $$? -eq 1

clean:
	rm -rf $(OUT)
