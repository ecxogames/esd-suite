#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <map>
#include <atomic>

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

// ============================================================================
// Python Bridge Module — exposes a "bridge" C extension to Python
//
// This module is registered via PyImport_AppendInittab() before Py_Initialize()
// so that any Python code can do `import bridge` to access these functions.
//
// Two functions are exposed:
//   bridge.eval_js(code)  — fire-and-forget JS execution (safe from any thread)
//   bridge.call_js(expr)  — synchronous JS eval with return value (background threads only)
//
// The call_js function uses a request/response pattern:
//   1. Python generates a unique request ID and dispatches JS to the UI thread
//   2. The JS evaluates the expression and calls __bridgeReturn with the result
//   3. __bridgeReturn stores the result and signals a condition variable
//   4. Python (waiting on the CV with GIL released) picks up the result and returns
// ============================================================================

// Global pointer to the webview instance, set in main() after webview creation.
// Read by bridge functions from Python threads — assigned once before any Python call is possible.
static webview::webview* g_webview = nullptr;

// Synchronization state for call_js: a mutex-protected map of pending request IDs to results,
// with a condition variable to wake the waiting Python thread when a result arrives.
static std::mutex g_js_mutex;
static std::condition_variable g_js_cv;
static std::map<int, std::string> g_js_results;
static std::atomic<int> g_js_request_id{0};

// bridge.eval_js(code) — Dispatches JS code to the WebView's UI thread.
// Returns immediately with {"status": "ok"}. The JS executes asynchronously.
// Safe to call from any context, including inside handle_message callbacks.
static PyObject* py_eval_js(PyObject* self, PyObject* args) {
    const char* js_code;
    if (!PyArg_ParseTuple(args, "s", &js_code)) return NULL;
    if (!g_webview) {
        PyErr_SetString(PyExc_RuntimeError, "WebView not initialized");
        return NULL;
    }
    std::string code(js_code);
    g_webview->dispatch([code]() { g_webview->eval(code); });
    return PyUnicode_FromString("{\"status\": \"ok\"}");
}

// bridge.call_js(expression) — Evaluates a JS expression and returns its result as a string.
// IMPORTANT: This function blocks the calling thread for up to 5 seconds. It releases the
// Python GIL before waiting so other Python threads can run. It CANNOT be called from the
// main UI thread (i.e. from inside a webview binding callback like handle_message), because
// the dispatched JS needs the UI thread's event loop to execute — calling from the UI thread
// would deadlock. Use bridge.eval_js() for fire-and-forget from the main thread instead.
static PyObject* py_call_js(PyObject* self, PyObject* args) {
    const char* expression;
    if (!PyArg_ParseTuple(args, "s", &expression)) return NULL;
    if (!g_webview) {
        PyErr_SetString(PyExc_RuntimeError, "WebView not initialized");
        return NULL;
    }

    int id = g_js_request_id++;  // Unique ID to match this request with its __bridgeReturn callback
    std::string expr(expression);
    // Wrap the expression in a try/catch IIFE that calls __bridgeReturn with the result or error
    std::string js = "(function(){ try { var __r = " + expr +
        "; window.__bridgeReturn(JSON.stringify({id:" + std::to_string(id) +
        ",result:typeof __r === 'string' ? __r : JSON.stringify(__r)})); } catch(e) { " +
        "window.__bridgeReturn(JSON.stringify({id:" + std::to_string(id) +
        ",error:e.message})); } })()";

    g_webview->dispatch([js]() { g_webview->eval(js); });

    // Release GIL before blocking wait to avoid deadlocks
    std::string result;
    bool ok = false;
    Py_BEGIN_ALLOW_THREADS
    std::unique_lock<std::mutex> lock(g_js_mutex);
    ok = g_js_cv.wait_for(lock, std::chrono::seconds(5),
        [id]() { return g_js_results.count(id) > 0; });
    if (ok) {
        result = g_js_results[id];
        g_js_results.erase(id);
    } else {
        g_js_results.erase(id); // Cleanup stale entry on timeout
    }
    Py_END_ALLOW_THREADS

    if (ok) {
        return PyUnicode_FromString(result.c_str());
    }
    PyErr_SetString(PyExc_TimeoutError, "call_js timed out after 5 seconds");
    return NULL;
}

