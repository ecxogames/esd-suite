# Building and Distributing

The ESD Suite Framework includes a powerful distribution build manager (`build.py`) that helps you package your application for end-users. Because the framework relies on C++ and an embedded Python runtime, deploying the app correctly is critical to ensure it works on computers that don't have Python or CMake installed.

## How to use the Build Manager

To start the build manager, open your terminal in the `main` directory and run:

```bash
python build.py
```

An interactive menu will appear with three distinct build options:

### 1. Installer (Recommended for Production)
This option generates a self-contained environment and seamlessly compiles it into an automated Windows Installer `.exe`.

* **What it does:** It creates a Standalone build (see below), generates an `installer.iss` script in the `dist/` folder, and automatically compiles it into `ESDSuite_Installer.exe` if Inno Setup is installed on your computer.
* **Requirements:** 
  To use the automatic installer generator, you must have [Inno Setup](https://jrsoftware.org/isdl.php) installed. It's a free, industry-standard Windows installer creator.
* **If Inno Setup is missing:** 
  The script will still build the `.iss` file successfully, but it will ask you to download Inno Setup, right-click the `installer.iss` file, and select **Compile** manually to get your final `_Installer.exe`.

### 2. Sandbox / Standalone (Portable App)
This builds a completely self-contained, portable folder containing your application. 

* **What it does:** 
  1. Compiles your C++ engine in `Release` mode.
  2. Copies your `ui/`, `server/`, and `.config` files into a new `dist/Standalone` directory.
  3. Reaches out to `python.org` to download the official "Windows Embeddable Package" matching your exact Python version.
  4. Extracts the Python runtime directly alongside your executable.
* **Why use this:** The resulting `dist/Standalone` folder requires **no installation**. You can zip this folder, send it to a friend, and they can run `ESDEngine.exe` instantly, even if they don't have Python installed on their computer.

### 3. Regular (Local Dev Testing Only)
This is the standard build process used during development.

* **What it does:** It simply runs the CMake commands to build your executable.
* **Warning:** The resulting `.exe` points to your *local system's* Python installation. If you send this `.exe` to another user, they will likely get "missing DLL" errors or crash on startup, because their PC won't have the exact same Python path/version as yours. 

---

## What needs to be distributed?
If you are distributing your application manually (using the Sandbox option), ensure the following directory structure is maintained in your ZIP file:

```text
📁 YourApp/
 ├── ESDEngine.exe         (Core Engine)
 ├── properties.config     (Core Settings)
 ├── python3.dll           (And other python embedded files...)
 ├── python311.zip         (Standard library)
 ├── 📁 ui/                (Your Frontend)
 └── 📁 server/            (Your Backend)
```

**Note for Developers:** Python packages installed via `pip` locally on your machine are NOT automatically copied into the Standalone build. If your `server/api.py` relies on third-party libraries (like `requests`, `numpy`, etc.), you will need to install them into the embeddable python environment or bundle them in a `site-packages` directory next to the engine before distribution.