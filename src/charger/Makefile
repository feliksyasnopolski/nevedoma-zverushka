CXX := aarch64-linux-gnu-g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -fPIE -I/usr/include/freetype2
LDFLAGS ?= -fPIE -pie -L/usr/lib/aarch64-linux-gnu
TARGET := tb330fu-native-charger-renderer

all: $(TARGET)

$(TARGET): tb330fu-native-charger-renderer.cpp
	$(CXX) $(CXXFLAGS) $< -I/usr/include/libdrm $(LDFLAGS) -o $@ -ldrm -lfreetype

clean:
	rm -f $(TARGET)

.PHONY: all clean
