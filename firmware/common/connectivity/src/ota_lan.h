#pragma once

// Device glue for LAN OTA and the boot self test mark (#40).
//
// The rules live in firmware/common/src/ota_transfer.h and boot_self_test.h,
// both host tested. This file only wraps esp_ota_begin, esp_ota_write,
// esp_ota_end, esp_ota_set_boot_partition and
// esp_ota_mark_app_valid_cancel_rollback. Nothing here touches NVS or the SD
// archive.

#include "control_sync.h"

#include <boot_self_test.h>

namespace ossync::ota {

// Command worker only, before handle_common_control. Answers OTA_BEGIN,
// OTA_WRITE, OTA_END and OTA_CANCEL and returns true, or returns false for
// any other op. The ops are LAN only. BLE gets ERROR not-on-this-link.
// Reboot stays a separate REBOOT request.
bool handle(const ble::ControlRequest &request, const ControlReplies &replies);

// The otadata state of the running slot.
[[nodiscard]] opensensor::selftest::OtaState running_state();

// Calls esp_ota_mark_app_valid_cancel_rollback() when should_mark_valid()
// says so, and reports what happened. Main loop, once per boot.
[[nodiscard]] opensensor::selftest::Mark
confirm_running_app(opensensor::selftest::Verdict verdict,
                    opensensor::selftest::OtaState state);

} // namespace ossync::ota