static PyMethodDef BridgeMethods[] = {
    {"eval_js", py_eval_js, METH_VARARGS, "Execute JS in WebView (fire-and-forget)"},
    {"call_js", py_call_js, METH_VARARGS, "Execute JS expression and return result (background threads only)"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef bridgemodule = {
    PyModuleDef_HEAD_INIT, "bridge", NULL, -1, BridgeMethods
};

PyMODINIT_FUNC PyInit_bridge(void) {
    return PyModule_Create(&bridgemodule);
}

// ============================================================================

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
                const char* tmp = PyUnicode_AsUTF8(pValue);
                if (tmp) resultStr = std::string(tmp);
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

// Minimal JSON string value extractor — finds "key":"value" and returns value.
// Only handles simple string values (no numbers, booleans, or nested objects).
// This is sufficient because the JSON is generated by our own JS code with a known format.
std::string ExtractJsonValue(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.length();
    size_t end = json.find("\"", pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// Escape a string for safe JSON embedding (handles " and \)
std::string EscapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

// Dynamically imports a Python module from the public/ or private/ directory and calls
// a specific function with keyword arguments parsed from a JSON string.
//
// Flow: JS sends {folder, module, function, args} -> C++ imports <folder>.<module> ->
//       calls <function>(**json.loads(args)) -> returns the result as a string.
//
// If the function returns a dict or list, it's serialized to JSON automatically.
// If it returns anything else, str() is called on it.
// Errors are returned as {"error": "message"} JSON strings.
std::string CallPythonModule(const std::string& folder, const std::string& module_name,
                              const std::string& func_name, const std::string& args_json) {
    std::string resultStr = "{\"error\": \"Failed to call Python module\"}";

    // Security: validate folder is strictly "public" or "private"
    if (folder != "public" && folder != "private") {
        return "{\"error\": \"Invalid folder: must be 'public' or 'private'\"}";
    }
    // Security: module_name must not contain dots or slashes (no path traversal)
    if (module_name.find('.') != std::string::npos || module_name.find('/') != std::string::npos
        || module_name.find('\\') != std::string::npos || module_name.empty()) {
        return "{\"error\": \"Invalid module name\"}";
    }

    std::string full_module = folder + "." + module_name;
    PyObject* pName = PyUnicode_DecodeFSDefault(full_module.c_str());
    PyObject* pModule = PyImport_Import(pName);
    Py_DECREF(pName);

    if (pModule != nullptr) {
        PyObject* pFunc = PyObject_GetAttrString(pModule, func_name.c_str());
        if (pFunc && PyCallable_Check(pFunc)) {
            // Parse args_json into Python kwargs dict via json.loads
            PyObject* json_module = PyImport_ImportModule("json");
            if (!json_module) {
                if (PyErr_Occurred()) PyErr_Print();
                Py_XDECREF(pFunc);
                Py_DECREF(pModule);
                return "{\"error\": \"Failed to import json module\"}";
            }
            PyObject* loads_func = PyObject_GetAttrString(json_module, "loads");
            if (!loads_func) {
                if (PyErr_Occurred()) PyErr_Print();
                Py_DECREF(json_module);
                Py_XDECREF(pFunc);
                Py_DECREF(pModule);
                return "{\"error\": \"Failed to get json.loads\"}";
            }
            PyObject* json_str = PyUnicode_FromString(args_json.c_str());
            PyObject* json_args = PyTuple_Pack(1, json_str);
            PyObject* kwargs = PyObject_CallObject(loads_func, json_args);
            Py_DECREF(json_str);
            Py_DECREF(json_args);
            Py_DECREF(loads_func);
            Py_DECREF(json_module);

            PyObject* pValue = nullptr;
            if (kwargs && PyDict_Check(kwargs)) {
                PyObject* empty_args = PyTuple_New(0);
                pValue = PyObject_Call(pFunc, empty_args, kwargs);
                Py_DECREF(empty_args);
                Py_DECREF(kwargs);
            } else {
                if (kwargs) Py_DECREF(kwargs);
                if (PyErr_Occurred()) PyErr_Clear();
                PyObject* pArgs = PyTuple_New(1);
                PyTuple_SetItem(pArgs, 0, PyUnicode_FromString(args_json.c_str()));
                pValue = PyObject_CallObject(pFunc, pArgs);
                Py_DECREF(pArgs);
            }

            if (pValue != nullptr) {
                if (PyDict_Check(pValue) || PyList_Check(pValue)) {
                    PyObject* json_mod = PyImport_ImportModule("json");
                    if (json_mod) {
                        PyObject* dumps = PyObject_GetAttrString(json_mod, "dumps");
                        if (dumps) {
                            PyObject* dump_args = PyTuple_Pack(1, pValue);
                            if (dump_args) {
                                PyObject* json_result = PyObject_CallObject(dumps, dump_args);
                                if (json_result) {
                                    const char* tmp = PyUnicode_AsUTF8(json_result);
                                    if (tmp) resultStr = std::string(tmp);
                                    Py_DECREF(json_result);
                                } else {
                                    if (PyErr_Occurred()) PyErr_Print();
                                }
                                Py_DECREF(dump_args);
                            }
                            Py_DECREF(dumps);
                        }
                        Py_DECREF(json_mod);
                    }
                } else {
                    PyObject* str_val = PyObject_Str(pValue);
                    if (str_val) {
                        const char* tmp = PyUnicode_AsUTF8(str_val);
                        if (tmp) resultStr = std::string(tmp);
                        Py_DECREF(str_val);
                    } else {
                        if (PyErr_Occurred()) PyErr_Clear();
                    }
                }
                Py_DECREF(pValue);
            } else {
                if (PyErr_Occurred()) {
                    PyObject *ptype, *pvalue, *ptraceback;
                    PyErr_Fetch(&ptype, &pvalue, &ptraceback);
                    if (pvalue) {
                        PyObject* str_val = PyObject_Str(pvalue);
                        if (str_val) {
                            const char* tmp = PyUnicode_AsUTF8(str_val);
                            if (tmp) resultStr = "{\"error\": \"" + EscapeJsonString(std::string(tmp)) + "\"}";
                            Py_DECREF(str_val);
                        }
                    }
                    Py_XDECREF(ptype);
                    Py_XDECREF(pvalue);
                    Py_XDECREF(ptraceback);
                }
            }
        } else {
            if (PyErr_Occurred()) PyErr_Print();
            resultStr = "{\"error\": \"Function '" + EscapeJsonString(func_name) + "' not found in " + EscapeJsonString(full_module) + "\"}";
        }
        Py_XDECREF(pFunc);
        Py_DECREF(pModule);
    } else {
        if (PyErr_Occurred()) PyErr_Print();
        resultStr = "{\"error\": \"Module '" + full_module + "' not found\"}";
    }
    return resultStr;
}

// Applies window properties dynamically to the window handle
#ifdef _WIN32
void ApplyWindowProperties(webview::webview& w, HWND hwnd, const std::unordered_map<std::string, std::string>& config, bool& dragBackgroundOut) {
#else
void ApplyWindowProperties(webview::webview& w, void* hwnd, const std::unordered_map<std::string, std::string>& config, bool& dragBackgroundOut) {
#endif
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
    // Register the built-in "bridge" module BEFORE Py_Initialize() so Python code
    // can do `import bridge` to access eval_js() and call_js().
    PyImport_AppendInittab("bridge", PyInit_bridge);
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
    g_webview = &w;  // Store global pointer so Python's bridge module can dispatch JS

    bool contextMenu = true;
    if (rootConfig.count("DEFAULT_CONTEXTUAL_MENU") && rootConfig.at("DEFAULT_CONTEXTUAL_MENU") == "false") {
        contextMenu = false;
    }
    
    if (!contextMenu) {
        w.init("window.addEventListener('contextmenu', e => e.preventDefault());");
    }

#ifdef _WIN32
    HWND hwnd = nullptr;
    hwnd = (HWND)w.window();

    // Load embedded icon and set it for the window taskbar/titlebar
    HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(101));
    if (hIcon) {
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    }
#else
    void* hwnd = nullptr;
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

    // ========================================================================
    // Python Bridge — dynamic module import (public/private) + Python→JS
    // ========================================================================

    // Binding: callPythonModule — the low-level C++ binding that Python.Import uses internally.
    // Receives a JSON string with {folder, module, function, args} from the JS Proxy API,
    // unescapes it (webview.h double-wraps in a JSON array), extracts the fields, and
    // delegates to CallPythonModule() which handles the actual Python import and function call.
    w.bind("callPythonModule", [&](std::string req) -> std::string {
        // webview.h wraps all binding arguments in a JSON array: ["escaped_string"]
        std::string payload = req;
        if (payload.length() > 2 && payload.front() == '[' && payload.back() == ']') {
            payload = payload.substr(1, payload.length() - 2);
        }
        // The inner string is JSON-escaped (quotes become \"), so we need to unescape it
        if (payload.length() >= 2 && payload.front() == '"' && payload.back() == '"') {
            payload = payload.substr(1, payload.length() - 2);
            std::string unescaped;
            for (size_t i = 0; i < payload.size(); i++) {
                if (payload[i] == '\\' && i + 1 < payload.size()) {
                    if (payload[i+1] == '"') { unescaped += '"'; i++; }
                    else if (payload[i+1] == '\\') { unescaped += '\\'; i++; }
                    else unescaped += payload[i];
                } else unescaped += payload[i];
            }
            payload = unescaped;
        }

        std::string folder = ExtractJsonValue(payload, "folder");
        std::string module = ExtractJsonValue(payload, "module");
        std::string function = ExtractJsonValue(payload, "function");

        // Extract args (nested JSON — need depth-aware parsing)
        std::string args = "{}";
        size_t argsPos = payload.find("\"args\":");
        if (argsPos != std::string::npos) {
            size_t start = argsPos + 7;
            if (start < payload.size()) {
                if (payload[start] == '"') {
                    // Quoted string — extract and unescape
                    size_t end = start + 1;
                    while (end < payload.size() && !(payload[end] == '"' && payload[end-1] != '\\')) end++;
                    args = payload.substr(start + 1, end - start - 1);
                    std::string ua;
                    for (size_t i = 0; i < args.size(); i++) {
                        if (args[i] == '\\' && i + 1 < args.size()) {
                            if (args[i+1] == '"') { ua += '"'; i++; }
                            else if (args[i+1] == '\\') { ua += '\\'; i++; }
                            else ua += args[i];
                        } else ua += args[i];
                    }
                    args = ua;
                } else if (payload[start] == '{') {
                    // Raw JSON object — match braces
                    int depth = 0;
                    size_t end = start;
                    for (; end < payload.size(); end++) {
                        if (payload[end] == '{') depth++;
                        else if (payload[end] == '}') { depth--; if (depth == 0) { end++; break; } }
                    }
                    args = payload.substr(start, end - start);
                }
            }
        }

        return CallPythonModule(folder, module, function, args);
    });

    // Binding: __bridgeReturn — internal callback used by bridge.call_js().
    // When Python calls call_js("expr"), the C++ layer dispatches JS that evaluates the
    // expression and calls __bridgeReturn({id, result}) with the result. This binding
    // receives that callback, extracts the request ID and result, stores it in g_js_results,
    // and signals the condition variable so the waiting Python thread can pick it up.
    w.bind("__bridgeReturn", [&](std::string req) -> std::string {
        std::string payload = req;
        if (payload.length() > 2 && payload.front() == '[' && payload.back() == ']') {
            payload = payload.substr(1, payload.length() - 2);
        }
        // Unescape JSON string
        if (payload.length() >= 2 && payload.front() == '"' && payload.back() == '"') {
            payload = payload.substr(1, payload.length() - 2);
            std::string unescaped;
            for (size_t i = 0; i < payload.size(); i++) {
                if (payload[i] == '\\' && i + 1 < payload.size()) {
                    if (payload[i+1] == '"') { unescaped += '"'; i++; }
                    else if (payload[i+1] == '\\') { unescaped += '\\'; i++; }
                    else unescaped += payload[i];
                } else unescaped += payload[i];
            }
            payload = unescaped;
        }

        int id = -1;
        std::string result;

        size_t idPos = payload.find("\"id\":");
        if (idPos != std::string::npos) {
            try { id = std::stoi(payload.substr(idPos + 5)); } catch(...) {}
        }

        size_t resPos = payload.find("\"result\":");
        if (resPos != std::string::npos) {
            size_t start = resPos + 9;
            if (start < payload.size() && payload[start] == '"') {
                size_t end = payload.find('"', start + 1);
                if (end != std::string::npos) result = payload.substr(start + 1, end - start - 1);
            } else if (start < payload.size()) {
                size_t end = payload.find_first_of(",}", start);
                if (end != std::string::npos) result = payload.substr(start, end - start);
            }
        }

        size_t errPos = payload.find("\"error\":");
        if (errPos != std::string::npos) {
            size_t start = errPos + 8;
            if (start < payload.size() && payload[start] == '"') {
                size_t end = payload.find('"', start + 1);
                if (end != std::string::npos) result = "ERROR:" + payload.substr(start + 1, end - start - 1);
            }
        }

        if (id >= 0) {
            std::lock_guard<std::mutex> lock(g_js_mutex);
            g_js_results[id] = result;
            g_js_cv.notify_all();
        }
        return "";
    });

    // Inject the Python.Import API into every page loaded by the webview.
    // This creates window.Python.Import.Public() and window.Python.Import.Private() which
    // return ES6 Proxy objects. When you access any property on a Proxy (e.g. .generate_greeting),
    // it returns a function that sends a callPythonModule request to C++. The function returns
    // a Promise that resolves with the Python return value.
    //
    // Usage:  const utils = Python.Import.Public('utils.py');
    //         const result = await utils.generate_greeting({ name: 'Alice' });
    w.init(R"(
        window.Python = {
            Import: {
                Public: function(filename) {
                    var moduleName = filename.replace(/\.py$/, '');
                    return new Proxy({}, {
                        get: function(target, funcName) {
                            if (funcName === 'then' || funcName === 'catch' || funcName === 'toJSON' || funcName === 'valueOf' || funcName === 'toString' || typeof funcName === 'symbol') return undefined;
                            return function(args) {
                                var argsObj = (typeof args === 'object' && args !== null) ? args : {};
                                return window.callPythonModule(JSON.stringify({
                                    folder: 'public',
                                    module: moduleName,
                                    function: funcName,
                                    args: JSON.stringify(argsObj)
                                })).then(function(res) {
                                    try { return JSON.parse(res); } catch(e) { return res; }
                                });
                            };
                        }
                    });
                },
                Private: function(filename) {
                    var moduleName = filename.replace(/\.py$/, '');
                    return new Proxy({}, {
                        get: function(target, funcName) {
                            if (funcName === 'then' || funcName === 'catch' || funcName === 'toJSON' || funcName === 'valueOf' || funcName === 'toString' || typeof funcName === 'symbol') return undefined;
                            return function(args) {
                                var argsObj = (typeof args === 'object' && args !== null) ? args : {};
                                return window.callPythonModule(JSON.stringify({
                                    folder: 'private',
                                    module: moduleName,
                                    function: funcName,
                                    args: JSON.stringify(argsObj)
                                })).then(function(res) {
                                    try { return JSON.parse(res); } catch(e) { return res; }
                                });
                            };
                        }
                    });
                }
            }
        };
    )");

    // ========================================================================

    if (!hideTerminal) {
        std::cout << "Loading Interface..." << std::endl;
    }

    // 4. Run loop
    w.run();

    // 5. Cleanup
    Py_Finalize();
    return 0;
}
