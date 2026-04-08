# Python Bridge

The Python Bridge enables bidirectional communication between JavaScript (frontend) and Python (backend), managed by the C++ engine layer. It lets you call Python functions directly from your UI code and push data back to the frontend from Python.

## How It Works

The bridge operates through three paths:

1. **Legacy Bridge** (`invokeBridge`): Routes JSON messages to `server/api.py`. This is the original, action-based dispatcher.
2. **Dynamic Import** (`Python.Import`): Calls any function in `public/` or `private/` by name, with keyword arguments.
3. **Python-to-JS** (`bridge` module): Lets Python code execute JavaScript in the WebView.

## 1. Calling Python from JavaScript

### Using `Python.Import`

Import a Python module from the `public/` or `private/` folder, then call its functions directly:

```javascript
// Import a public module
const utils = Python.Import.Public('utils.py');
const greeting = await utils.generate_greeting({ name: 'Alice' });

// Import a private module
const secret = Python.Import.Private('secret_processor.py');
const hash = await secret.process_secure_data({ data: 'sensitive' });
```

**How it works under the hood:**
- `Python.Import.Public('utils.py')` returns a [Proxy](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Proxy) object
- Accessing any property on it (e.g. `.generate_greeting`) returns a function
- That function sends a JSON message to C++ via the `callPythonModule` binding
- C++ imports the Python module dynamically and calls the function with `**kwargs`
- The return value comes back as a resolved Promise

**Arguments are passed as keyword arguments.** Your Python function should accept named parameters:

```python
# public/utils.py
def generate_greeting(name=''):
    return f"Hello, {name}!"
```

**Return values:**
- Strings are returned as-is
- Dicts and lists are automatically serialized to JSON

### Using the Legacy Bridge

The original `invokeBridge` still works for routing through `server/api.py`:

```javascript
const response = await window.invokeBridge(JSON.stringify({
    action: "ping",
    data: "Hello"
}));
```

## 2. Calling JavaScript from Python

The `bridge` module is a built-in C extension available to all Python code in the project.

### `bridge.eval_js(code)`

Executes JavaScript in the WebView. Fire-and-forget — does not return a value.

```python
import bridge

# Update the DOM
bridge.eval_js("document.title = 'Updated from Python'")

# Call a JavaScript function
bridge.eval_js("myFunction('arg1', 'arg2')")
```

This works from **any context**, including inside `handle_message` callbacks.

### `bridge.call_js(expression)`

Evaluates a JavaScript expression and returns the result. **Background threads only.**

```python
import bridge
import threading

def background_task():
    title = bridge.call_js("document.title")
    print(f"Page title: {title}")

threading.Thread(target=background_task).start()
```

**Important:** `call_js` blocks the calling thread for up to 5 seconds waiting for the JS result. It **cannot** be used from code that was called by JavaScript (e.g. inside `handle_message`), because that would deadlock the single UI thread. Use `eval_js` instead in those cases.

## 3. Public vs Private Folders

| Folder | Access | Use Case |
|--------|--------|----------|
| `public/` | `Python.Import.Public()` | Generic helpers, formatters, public API calls |
| `private/` | `Python.Import.Private()` | Database access, encryption, sensitive operations |

Both folders require an `__init__.py` file (can be empty) to be recognized as Python packages.

**Security:** The C++ layer validates that only `"public"` or `"private"` can be used as folder names, and module names cannot contain dots or slashes. This prevents path traversal attacks.

## 4. Cross-Call Patterns

| Pattern | Description |
|---------|-------------|
| **JS → Public Python** | `Python.Import.Public('module.py').func({...})` |
| **JS → Private Python** | `Python.Import.Private('module.py').func({...})` |
| **Python → JS (fire-and-forget)** | `bridge.eval_js("code")` |
| **Python → JS (with return)** | `bridge.call_js("expression")` (background thread) |
| **Python → Python (public)** | Standard `from public import module` |
| **Python → Python (private)** | Standard `from private import module` |

## 5. Error Handling

If a Python function raises an exception, the error is returned as a JSON object:

```json
{"error": "TypeError: generate_greeting() got an unexpected keyword argument 'invalid'"}
```

If a module or function is not found:

```json
{"error": "Module 'public.nonexistent' not found"}
{"error": "Function 'missing_func' not found in public.utils"}
```

On the JavaScript side, these come back as resolved Promises (not rejected), so check the response:

```javascript
const result = await utils.some_function({ arg: 'value' });
if (result && result.error) {
    console.error('Python error:', result.error);
}
```
