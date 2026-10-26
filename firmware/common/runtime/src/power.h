#pragma once

// Dynamic frequency scaling for an image built with CONFIG_PM_ENABLE. Limits
// come from CONFIG_OS_PM_DFS_MIN_MHZ and CONFIG_OS_PM_DFS_MAX_MHZ, set per
// board in its sdkconfig.defaults. Light sleep stays off: sampling, BLE and
// the LAN select loop do not tolerate a sleeping CPU. Without PM_ENABLE the
// CPU keeps its boot frequency and this reports pm=false.

namespace opensensor::power {
// Call once from the board's app_main, before tasks that take PM locks.
// Returns true when DFS was applied.
bool configure_dfs();
} // namespace opensensor::power
