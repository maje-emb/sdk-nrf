#
# Copyright (c) 2020 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

if (DEFINED SHIELD AND ${BOARD} STREQUAL "nrf5340dk_nrf5340_cpunet")
  string(REPLACE " " ";" SHIELD_LIST "${SHIELD}")
  foreach(sh IN_LIST SHIELD_LIST)
    if (${sh} STREQUAL "nrf21540_ek")
      if (CONFIG_DTM_USB)
        set(remote_shell_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/conf/remote_shell/nrf21540_ek_usb.overlay)
      else()
        set(remote_shell_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/conf/remote_shell/nrf21540_ek.overlay)
      endif()

      break()
    endif()
  endforeach()
endif()

if (NOT DEFINED remote_shell_DTC_OVERLAY_FILE AND CONFIG_DTM_USB)
  set(remote_shell_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/conf/remote_shell/usb.overlay)
endif()

find_package(Git QUIET)
if(CONFIG_VESRION_BANNER AND NOT APP_VERSION AND GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --abbrev=12
    WORKING_DIRECTORY                ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE                  APP_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE                   stderr
    RESULT_VARIABLE                  return_code
  )
  if(return_code)
    message(STATUS "git describe failed: ${stderr}; ${KERNEL_VERSION_STRING} will be used instead")
  elseif(CMAKE_VERBOSE_MAKEFILE)
    message(STATUS "git describe stderr: ${stderr}")
  endif()
endif()

if(APP_VERSION)
  zephyr_compile_definitions(
    APP_VERSION=${APP_VERSION}
  )
endif()

if (CONFIG_DTM_USB)
  set(remote_shell_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/conf/remote_shell/prj_usb.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(NONE)

# Application sources
add_subdirectory(src/fem)

# NORDIC SDK APP START
target_sources(app PRIVATE
  src/main.c
  src/dtm.c
  src/dtm_hw.c
)
# NORDIC SDK APP END
