cmake_minimum_required(VERSION 3.10)
project(ESDEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Search for Python3 (required for backend)
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

# The single webview.h needs to be in engine/ folder or included properly
set(CMAKE_INCLUDE_CURRENT_DIR ON)

add_executable(ESDEngine engine/main.cpp)

# Link C++ and Python
target_link_libraries(ESDEngine Python3::Python)

# Link C++ Webview requirements dynamically based on OS
if(WIN32)
    # Add the local extracted WebView2 headers so "WebView2.h" resolves
    target_include_directories(ESDEngine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/engine/webview2)
    
    # We dynamically load Webview2 on Windows via COM, but core Windows libs are still needed
    target_link_libraries(ESDEngine setupapi pathcch)
elseif(APPLE)
    target_link_libraries(ESDEngine "-framework WebKit" "-framework Cocoa")
elseif(UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBKIT2GTK REQUIRED webkit2gtk-4.0)
    target_link_libraries(ESDEngine ${WEBKIT2GTK_LIBRARIES})
    target_include_directories(ESDEngine PRIVATE ${WEBKIT2GTK_INCLUDE_DIRS})
endif()
