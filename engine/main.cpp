#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

// Workaround to prevent MSVC from searching for the Python debug library (pythonXX_d.lib)
#if defined(_MSC_VER) && defined(_DEBUG)
    #undef _DEBUG
    #include <Python.h>
    #define _DEBUG
#else
    #include <Python.h>
#endif

#include "webview.h"

#ifdef _WIN32
#include <direct.h>
#define GetCurrentDir _getcwd
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#else
#include <unistd.h>
#define GetCurrentDir getcwd
#endif

// Helper to get current directory and resolve project root
std::string GetCurrentWorkingDir() {
    char buff[FILENAME_MAX];
    GetCurrentDir(buff, FILENAME_MAX);
    std::string cwd(buff);
    
    // If run from within the build directory, strip it to reach the project root.
    size_t buildPos = cwd.find("\\build");
    if (buildPos == std::string::npos) buildPos = cwd.find("/build");
    if (buildPos != std::string::npos) {
        cwd = cwd.substr(0, buildPos);
    }
    return cwd;
}

// Helper to load properties.config
std::unordered_map<std::string, std::string> LoadConfig(const std::string& filename) {
    std::unordered_map<std::string, std::string> config;
    std::ifstream file(filename);
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        // Trim carriage return if present (Windows CRLF issue)
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        auto delimiterPos = line.find('=');
        if (delimiterPos != std::string::npos) {
            std::string key = line.substr(0, delimiterPos);
            std::string value = line.substr(delimiterPos + 1);
            config[key] = value;
        }
    }
    return config;
}

#ifdef _WIN32
// Tell MSVC linker to build a Windows GUI app (no console by default) but keep main() as the entrypoint.
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#endif

// Function to route a string payload to Python's /server/api.py
std::string CallPythonBackend(const std::string& message) {
    std::string resultStr = "{\"error\": \"Failed to call Python\"}";
    
    // Import the API module
    PyObject* pName = PyUnicode_DecodeFSDefault("server.api");
    PyObject* pModule = PyImport_Import(pName);
    Py_DECREF(pName);

    if (pModule != nullptr) {
        PyObject* pFunc = PyObject_GetAttrString(pModule, "handle_message");
        if (pFunc && PyCallable_Check(pFunc)) {
            // Pass the string argument
            PyObject* pArgs = PyTuple_New(1);
            PyTuple_SetItem(pArgs, 0, PyUnicode_FromString(message.c_str()));
            
            PyObject* pValue = PyObject_CallObject(pFunc, pArgs);
            Py_DECREF(pArgs);
            if (pValue != nullptr) {
                resultStr = PyUnicode_AsUTF8(pValue);
                Py_DECREF(pValue);
            }
        } else {
            if (PyErr_Occurred()) PyErr_Print();
        }
        Py_XDECREF(pFunc);
        Py_DECREF(pModule);
    } else {
        if (PyErr_Occurred()) PyErr_Print();
    }
    return resultStr;
}

