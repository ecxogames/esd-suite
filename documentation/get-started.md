# Getting Started with ESD Suite

Welcome to the Ecxo Softwares Development (ESD) Suite! This framework is a cross-platform desktop application engine that bridges C++, Python, and JavaScript.

## Architecture Overview

The application is built on three core pillars:
1. **C++ (Engine Layer):** Handles native system interactions, window creation, and embeds both the webview and the Python runtime.
2. **Python (Backend Layer):** Serves as the application's backend logic, scripting engine, and AI integration point.
3. **JavaScript/HTML/CSS (Frontend Layer):** Powers the user interface, rendering inside the embedded webview.

### The Communication Bridge

The core of the engine is an Inter-Process Communication (IPC) bridge that routes data:
`JavaScript ↔ C++ ↔ Python`

- **Frontend to Backend:** JS calls an injected global function (`window.invokeBridge`), sending a JSON string. C++ intercepts this and passes it to the embedded Python interpreter.
- **Backend to Frontend:** Python processes the request and returns a JSON string to C++. C++ then resolves the original JS Promise with this response.

## Directory Structure

* `/engine/` - Core C++ runtime code (Entry point, window creation, webview integration).
* `/server/` - Python backend logic. Treated as private during compilation.
* `/ui/` - User interface code (HTML/CSS/JS entry points and components).
* `/public/` - Shared assets accessible by all layers.
* `/private/` - Restricted code (C++, JS, Python) accessible only via secure imports.
* `/scripts/` - Utility scripts for development and building.

## Build Requirements

To build the prototype, you need:
- CMake (3.10+)
- A C++ Compiler (MSVC on Windows, Clang/GCC on macOS/Linux)
- Python 3 (installed on the system with development headers/libs)
- Webview Dependencies:
  - **Windows:** Microsoft Edge WebView2 SDK
  - **macOS:** WebKit/Cocoa (built-in)
  - **Linux:** WebKit2GTK

## Setting Up the Prototype

### 1. Fetch Dependencies
First, download the required lightweight `webview.h` header:
```bash
python scripts/setup_deps.py
```

### 2. Configure CMake
Create a build directory and configure the project. (On Windows, ensure you have the WebView2 SDK available, e.g., via vcpkg).
```bash
mkdir build
cd build
cmake ..
```

### 3. Build & Run
Compile the application:
```bash
cmake --build .
```

Run the executable from the project root (so it can find the `/server` and `/ui` folders):
```bash
# Windows
.\build\Debug\ESDEngine.exe

# macOS/Linux
./build/ESDEngine
```

When the window opens, click the button to test the `JS -> C++ -> Python` communication bridge!