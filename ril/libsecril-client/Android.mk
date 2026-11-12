# Copyright 2006 The Android Open Source Project

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    secril-client.cpp

LOCAL_SHARED_LIBRARIES := \
    libutils \
    libbinder \
    libcutils \
    liblog

LOCAL_C_INCLUDES += \
    hardware/ril/include

LOCAL_CFLAGS := -Wno-error

ifeq ($(TARGET_BOARD_PLATFORM),exynos4)
LOCAL_CFLAGS += -DRIL_CALL_AUDIO_PATH_EXTRAVOLUME
endif
ifneq ($(filter m7450 mdm9x35 ss333 xmm7260,$(BOARD_MODEM_TYPE)),)
LOCAL_CFLAGS += -DSAMSUNG_NEXT_GEN_MODEM
endif

ifeq ($(TARGET_USES_VND_SECRIL), true)
LOCAL_CFLAGS += -DUSES_VND_SECRIL
endif

LOCAL_MODULE:= libsecril-client
LOCAL_VENDOR_MODULE := true

include $(BUILD_SHARED_LIBRARY)
