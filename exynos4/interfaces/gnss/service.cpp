#define LOG_TAG "android.hardware.gnss@1.0-service"

#include <android/hardware/gnss/1.0/IGnss.h>

#include <hidl/LegacySupport.h>

using android::hardware::gnss::V1_0::IGnss;
using android::hardware::defaultPassthroughServiceImplementation;

int main() {
    // Stay on /dev/binder: the HAL takes its wakelocks through libpower,
    // which finds ISystemSuspend in servicemanager, and nothing in the
    // legacy GPS library uses /dev/vndbinder.
    return defaultPassthroughServiceImplementation<IGnss>();
}
