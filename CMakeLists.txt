cmake_minimum_required(VERSION 3.23)
project(astrothumbnail)
set(CMAKE_CXX_STANDARD 17)
set(QT_DEFAULT_MAJOR_VERSION 6)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

# Detect Qt6's actual plugin dir before KDEInstallDirs locks it in.
# On Arch/CachyOS, ECM resolves KDE_INSTALL_QTPLUGINDIR to /usr/lib/qt/plugins
# but Qt6 itself lives under /usr/lib/qt6/plugins.
find_package(Qt6 REQUIRED COMPONENTS Core Gui)
get_filename_component(_qt6_prefix "${Qt6Core_DIR}/../../.." ABSOLUTE)
if(EXISTS "${_qt6_prefix}/plugins" AND
   NOT "${_qt6_prefix}/plugins" STREQUAL "${KDE_INSTALL_QTPLUGINDIR}")
    set(KDE_INSTALL_QTPLUGINDIR "${_qt6_prefix}/plugins"
        CACHE PATH "Qt6 plugin install directory" FORCE)
endif()

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

find_package(KF6 REQUIRED COMPONENTS KIO CoreAddons)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CFITSIO REQUIRED cfitsio)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

kcoreaddons_add_plugin(astrothumbnail
    SOURCES astrothumbnail.cpp
    INSTALL_NAMESPACE "kf6/thumbcreator"
)
target_include_directories(astrothumbnail PRIVATE ${CFITSIO_INCLUDE_DIRS})
target_link_libraries(astrothumbnail PRIVATE
    Qt6::Core Qt6::Gui
    KF6::KIOGui
    ${CFITSIO_LIBRARIES}
)
