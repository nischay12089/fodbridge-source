LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := fodhook
LOCAL_SRC_FILES := ../libfodhook.cpp
# Lean/size flags: -Os, dead-code stripping, hidden symbols, strip. No -llog
# (logging removed for production).
LOCAL_CPPFLAGS  := -std=c++17 -Wall -Os -ffunction-sections -fdata-sections -fvisibility=hidden
LOCAL_LDFLAGS   := -Wl,--gc-sections -s
include $(BUILD_SHARED_LIBRARY)
