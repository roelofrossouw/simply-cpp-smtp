cmake_minimum_required(VERSION 3.22)
set(SC_MODULE smtp)
project(simply-cpp-${SC_MODULE} HOMEPAGE_URL "https://github.com/roelofrossouw/simply-cpp-smtp" LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_SMTP_BUILD_TESTS "Build simply-cpp-smtp tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

# sc::params for the example; libcurl (curl license) speaks SMTP, STARTTLS and SMTPS, from the system.
find_or_install_package(sc-core simply-cpp-dev simply-cpp 3.0.0)
find_or_install_package(CURL libcurl4-openssl-dev curl)

set(SOURCE_OBJECTS)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

# libcurl by name, as sc-http links it: CURL::libcurl's include directory (the macOS SDK's usr/include) would come
# before libc++'s headers in every module using sc-smtp.
add_sc_object(smtp LINK_LIBRARIES sc::sc-core curl PUBLIC_LINK_LIBRARIES sc::sc-core curl)
# The probe (RCPT TO without DATA): plain sockets, and the resolver library for MX records (libresolv, in libc's package).
add_sc_object(probe LINK_LIBRARIES resolv PUBLIC_LINK_LIBRARIES resolv)

add_sc_libraries(DESCRIPTION "simply-cpp SMTP client on libcurl: send mail to a relay, STARTTLS/TLS and AUTH, no sendmail; probe whether an address is taken (RCPT TO, no DATA)")
target_compile_features(sc-smtp PUBLIC cxx_std_20)
target_compile_features(sc-smtp-shared PUBLIC cxx_std_20)

install_sc_module(PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
install(FILES cmake/SimplyCppFunctions.cmake cmake/sc_bootstrap.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sc-smtp
        COMPONENT development)

if (SC_SMTP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests sc-smtp-test)
endif ()

add_sc_example(sc-smtp-demo)

package_sc_module(DEV_DEPENDS "simply-cpp-dev (>= 3.0.0~)" libcurl4-openssl-dev)
