LOCAL_PATH := $(call my-dir)

# Common size/lean flags: -Os, dead-code stripping, hidden symbols, strip.
COMMON_CPPFLAGS := -std=c++17 -Wall -Os -ffunction-sections -fdata-sections -fvisibility=hidden
COMMON_LDFLAGS  := -Wl,--gc-sections -s

include $(CLEAR_VARS)
LOCAL_MODULE    := fodbridge
LOCAL_SRC_FILES := ../fodbridge.cpp ../aoddoze.cpp
LOCAL_CPPFLAGS  := $(COMMON_CPPFLAGS)
LOCAL_LDFLAGS   := $(COMMON_LDFLAGS)
LOCAL_LDLIBS    := -lbinder_ndk
include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_MODULE    := fodinject
LOCAL_SRC_FILES := ../fodinject.cpp
LOCAL_CPPFLAGS  := $(COMMON_CPPFLAGS)
LOCAL_LDFLAGS   := $(COMMON_LDFLAGS)
include $(BUILD_EXECUTABLE)
