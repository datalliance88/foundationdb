################################################################################
# Helper Functions
################################################################################

function(install_symlink_impl)
  if (NOT WIN32)
    set(options "")
    set(one_value_options TO DESTINATION)
    set(multi_value_options COMPONENTS)
    cmake_parse_arguments(SYM "${options}" "${one_value_options}" "${multi_value_options}" "${ARGN}")

    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/symlinks)
    get_filename_component(fname ${SYM_DESTINATION} NAME)
    get_filename_component(dest_dir ${SYM_DESTINATION} DIRECTORY)
    set(sl ${CMAKE_CURRENT_BINARY_DIR}/symlinks/${fname})
    execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink ${SYM_TO} ${sl})
    foreach(component IN LISTS SYM_COMPONENTS)
      install(FILES ${sl} DESTINATION ${dest_dir} COMPONENT ${component})
    endforeach()
  endif()
endfunction()

function(install_symlink)
  if(NOT WIN32 AND NOT OPEN_FOR_IDE)
    set(options "")
    set(one_value_options COMPONENT LINK_DIR FILE_DIR LINK_NAME FILE_NAME)
    set(multi_value_options "")
    cmake_parse_arguments(IN "${options}" "${one_value_options}" "${multi_value_options}" "${ARGN}")

    set(rel_path "")
    string(REGEX MATCHALL "\\/" slashes "${IN_LINK_NAME}")
    foreach(ignored IN LISTS slashes)
      set(rel_path "../${rel_path}")
    endforeach()
    if("${IN_FILE_DIR}" MATCHES "bin")
      if("${IN_LINK_DIR}" MATCHES "lib")
        install_symlink_impl(
          TO "../${rel_path}bin/${IN_FILE_NAME}"
          DESTINATION "lib/${IN_LINK_NAME}"
          COMPONENTS "${IN_COMPONENT}-tgz")
        install_symlink_impl(
          TO "../${rel_path}bin/${IN_FILE_NAME}"
          DESTINATION "usr/lib64/${IN_LINK_NAME}"
          COMPONENTS "${IN_COMPONENT}-el6"
                     "${IN_COMPONENT}-el7"
                     "${IN_COMPONENT}-deb")
        install_symlink_impl(
          TO "../${rel_path}bin/${IN_FILE_NAME}"
          DESTINATION "usr/lib64/${IN_LINK_NAME}"
          COMPONENTS "${IN_COMPONENT}-deb")
      elseif("${IN_LINK_DIR}" MATCHES "bin")
        install_symlink_impl(
          TO "../${rel_path}bin/${IN_FILE_NAME}"
          DESTINATION "bin/${IN_LINK_NAME}"
          COMPONENTS "${IN_COMPONENT}-tgz")
        install_symlink_impl(
          TO "../${rel_path}bin/${IN_FILE_NAME}"
          DESTINATION "usr/bin/${IN_LINK_NAME}"
          COMPONENTS "${IN_COMPONENT}-el6"
                     "${IN_COMPONENT}-el7"
                     "${IN_COMPONENT}-deb")
      elseif("${IN_LINK_DIR}" MATCHES "fdbmonitor")
        install_symlink_impl(
          TO "../../${rel_path}bin/${IN_FILE_NAME}"
          DESTINATION "lib/foundationdb/${IN_LINK_NAME}"
          COMPONENTS "${IN_COMPONENT}-tgz")
        install_symlink_impl(
          TO "../../${rel_path}bin/${IN_FILE_NAME}"
          DESTINATION "usr/lib/foundationdb/${IN_LINK_NAME}"
          COMPONENTS "${IN_COMPONENT}-el6"
                     "${IN_COMPONENT}-el7"
                     "${IN_COMPONENT}-deb")
      else()
        message(FATAL_ERROR "Unknown LINK_DIR ${IN_LINK_DIR}")
      endif()
    else()
      message(FATAL_ERROR "Unknown FILE_DIR ${IN_FILE_DIR}")
    endif()
  endif()
endfunction()

