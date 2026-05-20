# Ecxo Softwares Development Suite (ESD Suite) - Main Branch

Welcome to the **main** branch of the ESD Suite repository. This branch contains the active, in-development code and serves as the default branch for ongoing development. All new features, bug fixes, and improvements are committed here before making their way to release.

## About the Project

The **Ecxo Softwares Development Suite (ESD Suite)** is a hybrid desktop application framework and development environment. Its architecture consists of:

- **C++ & WebView2 Core Engine:** Built in C++ using CMake, the core leverages Microsoft's WebView2 to create a high-performance desktop window that renders a web-based user interface, similar to frameworks like Electron or Tauri.
- **Web-Based User Interface:** The front-end utilizes standard web technologies (HTML/CSS/JS) to serve as the graphical interface loaded by the WebView2 engine.
- **Python Backend & Scripting:** The suite uses Python extensively for its internal logic, local server APIs to handle communication between the UI and backend, and automated development/build scripts.