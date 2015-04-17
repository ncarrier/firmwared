LOCAL_PATH := $(call my-dir)

################################################################################
# firmwared
################################################################################

include $(CLEAR_VARS)
LOCAL_MODULE := firmwared
LOCAL_DESCRIPTION := Firmware instance manager daemon
LOCAL_CATEGORY_PATH := simulator/

LOCAL_LIBRARIES := \
	libulog \
	libpomp

LOCAL_SRC_FILES := \
	$(call all-c-files-under,src)

LOCAL_C_INCLUDES := $(LOCAL_PATH)/src

LOCAL_COPY_FILES := \
	resources/adjectives:usr/share/$(LOCAL_MODULE)/ \
	resources/names:usr/share/$(LOCAL_MODULE)/

include $(BUILD_EXECUTABLE)