# 'map' from (destination, package) to path
# format vars like install_destination_for_${destination}_${package}
set(install_destination_for_bin_tgz "bin")
set(install_destination_for_bin_deb "usr/bin")
set(install_destination_for_bin_el6 "usr/bin")
set(install_destination_for_bin_el7 "usr/bin")
set(install_destination_for_bin_pm "usr/local/bin")
set(install_destination_for_sbin_tgz "sbin")
set(install_destination_for_sbin_deb "usr/sbin")
set(install_destination_for_sbin_el6 "usr/sbin")
set(install_destination_for_sbin_el7 "usr/sbin")
set(install_destination_for_sbin_pm "usr/local/libexec")
set(install_destination_for_lib_tgz "lib")
set(install_destination_for_lib_deb "usr/lib")
set(install_destination_for_lib_el6 "usr/lib64")
set(install_destination_for_lib_el7 "usr/lib64")
set(install_destination_for_lib_pm "lib")
set(install_destination_for_fdbmonitor_tgz "libexec")
set(install_destination_for_fdbmonitor_deb "usr/lib/foundationdb")
set(install_destination_for_fdbmonitor_el6 "usr/lib/foundationdb")
set(install_destination_for_fdbmonitor_el7 "usr/lib/foundationdb")
set(install_destination_for_fdbmonitor_pm "usr/local/libexec")
set(install_destination_for_include_tgz "include")
set(install_destination_for_include_deb "usr/include")
set(install_destination_for_include_el6 "usr/include")
set(install_destination_for_include_el7 "usr/include")
set(install_destination_for_include_pm "usr/local/include")
set(install_destination_for_etc_tgz "etc/foundationdb")
set(install_destination_for_etc_deb "etc/foundationdb")
set(install_destination_for_etc_el6 "etc/foundationdb")
set(install_destination_for_etc_el7 "etc/foundationdb")
set(install_destination_for_etc_pm "usr/local/etc/foundationdb")
set(install_destination_for_log_tgz "log/foundationdb")
set(install_destination_for_log_deb "var/log/foundationdb")
set(install_destination_for_log_el6 "var/log/foundationdb")
set(install_destination_for_log_el7 "var/log/foundationdb")
set(install_destination_for_log_pm "")
set(install_destination_for_data_tgz "lib/foundationdb")
set(install_destination_for_data_deb "var/lib/foundationdb")
set(install_destination_for_data_el6 "var/lib/foundationdb")
set(install_destination_for_data_el7 "var/lib/foundationdb")
set(install_destination_for_data_pm "")

set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
function(fdb_configure_and_install)
  if(NOT WIN32 AND NOT OPEN_FOR_IDE)
    set(one_value_options COMPONENT DESTINATION FILE DESTINATION_SUFFIX)
    cmake_parse_arguments(IN "${options}" "${one_value_options}" "${multi_value_options}" "${ARGN}")
    foreach(package tgz deb el6 el7 pm)
      set(INCLUDE_DIR "${install_destination_for_include_${package}}")
      set(LIB_DIR "${install_destination_for_lib_${package}}")
      set(install_path "${install_destination_for_${IN_DESTINATION}_${package}}")
      string(REGEX REPLACE "\.in$" "" name "${IN_FILE}")
      get_filename_component(name "${name}" NAME)
      set(generated_file_name "${generated_dir}/${package}/${name}")
      configure_file("${IN_FILE}" "${generated_file_name}" @ONLY)
      install(
        FILES "${generated_file_name}"
        DESTINATION "${install_path}${IN_DESTINATION_SUFFIX}"
        COMPONENT "${IN_COMPONENT}-${package}")
    endforeach()
  endif()
endfunction()

