#include <iostream>
#include <string>
#include <cstdlib>
#include <Python.h>
#include "webview.h"

#ifdef _WIN32
#include <direct.h>
#define GetCurrentDir _getcwd
#else
#include <unistd.h>
#define GetCurrentDir getcwd
#endif

// Helper to get current directory
std::string GetCurrentWorkingDir() {
    char buff[FILENAME_MAX];
    GetCurrentDir(buff, FILENAME_MAX);
    std::string current_working_dir(buff);
    return current_working_dir;
}

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

int main() {
    // 1. Initialize Python Interpreter
    // Allow Python to look in the current executing directory
    Py_Initialize();
    PyRun_SimpleString("import sys, os\n"
                       "sys.path.append(os.getcwd())\n"
                       "print('Python backend initialized.')");

    // 2. Initialize the Webview
    webview::webview w(true, nullptr);
    w.set_title("ESD Suite Framework");
    w.set_size(800, 600, WEBVIEW_HINT_NONE);

    // 3. Create the JS -> C++ -> Python Bridge
    w.bind("invokeBridge", [&](std::string req) -> std::string {
        // webview.h sends arguments as a JSON array e.g. ["{...}"]
        // We strip the outer array brackets to parse our raw JSON object string
        std::string payload = "";
        if (req.length() > 2 && req.front() == '[' && req.back() == ']') {
            // Remove the bounding [ ] array markers
            payload = req.substr(1, req.length() - 2);
            // Additional minor unescaping may be necessary depending on JSON payload,
            // but for simple structures, passing directly works well.
        } else {
            payload = req;
        }

        std::string pyRes = CallPythonBackend(payload);
        return pyRes; 
    });

    // 4. Load the UI HTML
    std::string cwd = GetCurrentWorkingDir();
    
    // Replace backslashes with forward slashes for URI formatting (specifically Windows)
    for(size_t i = 0; i < cwd.length(); ++i) {
        if(cwd[i] == '\\') cwd[i] = '/';
    }

    std::string htmlUrl = "file://" + cwd + "/ui/pages/index.html";
    std::cout << "Loading UI at: " << htmlUrl << std::endl;
    w.navigate(htmlUrl);

    // 5. Run loop
    w.run();

    // 6. Cleanup
    Py_Finalize();
    return 0;
}
