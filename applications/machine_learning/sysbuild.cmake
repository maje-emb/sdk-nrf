#
# Copyright (c) 2023 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

if(SB_CONFIG_ML_APP_INCLUDE_REMOTE_IMAGE AND DEFINED SB_CONFIG_ML_APP_REMOTE_BOARD)
  # Add remote project
  ExternalZephyrProject_Add(
    APPLICATION remote
    SOURCE_DIR ${APP_DIR}/remote
    BOARD ${SB_CONFIG_ML_APP_REMOTE_BOARD}
  )

set_config_bool(machine_learning CONFIG_ML_APP_INCLUDE_REMOTE_IMAGE y)

# set_property(GLOBAL APPEND PROPERTY PM_DOMAINS CPUNET)
# set_property(GLOBAL APPEND PROPERTY PM_CPUNET_IMAGES remote)
# set_property(GLOBAL PROPERTY DOMAIN_APP_CPUNET remote)
# set(CPUNET_PM_DOMAIN_DYNAMIC_PARTITION remote CACHE INTERNAL "")

# Add a dependency so that the remote sample will be built and flashed first
add_dependencies(machine_learning remote)
# Add dependency so that the remote image is flashed first.
sysbuild_add_dependencies(FLASH machine_learning remote)

endif()