function(fdb_install)
  if(NOT WIN32 AND NOT OPEN_FOR_IDE)
    set(one_value_options COMPONENT DESTINATION EXPORT DESTINATION_SUFFIX)
    set(multi_value_options TARGETS FILES DIRECTORY)
    cmake_parse_arguments(IN "${options}" "${one_value_options}" "${multi_value_options}" "${ARGN}")

    set(install_export 0)
    if(IN_TARGETS)
      set(args TARGETS ${IN_TARGETS})
    elseif(IN_FILES)
      set(args FILES ${IN_FILES})
    elseif(IN_DIRECTORY)
      set(args DIRECTORY ${IN_DIRECTORY})
    elseif(IN_EXPORT)
      set(install_export 1)
    else()
      message(FATAL_ERROR "Expected FILES or TARGETS")
    endif()
    foreach(package tgz deb el6 el7 pm)
      set(install_path "${install_destination_for_${IN_DESTINATION}_${package}}")
      if(install_export)
        install(
          EXPORT "${IN_EXPORT}-${package}"
          DESTINATION "${install_path}${IN_DESTINATION_SUFFIX}"
          FILE "${IN_EXPORT}.cmake"
          COMPONENT "${IN_COMPONENT}-${package}")
      else()
        set(export_args "")
        if (IN_EXPORT)
          set(export_args EXPORT "${IN_EXPORT}-${package}")
        endif()
        if(NOT ${install_path} STREQUAL "")
          install(
            ${args}
            ${export_args}
            DESTINATION "${install_path}${IN_DESTINATION_SUFFIX}"
            COMPONENT "${IN_COMPONENT}-${package}")
        endif()
      endif()
    endforeach()
  endif()
endfunction()

if(APPLE)
  set(CPACK_GENERATOR TGZ productbuild)
else()
  set(CPACK_GENERATOR RPM DEB TGZ)
endif()


set(CPACK_PACKAGE_CHECKSUM SHA256)
configure_file("${PROJECT_SOURCE_DIR}/cmake/CPackConfig.cmake" "${PROJECT_BINARY_DIR}/packaging/CPackConfig.cmake")
set(CPACK_PROJECT_CONFIG_FILE "${PROJECT_BINARY_DIR}/packaging/CPackConfig.cmake")

################################################################################
# Version information
################################################################################

string(REPLACE "." ";" FDB_VERSION_LIST ${FDB_VERSION_PLAIN})
list(GET FDB_VERSION_LIST 0 FDB_MAJOR)
list(GET FDB_VERSION_LIST 1 FDB_MINOR)
list(GET FDB_VERSION_LIST 2 FDB_PATCH)

################################################################################
# General CPack configuration
################################################################################

