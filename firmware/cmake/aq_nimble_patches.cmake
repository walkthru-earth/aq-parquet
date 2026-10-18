# NimBLE host fixes the pinned ESP-IDF does not carry yet.
#
# The SDK checkout stays untouched (tools/setup-idf.sh refuses a changed one).
# aq_patch_nimble() copies the pinned source into the build directory, applies
# our patch from firmware/common/vendor/esp-nimble and compiles that copy in
# place of the SDK file. Every board project calls it right after project().
#
# The SHA-256 pin fails the configure step when an SDK upgrade changes the
# upstream file, so whoever moves the SDK must check whether the fix landed
# upstream and then drop or rebase the patch.
include_guard(GLOBAL)

set(AQ_NIMBLE_PATCH_DIR "${CMAKE_CURRENT_LIST_DIR}/../common/vendor/esp-nimble")

function(aq_patch_nimble)
    # The Waveshare diagnostic image has no Bluetooth.
    if(NOT CONFIG_BT_NIMBLE_ENABLED)
        message(STATUS "aq_patch_nimble: NimBLE is off, nothing to patch")
        return()
    endif()
    idf_build_get_property(idf_path IDF_PATH)
    idf_component_get_property(bt_lib bt COMPONENT_LIB)
    if(NOT bt_lib OR NOT TARGET "${bt_lib}")
        message(FATAL_ERROR "aq_patch_nimble: the bt component is not part of this build")
    endif()

    set(relative "nimble/host/src/ble_hs_hci.c")
    set(upstream_root "${idf_path}/components/bt/host/nimble/nimble")
    set(upstream "${upstream_root}/${relative}")
    set(patch_file "${AQ_NIMBLE_PATCH_DIR}/ble_hs_hci-stale-ack.patch")
    # esp-nimble 139cada as pinned by ESP-IDF v6.1 (tools/idf-dependencies.lock).
    set(expected "576e8582bf25dd56170ce1d47d5c64ccddf1fc5facd7dcfa818dc19629cbd7b1")

    file(SHA256 "${upstream}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR
            "aq_patch_nimble: ${upstream} changed (sha256 ${actual}, pinned ${expected}). "
            "Check whether the SDK now carries the stale ack drain from esp-nimble "
            "79316193f9, then drop or rebase ${patch_file} and update this pin.")
    endif()

    set(stage "${CMAKE_BINARY_DIR}/aq_nimble_patch")
    file(REMOVE_RECURSE "${stage}")
    configure_file("${upstream}" "${stage}/${relative}" COPYONLY)
    find_program(AQ_PATCH_PROGRAM patch REQUIRED)
    # -F0 refuses fuzz, -N refuses a reversed or already applied patch.
    execute_process(
        COMMAND "${AQ_PATCH_PROGRAM}" -p1 -N -F0 -s -d "${stage}" -i "${patch_file}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "aq_patch_nimble: ${patch_file} does not apply\n${output}")
    endif()
    set(patched "${stage}/${relative}")

    get_target_property(sources "${bt_lib}" SOURCES)
    set(matches "")
    foreach(source IN LISTS sources)
        if(source MATCHES "(^|/)nimble/host/src/ble_hs_hci\\.c$")
            list(APPEND matches "${source}")
        endif()
    endforeach()
    list(LENGTH matches count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "aq_patch_nimble: expected one ble_hs_hci.c in ${bt_lib}, found ${count}")
    endif()
    list(REMOVE_ITEM sources ${matches})
    list(APPEND sources "${patched}")
    set_property(TARGET "${bt_lib}" PROPERTY SOURCES "${sources}")
    # The copy lives outside its source directory, so its quoted includes
    # (ble_hs_priv.h) need that directory on the path.
    set_source_files_properties("${patched}" TARGET_DIRECTORY "${bt_lib}"
        PROPERTIES INCLUDE_DIRECTORIES "${upstream_root}/nimble/host/src")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${patch_file}" "${upstream}")
    message(STATUS "aq_patch_nimble: ${relative} patched with ${patch_file}")
endfunction()
