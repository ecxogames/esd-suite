# Ecxo Softwares Development Suite (ESD Suite)

The **ESD Suite** is a lightweight, cross-platform desktop application framework that bridges C++, Python, and JavaScript into a single runtime. Build native desktop apps with a web-based UI and a Python backend — without the overhead of Electron or Tauri.

## Architecture

| Layer | Technology | Role |
|-------|-----------|------|
| **Engine** | C++ / WebView2 (Win) / WebKit (macOS) / GTK (Linux) | Native window, Python runtime host, bridge layer |
| **Frontend** | HTML / CSS / JavaScript | User interface rendered inside the webview |
| **Backend** | Python | Application logic, scripting, data processing |

Communication between layers is handled by the **Python Bridge** — a bidirectional calling system managed by the C++ engine.

## Quick Start

### Requirements
- CMake 3.14+
- C++ compiler (MSVC on Windows, Clang on macOS, GCC on Linux)
- Python 3 with development headers

### Setup & Run

```bash
# 1. Install dependencies (downloads webview.h, WebView2 SDK, configures CMake)
python scripts/setup.py

# 2. Build the application
python scripts/build.py

# 3. Start development mode (auto-rebuild on file changes)
python scripts/dev.py
```

## Project Structure

```
├── engine/          C++ core (main.cpp, webview.h)
├── ui/              Frontend (HTML/CSS/JS pages, splash screen)
├── server/          Backend router (api.py — legacy action-based bridge)
├── public/          Python modules accessible from the frontend
├── private/         Python modules restricted to backend use
├── scripts/         Dev tools (build, dev watcher, setup, packaging, updater)
├── documentation/   Developer guides
└── properties.config   Application settings (title, window size, features)
```

## Using the Python Bridge

### JavaScript → Python

Import any Python module from `public/` or `private/` and call its functions directly:

```javascript
// Import and call a public module
const utils = Python.Import.Public('utils.py');
const greeting = await utils.generate_greeting({ name: 'Alice' });

// Import and call a private module
const secret = Python.Import.Private('secret_processor.py');
const hash = await secret.process_secure_data({ data: 'sensitive' });
```

Arguments are passed as keyword arguments. Your Python functions should accept named parameters:

```python
# public/my_module.py
def my_function(name='', count=0):
    return f"Hello {name}, count is {count}"
```

The legacy bridge (`invokeBridge`) is still available for routing through `server/api.py`:

```javascript
const res = await window.invokeBridge(JSON.stringify({ action: "ping", data: "Hello" }));
```

### Python → JavaScript

Use the built-in `bridge` module from any Python file:

```python
import bridge

# Fire-and-forget — works from any context
bridge.eval_js("document.title = 'Updated from Python'")

# With return value — background threads only
import threading
def bg():
    title = bridge.call_js("document.title")
    print(f"Title: {title}")
threading.Thread(target=bg).start()
```

For full details, see [documentation/python-bridge.md](documentation/python-bridge.md).

## Branch Workflow

- **`main`** — Active development. All new features and fixes land here first.
- **`pre-release`** — Final testing and QA before release.
- **`release`** — Stable, production-ready code.
- **`website`** — Source for the official download site.