include(InstallRequiredSystemLibraries)
set(CPACK_PACKAGE_NAME "foundationdb")
set(CPACK_PACKAGE_VENDOR "FoundationDB")
set(CPACK_PACKAGE_CONTACT "fdb-dist@apple.com")
set(CPACK_PACKAGE_VERSION_MAJOR ${FDB_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${FDB_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${FDB_PATCH})
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${FDB_VERSION}-${CPACK_SYSTEM_NAME}")
set(CPACK_OUTPUT_FILE_PREFIX "${PROJECT_BINARY_DIR}/packages")
set(CPACK_PACKAGE_DESCRIPTION_FILE ${PROJECT_SOURCE_DIR}/packaging/description)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
  "FoundationDB is a scalable, fault-tolerant, ordered key-value store with full ACID transactions.")
set(CPACK_PACKAGE_ICON ${PROJECT_SOURCE_DIR}/packaging/foundationdb.ico)
set(CPACK_PACKAGE_CONTACT "The FoundationDB Community")

set(CPACK_COMPONENT_SERVER-EL6_DEPENDS clients-el6)
set(CPACK_COMPONENT_SERVER-EL7_DEPENDS clients-el7)
set(CPACK_COMPONENT_SERVER-DEB_DEPENDS clients-deb)
set(CPACK_COMPONENT_SERVER-TGZ_DEPENDS clients-tgz)
set(CPACK_COMPONENT_SERVER-PM_DEPENDS clients-pm)

set(CPACK_COMPONENT_SERVER-EL6_DISPLAY_NAME "foundationdb-server")
set(CPACK_COMPONENT_SERVER-EL7_DISPLAY_NAME "foundationdb-server")
set(CPACK_COMPONENT_SERVER-DEB_DISPLAY_NAME "foundationdb-server")
set(CPACK_COMPONENT_SERVER-TGZ_DISPLAY_NAME "foundationdb-server")
set(CPACK_COMPONENT_SERVER-PM_DISPLAY_NAME "foundationdb-server")

set(CPACK_COMPONENT_CLIENTS-EL6_DISPLAY_NAME "foundationdb-clients")
set(CPACK_COMPONENT_CLIENTS-EL7_DISPLAY_NAME "foundationdb-clients")
set(CPACK_COMPONENT_CLIENTS-DEB_DISPLAY_NAME "foundationdb-clients")
set(CPACK_COMPONENT_CLIENTS-TGZ_DISPLAY_NAME "foundationdb-clients")
set(CPACK_COMPONENT_CLIENTS-PM_DISPLAY_NAME "foundationdb-clients")


# MacOS needs a file exiension for the LICENSE file
configure_file(${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_BINARY_DIR}/License.txt COPYONLY)

################################################################################
# Filename of packages
################################################################################

if(NOT FDB_RELEASE)
  set(prerelease_string ".PRERELEASE")
endif()
set(clients-filename "foundationdb-clients-${PROJECT_VERSION}.${CURRENT_GIT_VERSION}${prerelease_string}")
set(server-filename "foundationdb-server-${PROJECT_VERSION}.${CURRENT_GIT_VERSION}${prerelease_string}")

################################################################################
# Configuration for RPM
################################################################################

set(CPACK_RPM_PACKAGE_LICENSE "Apache 2.0")

set(CPACK_RPM_PACKAGE_NAME "foundationdb")
set(CPACK_RPM_CLIENTS-EL6_PACKAGE_NAME "foundationdb-clients")
set(CPACK_RPM_CLIENTS-EL7_PACKAGE_NAME "foundationdb-clients")
set(CPACK_RPM_SERVER-EL6_PACKAGE_NAME "foundationdb-server")
set(CPACK_RPM_SERVER-EL7_PACKAGE_NAME "foundationdb-server")

set(CPACK_RPM_CLIENTS-EL6_FILE_NAME "${clients-filename}.el6.x86_64.rpm")
set(CPACK_RPM_CLIENTS-EL7_FILE_NAME "${clients-filename}.el7.x86_64.rpm")
set(CPACK_RPM_SERVER-EL6_FILE_NAME "${server-filename}.el6.x86_64.rpm")
set(CPACK_RPM_SERVER-EL7_FILE_NAME "${server-filename}.el7.x86_64.rpm")

set(CPACK_RPM_CLIENTS-EL6_DEBUGINFO_FILE_NAME "${clients-filename}.el6-debuginfo.x86_64.rpm")
set(CPACK_RPM_CLIENTS-EL7_DEBUGINFO_FILE_NAME "${clients-filename}.el7-debuginfo.x86_64.rpm")
set(CPACK_RPM_SERVER-EL6_DEBUGINFO_FILE_NAME "${server-filename}.el6-debuginfo.x86_64.rpm")
set(CPACK_RPM_SERVER-EL7_DEBUGINFO_FILE_NAME "${server-filename}.el7-debuginfo.x86_64.rpm")

file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/packaging/emptydir")
fdb_install(DIRECTORY "${PROJECT_BINARY_DIR}/packaging/emptydir/" DESTINATION data COMPONENT server)
fdb_install(DIRECTORY "${PROJECT_BINARY_DIR}/packaging/emptydir/" DESTINATION log COMPONENT server)
fdb_install(DIRECTORY "${PROJECT_BINARY_DIR}/packaging/emptydir/" DESTINATION etc COMPONENT clients)

set(CPACK_RPM_SERVER-EL6_USER_FILELIST
  "%config(noreplace) /etc/foundationdb/foundationdb.conf"
  "%attr(0700,foundationdb,foundationdb) /var/log/foundationdb"
  "%attr(0700, foundationdb, foundationdb) /var/lib/foundationdb")
set(CPACK_RPM_SERVER-EL7_USER_FILELIST
  "%config(noreplace) /etc/foundationdb/foundationdb.conf"
  "%attr(0700,foundationdb,foundationdb) /var/log/foundationdb"
  "%attr(0700, foundationdb, foundationdb) /var/lib/foundationdb")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
  "/usr/sbin"
  "/usr/share/java"
  "/usr/lib64/python2.7"
  "/usr/lib64/python2.7/site-packages"
  "/var"
  "/var/log"
  "/var/lib"
  "/lib"
  "/lib/systemd"
  "/lib/systemd/system"
  "/etc/rc.d/init.d")
