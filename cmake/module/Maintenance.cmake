# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include_guard(GLOBAL)

function(setup_split_debug_script)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(OBJCOPY ${CMAKE_OBJCOPY})
    set(STRIP ${CMAKE_STRIP})
    configure_file(
      contrib/devtools/split-debug.sh.in split-debug.sh
      FILE_PERMISSIONS OWNER_READ OWNER_EXECUTE
                       GROUP_READ GROUP_EXECUTE
                       WORLD_READ
      @ONLY
    )
  endif()
endfunction()

function(add_maintenance_targets)
  if(NOT TARGET Python3::Interpreter)
    return()
  endif()

  foreach(target IN ITEMS sideboost sideboostd sideboost-node sideboost-qt sideboost-gui sideboost-cli sideboost-tx sideboost-util sideboost-wallet test_sideboost bench_sideboost)
    if(TARGET ${target})
      list(APPEND executables $<TARGET_FILE:${target}>)
    endif()
  endforeach()

  add_custom_target(check-symbols
    COMMAND ${CMAKE_COMMAND} -E echo "Running symbol and dynamic library checks..."
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/contrib/guix/symbol-check.py ${executables}
    VERBATIM
  )

  add_custom_target(check-security
    COMMAND ${CMAKE_COMMAND} -E echo "Checking binary security..."
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/contrib/guix/security-check.py ${executables}
    VERBATIM
  )
endfunction()

function(add_windows_deploy_target)
  configure_file(${PROJECT_SOURCE_DIR}/cmake/script/GenerateWindowsInstaller.cmake.in ${PROJECT_BINARY_DIR}/GenerateWindowsInstaller.cmake USE_SOURCE_PERMISSIONS @ONLY)
  if(MINGW AND TARGET sideboost AND TARGET sideboost-qt AND TARGET sideboostd AND TARGET sideboost-cli AND TARGET sideboost-tx AND TARGET sideboost-wallet AND TARGET sideboost-util AND TARGET test_sideboost)
    add_custom_command(
      OUTPUT ${PROJECT_BINARY_DIR}/sideboost-win64-setup.exe
      WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
      COMMAND ${CMAKE_COMMAND} -E make_directory release
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:sideboost> -o release/$<TARGET_FILE_NAME:sideboost>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:sideboost-qt> -o release/$<TARGET_FILE_NAME:sideboost-qt>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:sideboostd> -o release/$<TARGET_FILE_NAME:sideboostd>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:sideboost-cli> -o release/$<TARGET_FILE_NAME:sideboost-cli>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:sideboost-tx> -o release/$<TARGET_FILE_NAME:sideboost-tx>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:sideboost-wallet> -o release/$<TARGET_FILE_NAME:sideboost-wallet>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:sideboost-util> -o release/$<TARGET_FILE_NAME:sideboost-util>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:test_sideboost> -o release/$<TARGET_FILE_NAME:test_sideboost>
      COMMAND ${CMAKE_COMMAND} -D BIN_DIR=release -D LIBEXEC_DIR=release -P GenerateWindowsInstaller.cmake
    )
    add_custom_target(deploy DEPENDS ${PROJECT_BINARY_DIR}/bitcoin-win64-setup.exe)
  endif()
endfunction()

function(add_macos_deploy_target)
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND TARGET sideboost-qt)
    set(macos_app "Namecoin-Qt.app")
    # Populate Contents subdirectory.
    configure_file(${PROJECT_SOURCE_DIR}/share/qt/Info.plist.in ${macos_app}/Contents/Info.plist NO_SOURCE_PERMISSIONS)
    file(CONFIGURE OUTPUT ${macos_app}/Contents/PkgInfo CONTENT "APPL????")
    # Populate Contents/Resources subdirectory.
    file(CONFIGURE OUTPUT ${macos_app}/Contents/Resources/empty.lproj CONTENT "")
    configure_file(${PROJECT_SOURCE_DIR}/src/qt/res/icons/namecoin.icns ${macos_app}/Contents/Resources/namecoin.icns NO_SOURCE_PERMISSIONS COPYONLY)
    file(CONFIGURE OUTPUT ${macos_app}/Contents/Resources/Base.lproj/InfoPlist.strings
      CONTENT "{ CFBundleDisplayName = \"@CLIENT_NAME@\"; CFBundleName = \"@CLIENT_NAME@\"; }"
    )

    add_custom_command(
      OUTPUT ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/Namecoin-Qt
      COMMAND ${CMAKE_COMMAND} --install ${PROJECT_BINARY_DIR} --config $<CONFIG> --component sideboost-qt --prefix ${macos_app}/Contents/MacOS --strip
      COMMAND ${CMAKE_COMMAND} -E rename ${macos_app}/Contents/MacOS/bin/$<TARGET_FILE_NAME:sideboost-qt> ${macos_app}/Contents/MacOS/Namecoin-Qt
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${macos_app}/Contents/MacOS/bin
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${macos_app}/Contents/MacOS/share
      VERBATIM
    )

    string(REPLACE " " "-" osx_volname ${CLIENT_NAME})
    if(CMAKE_HOST_APPLE)
      add_custom_command(
        OUTPUT ${PROJECT_BINARY_DIR}/${osx_volname}.zip
        COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/contrib/macdeploy/macdeployqtplus ${macos_app} ${osx_volname} -translations-dir=${QT_TRANSLATIONS_DIR} -zip
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/Namecoin-Qt
        VERBATIM
      )
      add_custom_target(deploydir
        DEPENDS ${PROJECT_BINARY_DIR}/${osx_volname}.zip
      )
      add_custom_target(deploy
        DEPENDS ${PROJECT_BINARY_DIR}/${osx_volname}.zip
      )
    else()
      add_custom_command(
        OUTPUT ${PROJECT_BINARY_DIR}/dist/${macos_app}/Contents/MacOS/Namecoin-Qt
        COMMAND ${CMAKE_COMMAND} -E env OBJDUMP=${CMAKE_OBJDUMP} $<TARGET_FILE:Python3::Interpreter> ${PROJECT_SOURCE_DIR}/contrib/macdeploy/macdeployqtplus ${macos_app} ${osx_volname} -translations-dir=${QT_TRANSLATIONS_DIR}
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/Namecoin-Qt
        VERBATIM
      )
      add_custom_target(deploydir
        DEPENDS ${PROJECT_BINARY_DIR}/dist/${macos_app}/Contents/MacOS/Namecoin-Qt
      )

      find_program(ZIP_EXECUTABLE zip)
      if(NOT ZIP_EXECUTABLE)
        add_custom_target(deploy
          COMMAND ${CMAKE_COMMAND} -E echo "Error: ZIP not found"
        )
      else()
        add_custom_command(
          OUTPUT ${PROJECT_BINARY_DIR}/dist/${osx_volname}.zip
          WORKING_DIRECTORY dist
          COMMAND ${PROJECT_SOURCE_DIR}/cmake/script/macos_zip.sh ${ZIP_EXECUTABLE} ${osx_volname}.zip
          VERBATIM
        )
        add_custom_target(deploy
          DEPENDS ${PROJECT_BINARY_DIR}/dist/${osx_volname}.zip
        )
      endif()
    endif()
    add_dependencies(deploydir sideboost-qt)
    add_dependencies(deploy deploydir)
  endif()
endfunction()