// Applies window properties dynamically to the window handle
void ApplyWindowProperties(webview::webview& w, HWND hwnd, const std::unordered_map<std::string, std::string>& config, bool& dragBackgroundOut) {
    std::string title = config.count("TITLE") ? config.at("TITLE") : "ESD Suite Framework";
    w.set_title(title);

    int windowWidth = 800;
    if (config.count("WINDOW_WIDTH")) {
        try { windowWidth = std::stoi(config.at("WINDOW_WIDTH")); } catch(...) {}
    }
    
    int windowHeight = 600;
    if (config.count("WINDOW_HEIGHT")) {
        try { windowHeight = std::stoi(config.at("WINDOW_HEIGHT")); } catch(...) {}
    }
    
    w.set_size(windowWidth, windowHeight, WEBVIEW_HINT_NONE);
    dragBackgroundOut = (config.count("DRAG_BACKGROUND") && config.at("DRAG_BACKGROUND") == "true");

#ifdef _WIN32
    if (!hwnd) return;

    bool canClose = (config.count("CAN_CLOSE") == 0 || config.at("CAN_CLOSE") != "false");
    bool canMinimize = (config.count("CAN_MINIMIZE") == 0 || config.at("CAN_MINIMIZE") != "false");
    bool canMaximize = (config.count("CAN_MAXIMIZE") == 0 || config.at("CAN_MAXIMIZE") != "false");
    bool showTitlebar = (config.count("TITLEBAR") == 0 || config.at("TITLEBAR") != "false");
    
    int cornerRadius = 0;
    if (config.count("CORNER_RADIUS")) {
        try { cornerRadius = std::stoi(config.at("CORNER_RADIUS")); } catch(...) {}
    }

    std::string iconPath = config.count("ICON") ? config.at("ICON") : "";
    if (!iconPath.empty()) {
        HICON hIcon = (HICON)LoadImageA(NULL, iconPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        if (hIcon) {
            SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        }
    }

    // Apply window styles based on properties
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    if (!canMinimize) style &= ~WS_MINIMIZEBOX; else style |= WS_MINIMIZEBOX;
    if (!canMaximize) style &= ~WS_MAXIMIZEBOX; else style |= WS_MAXIMIZEBOX;
    if (!showTitlebar) {
        style &= ~(WS_CAPTION | WS_THICKFRAME);
    } else {
        style |= (WS_CAPTION | WS_THICKFRAME);
    }
    SetWindowLongPtr(hwnd, GWL_STYLE, style);
    
    // Center the window on the screen and force it to redraw its frame
    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfo(hMonitor, &mi)) {
        int screenWidth = mi.rcWork.right - mi.rcWork.left;
        int screenHeight = mi.rcWork.bottom - mi.rcWork.top;
        int x = mi.rcWork.left + (screenWidth - windowWidth) / 2;
        int y = mi.rcWork.top + (screenHeight - windowHeight) / 2;
        SetWindowPos(hwnd, NULL, x, y, windowWidth, windowHeight, SWP_NOZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    if (!showTitlebar && cornerRadius > 0) {
        RECT rect;
        GetWindowRect(hwnd, &rect);
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        HRGN hRgn = CreateRoundRectRgn(0, 0, width, height, cornerRadius, cornerRadius);
        SetWindowRgn(hwnd, hRgn, TRUE);
    } else {
        SetWindowRgn(hwnd, NULL, TRUE); // Ensure region is reset if not rounded
    }

    HMENU hMenu = GetSystemMenu(hwnd, FALSE);
    if (!canClose) {
        EnableMenuItem(hMenu, SC_CLOSE, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
    } else {
        EnableMenuItem(hMenu, SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);
    }
#endif
}

int main() {
    std::string cwd = GetCurrentWorkingDir();
    auto rootConfig = LoadConfig(cwd + "/properties.config");
    
    // Check if terminal should be hidden
    bool hideTerminal = (rootConfig.count("SHOW_TERMINAL") && rootConfig["SHOW_TERMINAL"] == "false");
#ifdef _WIN32
    if (!hideTerminal) {
        // Allocate a new console window manually since we are running as a GUI application
        AllocConsole();
        FILE* fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        freopen_s(&fp, "CONIN$", "r", stdin);
    }
#endif

    // Determine the main page
    std::string mainPageFile = rootConfig.count("MAIN_PAGE") ? rootConfig["MAIN_PAGE"] : "ui/pages/index.html";
    std::string titlebarColor = rootConfig.count("TITLEBAR_COLOR") ? rootConfig["TITLEBAR_COLOR"] : "";

    // Replace backslashes with forward slashes for URI formatting
    std::string cwdUri = cwd;
    for(size_t i = 0; i < cwdUri.length(); ++i) {
        if(cwdUri[i] == '\\') cwdUri[i] = '/';
    }

    // 1. Initialize Python Interpreter
    Py_Initialize();
    PyRun_SimpleString("import sys, os\n"
                       "sys.path.append(os.getcwd())\n"
                       "print('Python backend initialized.')");

#ifdef _WIN32
    // Allows CORS fetching and module scripting directly out of the local file:// protocol
    _putenv("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--allow-file-access-from-files --remote-allow-origins=*");
#endif

    // 2. Initialize the Webview
    bool devTools = true;
    if (rootConfig.count("DEVTOOLS") && rootConfig.at("DEVTOOLS") == "false") {
        devTools = false;
    }

    webview::webview w(devTools, nullptr);
    
    bool contextMenu = true;
    if (rootConfig.count("DEFAULT_CONTEXTUAL_MENU") && rootConfig.at("DEFAULT_CONTEXTUAL_MENU") == "false") {
        contextMenu = false;
    }
    
    if (!contextMenu) {
        w.init("window.addEventListener('contextmenu', e => e.preventDefault());");
    }

    HWND hwnd = nullptr;
#ifdef _WIN32
    hwnd = (HWND)w.window();
    
    // Load embedded icon and set it for the window taskbar/titlebar
    HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(101));
    if (hIcon) {
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    }
#endif

    bool dragBackground = false;

    // Optional: Load Splash Screen Config
    std::string splashConfigPath = cwd + "/ui/splash/properties.config";
    std::ifstream splashFile(splashConfigPath);

    bool userWantsSplash = true;
    if (rootConfig.count("SPLASH_SCREEN") && rootConfig.at("SPLASH_SCREEN") == "false") {
        userWantsSplash = false;
    }

    bool useSplash = splashFile.good() && userWantsSplash;

    if (useSplash) {
        auto splashConfig = LoadConfig(splashConfigPath);
        ApplyWindowProperties(w, hwnd, splashConfig, dragBackground);
        w.navigate("file://" + cwdUri + "/ui/splash/splash.html");
    } else {
        ApplyWindowProperties(w, hwnd, rootConfig, dragBackground);
        w.navigate("file://" + cwdUri + "/" + mainPageFile);
    }

    // 3. Create the JS -> C++ -> Python Bridge
    w.bind("setNativeTitlebarColor", [&](std::string req) -> std::string {
#ifdef _WIN32
        if (hwnd) {
            int r = 255, g = 255, b = 255;
            if (sscanf(req.c_str(), "[\"rgb(%d, %d, %d)\"]", &r, &g, &b) == 3 ||
                sscanf(req.c_str(), "[\"rgba(%d, %d, %d", &r, &g, &b) == 3) {
                COLORREF color = RGB(r, g, b);
                DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &color, sizeof(color));
            }
        }
#endif
        return "";
    });

    if (titlebarColor == "transparent" && rootConfig.count("TITLEBAR") && rootConfig["TITLEBAR"] != "false") {
        w.init("window.addEventListener('DOMContentLoaded', () => {"
               "  setTimeout(() => {"
               "    let bg = window.getComputedStyle(document.body).backgroundColor;"
               "    if (window.setNativeTitlebarColor) window.setNativeTitlebarColor(bg);"
               "  }, 50);"
               "});");
    }

    w.bind("invokeBridge", [&](std::string req) -> std::string {
        // webview.h sends arguments as a JSON array e.g. ["{...}"]
        // We strip the outer array brackets to parse our raw JSON object string
        std::string payload = "";
        if (req.length() > 2 && req.front() == '[' && req.back() == ']') {
            // Remove the bounding [ ] array markers
            payload = req.substr(1, req.length() - 2);
            // Additional minor unescaping may be necessary depending on JSON payload
        } else {
            payload = req;
        }

        std::string pyRes = CallPythonBackend(payload);
        return pyRes; 
    });

    // Binding for splash screen to trigger transition
    w.bind("transitionToMain", [&](std::string req) -> std::string {
        ApplyWindowProperties(w, hwnd, rootConfig, dragBackground);
        w.navigate("file://" + cwdUri + "/" + mainPageFile);
        return "";
    });

    // Native Drag window binding for UI
    w.bind("dragWindow", [&](std::string req) -> std::string {
        if (dragBackground) {
#ifdef _WIN32
            if (hwnd) {
                ReleaseCapture();
                SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
#endif
        }
        return "";
    });

    // Native External Link opener
    w.bind("openExternalLink", [&](std::string req) -> std::string {
        std::string url = req;
        // The payload arrives as a JSON array string: ["https://example.com"]
        if (url.length() >= 4 && url.front() == '[' && url.back() == ']') {
            url = url.substr(2, url.length() - 4);
        }
#ifdef _WIN32
        ShellExecuteA(0, "open", url.c_str(), 0, 0, SW_SHOW);
#endif
        return "";
    });

    // Inject Javascript to intercept external links
    w.init(R"(
        window.addEventListener('click', function(e) {
            let target = e.target.closest('a');
            if (target && target.href) {
                // Check if it's an external URL (http or https)
                if (target.href.startsWith('http://') || target.href.startsWith('https://')) {
                    e.preventDefault();
                    if (window.openExternalLink) {
                        window.openExternalLink(target.href);
                    }
                }
            }
        });
    )");

    if (!hideTerminal) {
        std::cout << "Loading Interface..." << std::endl;
    }

    // 4. Run loop
    w.run();

    // 5. Cleanup
    Py_Finalize();
    return 0;
}