set(CPACK_RPM_DEBUGINFO_PACKAGE ON)
#set(CPACK_RPM_BUILD_SOURCE_DIRS_PREFIX /usr/src)
set(CPACK_RPM_COMPONENT_INSTALL ON)

set(CPACK_RPM_CLIENTS-EL6_PRE_INSTALL_SCRIPT_FILE
  ${PROJECT_SOURCE_DIR}/packaging/rpm/scripts/preclients.sh)
set(CPACK_RPM_clients-el7_PRE_INSTALL_SCRIPT_FILE
  ${PROJECT_SOURCE_DIR}/packaging/rpm/scripts/preclients.sh)

set(CPACK_RPM_CLIENTS-EL6_POST_INSTALL_SCRIPT_FILE
  ${PROJECT_SOURCE_DIR}/packaging/rpm/scripts/postclients.sh)
set(CPACK_RPM_CLIENTS-EL7_POST_INSTALL_SCRIPT_FILE
  ${PROJECT_SOURCE_DIR}/packaging/rpm/scripts/postclients.sh)

set(CPACK_RPM_SERVER-EL6_PRE_INSTALL_SCRIPT_FILE
  ${PROJECT_SOURCE_DIR}/packaging/rpm/scripts/preserver.sh)
set(CPACK_RPM_SERVER-EL7_PRE_INSTALL_SCRIPT_FILE
  ${PROJECT_SOURCE_DIR}/packaging/rpm/scripts/preserver.sh)

set(CPACK_RPM_SERVER-EL6_POST_INSTALL_SCRIPT_FILE
  ${PROJECT_SOURCE_DIR}/packaging/rpm/scripts/postserver-el6.sh)
set(CPACK_RPM_SERVER-EL7_POST_INSTALL_SCRIPT_FILE
  ${PROJECT_SOURCE_DIR}/packaging/rpm/scripts/postserver.sh)

set(CPACK_RPM_SERVER-EL6_PRE_UNINSTALL_SCRIPT_FILE
  ${PROJECT_SOURCE_DIR}/packaging/rpm/scripts/preunserver.sh)
set(CPACK_RPM_SERVER-EL7_PRE_UNINSTALL_SCRIPT_FILE
  ${PROJECT_SOURCE_DIR}/packaging/rpm/scripts/preunserver.sh)

set(CPACK_RPM_SERVER-EL6_PACKAGE_REQUIRES
  "foundationdb-clients = ${FDB_MAJOR}.${FDB_MINOR}.${FDB_PATCH}")
set(CPACK_RPM_SERVER-EL7_PACKAGE_REQUIRES
  "foundationdb-clients = ${FDB_MAJOR}.${FDB_MINOR}.${FDB_PATCH}")
#set(CPACK_RPM_java_PACKAGE_REQUIRES
#  "foundationdb-clients = ${FDB_MAJOR}.${FDB_MINOR}.${FDB_PATCH}")
#set(CPACK_RPM_python_PACKAGE_REQUIRES
#  "foundationdb-clients = ${FDB_MAJOR}.${FDB_MINOR}.${FDB_PATCH}")

################################################################################
# Configuration for DEB
################################################################################

set(CPACK_DEBIAN_CLIENTS-DEB_FILE_NAME "${clients-filename}_amd64.deb")
set(CPACK_DEBIAN_SERVER-DEB_FILE_NAME "${server-filename}_amd64.deb")
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_DEBUGINFO_PACKAGE ON)
set(CPACK_DEBIAN_PACKAGE_SECTION "database")
set(CPACK_DEBIAN_ENABLE_COMPONENT_DEPENDS ON)

set(CPACK_DEBIAN_SERVER-DEB_PACKAGE_NAME "foundationdb-server")
set(CPACK_DEBIAN_CLIENTS-DEB_PACKAGE_NAME "foundationdb-clients")

