CC := aarch64-linux-gnu-gcc
CXX := aarch64-linux-android35-clang++
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11 -fPIC
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c++17 -fPIE
LDFLAGS ?=

# Android 15 r17 headers and parser/serializer libraries are external build
# inputs. Set AOSP_ROOT to the checkout containing those sources.
AOSP_ROOT ?=
BIONIC_INC ?= $(AOSP_ROOT)/bionic/libc/include
PROPERTY_PARSER_INC ?= $(AOSP_ROOT)/system/core/property_service/libpropertyinfoparser/include
PROPERTY_SERIALIZER_INC ?= $(AOSP_ROOT)/system/core/property_service/libpropertyinfoserializer/include
ANDROID_PROPERTY_SERVICE_LDFLAGS ?=
PROPERTY_INFO_SERIALIZER_LDFLAGS ?=

.PHONY: all broker android-frontend property-info-generator validation clean check-aosp-root

all: broker
broker: rediska rediskactl

rediska: rediska.c rediska_protocol.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ rediska.c -pthread

rediskactl: rediskactl.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ rediskactl.c

android-frontend: check-aosp-root rediska_android_property_service
rediska_android_property_service: rediska_android_property_service.cpp check-aosp-root
	$(CXX) $(CXXFLAGS) -I$(BIONIC_INC) -I$(PROPERTY_PARSER_INC) $(LDFLAGS) -o $@ $< $(ANDROID_PROPERTY_SERVICE_LDFLAGS)

property-info-generator: check-aosp-root rediska_property_info_gen
rediska_property_info_gen: rediska_property_info_gen.cpp check-aosp-root
	$(CXX) $(CXXFLAGS) -I$(PROPERTY_SERIALIZER_INC) $(LDFLAGS) -o $@ $< $(PROPERTY_INFO_SERIALIZER_LDFLAGS)

validation: check-aosp-root rediska_genuine_backend rediska_genuine_client rediska-no-selinux-xattr.so
rediska_genuine_backend: rediska_genuine_backend.c check-aosp-root
	$(CC) $(CFLAGS) -I$(BIONIC_INC) $(LDFLAGS) -o $@ $< -ldl
rediska_genuine_client: rediska_genuine_client.c check-aosp-root
	$(CC) $(CFLAGS) -I$(BIONIC_INC) $(LDFLAGS) -o $@ $< -ldl
rediska-no-selinux-xattr.so: rediska-no-selinux-xattr.c
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -o $@ $<

check-aosp-root:
	test -n "$(AOSP_ROOT)" || (echo "set AOSP_ROOT to the Android 15 r17 AOSP checkout" >&2; exit 2)

clean:
	rm -f rediska rediskactl rediska_android_property_service rediska_property_info_gen
	rm -f rediska_genuine_backend rediska_genuine_client rediska-no-selinux-xattr.so
