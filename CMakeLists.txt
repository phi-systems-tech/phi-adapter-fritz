cmake_minimum_required(VERSION 3.21)

project(phi-adapter-fritz
    VERSION 1.0.0
    DESCRIPTION "Phi FRITZ!Box adapter plugin"
    LANGUAGES CXX
)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# No Qt. The loop, the timers and the strings come from phi-runtime through the
# SDK; HTTP with digest authentication from the SDK's own client; JSON from
# nlohmann, which is what phi-core uses.
find_package(nlohmann_json 3.11 REQUIRED)

option(PHI_ADAPTER_FRITZ_BUILD_IPC "Build IPC sidecar executable for fritz adapter" ON)

set(PHI_ADAPTER_SDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../phi-adapter-sdk" CACHE PATH
    "Path to local phi-adapter-sdk checkout. If not found, find_package(phi-adapter-sdk) is used."
)
option(PHI_ADAPTER_FRITZ_USE_LOCAL_ADAPTER_SDK
    "Use local ../phi-adapter-sdk checkout when available" ON)

if(PHI_ADAPTER_FRITZ_BUILD_IPC)
    if(NOT TARGET phi::adapter-sdk)
        if(PHI_ADAPTER_FRITZ_USE_LOCAL_ADAPTER_SDK AND EXISTS "${PHI_ADAPTER_SDK_SOURCE_DIR}/CMakeLists.txt")
            message(STATUS "Using local phi-adapter-sdk from: ${PHI_ADAPTER_SDK_SOURCE_DIR}")
            set(PHI_ADAPTER_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
            add_subdirectory("${PHI_ADAPTER_SDK_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/phi-adapter-sdk")
        else()
            message(STATUS "Using installed phi-adapter-sdk via find_package")
            find_package(phi-adapter-sdk CONFIG REQUIRED)
        endif()
    endif()

    if(NOT TARGET phi::adapter-sdk)
        message(FATAL_ERROR "phi-adapter-sdk target phi::adapter-sdk not found")
    endif()

    # One concern per translation unit; everything above the session is pure
    # enough to test without a router.
    add_library(phi_adapter_fritz_core STATIC
        src/fritz_devicemodel.cpp
        src/fritz_instance.cpp
        src/fritz_json.cpp
        src/fritz_probe.cpp
        src/fritz_schema.cpp
        src/fritz_session.cpp
        src/fritz_soap.cpp
        src/fritz_state.cpp
        src/fritz_tr064.cpp
    )
    target_compile_features(phi_adapter_fritz_core PUBLIC cxx_std_20)
    set_target_properties(phi_adapter_fritz_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(phi_adapter_fritz_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(phi_adapter_fritz_core
        PUBLIC
            phi::adapter-sdk
            nlohmann_json::nlohmann_json
    )

    add_executable(phi_adapter_fritz_ipc src/main.cpp)
    target_link_libraries(phi_adapter_fritz_ipc PRIVATE phi_adapter_fritz_core)

    enable_testing()

    # The TR-064 vocabulary and the SOAP it is carried in.
    add_executable(fritz_tr064_tests tests/fritz_tr064_tests.cpp)
    target_link_libraries(fritz_tr064_tests PRIVATE phi_adapter_fritz_core)
    add_test(NAME fritz_tr064_tests COMMAND fritz_tr064_tests)

    # What has already been said, and what a router turns out to implement.
    add_executable(fritz_behaviour_tests tests/fritz_behaviour_tests.cpp)
    target_link_libraries(fritz_behaviour_tests PRIVATE phi_adapter_fritz_core)
    add_test(NAME fritz_behaviour_tests COMMAND fritz_behaviour_tests)

    set_tests_properties(fritz_tr064_tests fritz_behaviour_tests PROPERTIES TIMEOUT 120)

    set_target_properties(phi_adapter_fritz_ipc PROPERTIES
        OUTPUT_NAME "phi_adapter_fritz_ipc"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/adapters"
    )

    install(TARGETS phi_adapter_fritz_ipc
        RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/phi/plugins/adapters
    )

    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/fritz-config.json"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/phi/plugins/adapters
        RENAME fritz-config.json
    )
endif()