set(CPACK_DEBIAN_SERVER-DEB_PACKAGE_DEPENDS "adduser, libc6 (>= 2.12), foundationdb-clients (= ${FDB_VERSION})")
set(CPACK_DEBIAN_SERVER-DEB_PACKAGE_RECOMMENDS "python (>= 2.6)")
set(CPACK_DEBIAN_CLIENTS-DEB_PACKAGE_DEPENDS "adduser, libc6 (>= 2.12)")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://www.foundationdb.org")
set(CPACK_DEBIAN_CLIENTS-DEB_PACKAGE_CONTROL_EXTRA
  ${PROJECT_SOURCE_DIR}/packaging/deb/DEBIAN-foundationdb-clients/postinst)
set(CPACK_DEBIAN_SERVER-DEB_PACKAGE_CONTROL_EXTRA
  ${PROJECT_SOURCE_DIR}/packaging/deb/DEBIAN-foundationdb-server/conffiles
  ${PROJECT_SOURCE_DIR}/packaging/deb/DEBIAN-foundationdb-server/preinst
  ${PROJECT_SOURCE_DIR}/packaging/deb/DEBIAN-foundationdb-server/postinst
  ${PROJECT_SOURCE_DIR}/packaging/deb/DEBIAN-foundationdb-server/prerm
  ${PROJECT_SOURCE_DIR}/packaging/deb/DEBIAN-foundationdb-server/postrm)

################################################################################
# MacOS configuration
################################################################################

if(APPLE)
  install(PROGRAMS ${PROJECT_SOURCE_DIR}/packaging/osx/uninstall-FoundationDB.sh
    DESTINATION "usr/local/foundationdb"
    COMPONENT clients-pm)
  install(FILES ${PROJECT_SOURCE_DIR}/packaging/osx/com.foundationdb.fdbmonitor.plist
    DESTINATION "Library/LaunchDaemons"
    COMPONENT server-pm)
endif()

################################################################################
# Configuration for DEB
################################################################################

set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_ARCHIVE_CLIENTS-TGZ_FILE_NAME "${clients-filename}.x86_64")
set(CPACK_ARCHIVE_SERVER-TGZ_FILE_NAME "${server-filename}.x86_64")

################################################################################
# Server configuration
################################################################################

string(RANDOM LENGTH 8 description1)
string(RANDOM LENGTH 8 description2)
set(CLUSTER_DESCRIPTION1 ${description1} CACHE STRING "Cluster description")
set(CLUSTER_DESCRIPTION2 ${description2} CACHE STRING "Cluster description")

if(NOT WIN32)
  install(FILES ${PROJECT_SOURCE_DIR}/packaging/osx/foundationdb.conf.new
    DESTINATION "usr/local/etc"
    COMPONENT server-pm)
  fdb_install(FILES ${PROJECT_SOURCE_DIR}/packaging/foundationdb.conf
    DESTINATION etc
    COMPONENT server)
  install(FILES ${PROJECT_SOURCE_DIR}/packaging/argparse.py
    DESTINATION "usr/lib/foundationdb"
    COMPONENT server-el6)
  install(FILES ${PROJECT_SOURCE_DIR}/packaging/make_public.py
    DESTINATION "usr/lib/foundationdb"
    COMPONENT server-el6)
  install(FILES ${PROJECT_SOURCE_DIR}/packaging/argparse.py
    DESTINATION "usr/lib/foundationdb"
    COMPONENT server-deb)
  install(FILES ${PROJECT_SOURCE_DIR}/packaging/make_public.py
    DESTINATION "usr/lib/foundationdb"
    COMPONENT server-deb)
  install(FILES ${PROJECT_SOURCE_DIR}/packaging/rpm/foundationdb.service
    DESTINATION "lib/systemd/system"
    COMPONENT server-el7)
  install(PROGRAMS ${PROJECT_SOURCE_DIR}/packaging/rpm/foundationdb-init
    DESTINATION "etc/rc.d/init.d"
    RENAME "foundationdb"
    COMPONENT server-el6)
  install(PROGRAMS ${PROJECT_SOURCE_DIR}/packaging/deb/foundationdb-init
    DESTINATION "etc/init.d"
    RENAME "foundationdb"
    COMPONENT server-deb)
endif()
