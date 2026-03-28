import os
import sys
import shutil
import urllib.request
import zipfile
import subprocess

def print_header(title):
    print("\n" + "="*50)
    print(f"  {title}")
    print("="*50)

def get_or_convert_icon(props):
    icon_path = props.get("ICON", "")
    if not icon_path or not os.path.exists(icon_path):
        return None

    if icon_path.lower().endswith(".png"):
        try:
            from PIL import Image
            ico_path = os.path.join("engine", "converted_icon.ico")
            img = Image.open(icon_path)
            img.save(ico_path, format="ICO", sizes=[(256, 256), (128, 128), (64, 64), (32, 32), (16, 16)])
            return ico_path
        except ImportError:
            print("[Warning] 'Pillow' is required to convert PNG icons to ICO. Run 'pip install Pillow'. Ignoring icon.")
            return None
        except Exception as e:
            print(f"[Warning] Failed to convert PNG to ICO: {e}")
            return None
    return icon_path

def prepare_icon():
    props = parse_properties_config()
    icon_path = get_or_convert_icon(props)
    rc_path = os.path.join("engine", "icon.rc")
    
    if icon_path and os.path.exists(icon_path):
        with open(rc_path, "w") as f:
            # RC compiler needs normalized/escaped paths 
            abs_icon = os.path.abspath(icon_path).replace("\\", "/")
            f.write(f'101 ICON "{abs_icon}"\n')
    else:
        if os.path.exists(rc_path):
            try:
                os.remove(rc_path)
            except:
                pass

def run_cmake_build():
    print_header("Compiling C++ Executable")
    prepare_icon()
    
    # Force CMake to re-evaluate whether icon.rc exists by removing the cache.
    cache_file = os.path.join("build", "CMakeCache.txt")
    if os.path.exists(cache_file):
        try:
            os.remove(cache_file)
        except OSError:
            pass

    subprocess.run(["cmake", "-B", "build"], check=True)
    subprocess.run(["cmake", "--build", "build", "--config", "Release"], check=True)

def find_exe():
    bases = [
        os.path.join('build', 'Release', 'ESDEngine.exe'),
        os.path.join('build', 'ESDEngine.exe'),
    ]
    for b in bases:
        if os.path.exists(b):
            return b
    return None

def build_regular():
    print_header("REGULAR BUILD (Local Testing Only)")
    print("[WARNING] The Regular build creates an executable that points directly to your local system's Python installation.")
    print("[WARNING] It will NOT work on other computers unless they have the exact same Python version installed in the same path.")
    
    run_cmake_build()
    exe = find_exe()
    if not exe:
        print("[Error] Executable not found after build.")
        return

    dist_dir = os.path.join("dist", "Regular")
    if os.path.exists(dist_dir):
        shutil.rmtree(dist_dir)
    os.makedirs(dist_dir)

    props = parse_properties_config()
    app_title = props.get("TITLE", "ESDEngine")
    safe_exe_name = "".join(c for c in app_title if c.isalnum() or c in " _-") + ".exe"

    print("\n -> Copying executable...")
    dest_exe = os.path.join(dist_dir, safe_exe_name)
    shutil.copy(exe, dest_exe)

    # Optional: copy resources like UI, Server, Config for local testing from this Regular directory
    try:
        shutil.copytree("ui", os.path.join(dist_dir, "ui"))
        shutil.copytree("server", os.path.join(dist_dir, "server"))
        if os.path.exists("properties.config"):
            shutil.copy("properties.config", dist_dir)
    except Exception as e:
        print(f" -> [Warning] Skipping some asset copies: {e}")

    print(f"\n[Success] Built locally for testing: {dest_exe}")

def build_standalone():
    print_header("SANDBOX / SELF-SUSTAINED BUILD")
    print("Building a completely self-contained distribution folder (Standalone Sandbox)...")
    print("This will embed the official Python runtime alongside the app, increasing file size but making it 100% portable.")
    
    run_cmake_build()
    exe = find_exe()
    if not exe:
        print("[Error] Executable not found after build.")
        return

    dist_dir = os.path.join("dist", "Standalone")
    if os.path.exists(dist_dir):
        shutil.rmtree(dist_dir)
    os.makedirs(dist_dir)

    props = parse_properties_config()
    app_title = props.get("TITLE", "ESDEngine")
    safe_exe_name = "".join(c for c in app_title if c.isalnum() or c in " _-") + ".exe"

    print("\n -> Copying executable...")
    shutil.copy(exe, os.path.join(dist_dir, safe_exe_name))
    
    print(" -> Copying UI and Server assets...")
    shutil.copytree("ui", os.path.join(dist_dir, "ui"))
    shutil.copytree("server", os.path.join(dist_dir, "server"))
    
    if os.path.exists("properties.config"):
        shutil.copy("properties.config", dist_dir)

    print(" -> Bundling Visual C++ Runtime libraries...")
    # Bundle MSVCP140.dll and VCRUNTIME to prevent "DLL not found" on fresh Windows
    sys32 = os.path.join(os.environ.get('WINDIR', 'C:\\Windows'), 'System32')
    for dll_name in ['msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll']:
        dll_path = os.path.join(sys32, dll_name)
        if os.path.exists(dll_path):
            shutil.copy(dll_path, dist_dir)

    print(f" -> Downloading Windows Embeddable Python ({sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro})...")
    # Fetch the exact Python version currently being used to build the engine
    embed_url = f"https://www.python.org/ftp/python/{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}/python-{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}-embed-amd64.zip"
    
    zip_path = "python-embed.zip"
    try:
        urllib.request.urlretrieve(embed_url, zip_path)
        with zipfile.ZipFile(zip_path, 'r') as zip_ref:
            zip_ref.extractall(dist_dir)
        os.remove(zip_path)
        print(f"\n[Success] Standalone build complete! Location: {os.path.abspath(dist_dir)}")
        print("You can zip this folder and send it to anyone. No installation is required!")
    except Exception as e:
        print(f"[Error] Failed to download/extract Python embed: {e}")
        print("Please ensure your Python version has a valid Windows embeddable package on python.org.")

def find_inno_setup():
    local_appdata = os.environ.get('LOCALAPPDATA', '')
    paths = [
        r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        r"C:\Program Files\Inno Setup 6\ISCC.exe",
        r"C:\Program Files (x86)\Inno Setup 5\ISCC.exe",
        r"C:\Program Files\Inno Setup 5\ISCC.exe"
    ]
    if local_appdata:
        paths.append(os.path.join(local_appdata, r"Programs\Inno Setup 6\ISCC.exe"))
        paths.append(os.path.join(local_appdata, r"Programs\Inno Setup 5\ISCC.exe"))

    for p in paths:
        if os.path.exists(p):
            return p
    return shutil.which("ISCC")

def install_inno_setup_if_needed():
    print("\n[Warning] Inno Setup compiler (ISCC.exe) was not found on your system.")
    choice = input("Would you like to automatically install Inno Setup using Windows Package Manager (winget)? (y/n): ").strip().lower()
    if choice == 'y':
        print("\n -> Installing Inno Setup... (This may ask for Administrator privileges)")
        try:
            subprocess.run(["winget", "install", "--id", "JRSoftware.InnoSetup", "--silent", "--accept-package-agreements", "--accept-source-agreements"], check=True)
            print("\n[Success] Inno Setup installed successfully!")
            return True
        except FileNotFoundError:
            print("\n[Error] 'winget' command not found. Windows Package Manager is not available.")
        except subprocess.CalledProcessError as e:
            print(f"\n[Error] Failed to install Inno Setup via winget: {e}")
    return False

def parse_properties_config():
    props = {
        "TITLE": "ESD Suite Application",
        "VERSION": "1.0.0",
        "AUTHOR": "Ecxo Softwares"
    }
    config_path = "properties.config"
    if os.path.exists(config_path):
        with open(config_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and "=" in line and not line.startswith("#"):
                    key, val = line.split("=", 1)
                    props[key.strip()] = val.strip()
    return props

def build_installer():
    print_header("INSTALLER BUILD")
    print("Preparing an installation package setup...")
    print("This will create a Standalone build and generate a setup installer script.")
    
    props = parse_properties_config()
    app_title = props.get("TITLE", "ESD Suite Application")
    app_version = props.get("VERSION", "1.0.0")
    app_publisher = props.get("AUTHOR", "Ecxo Softwares")
    safe_name = "".join(c for c in app_title if c.isalnum())
    safe_exe_name = "".join(c for c in app_title if c.isalnum() or c in " _-") + ".exe"
    
    dist_dir = os.path.join("dist", "Standalone")
    
    # Always rebuild so we guarantee the latest properties.config and Icon changes are applied
    build_standalone()
    
    print("\n -> Generating Inno Setup Installer Script (.iss)...")

    print(" -> Downloading Visual C++ Redistributable installer to bundle...")
    vcredist_url = "https://aka.ms/vs/17/release/vc_redist.x64.exe"
    vcredist_path = os.path.join(dist_dir, "vc_redist.x64.exe")
    if not os.path.exists(vcredist_path):
        try:
            urllib.request.urlretrieve(vcredist_url, vcredist_path)
            print("    [Success] Downloaded VC++ Redistributable.")
        except Exception as e:
            print(f"    [Error] Failed to download VC++ Redist: {e}")

    icon_prop = get_or_convert_icon(props)
    setup_icon_line = ""
    if icon_prop and os.path.exists(icon_prop):
        setup_icon_line = f"SetupIconFile={os.path.abspath(icon_prop)}\n"

    iss_content = f"""
[Setup]
AppName={app_title}
AppVersion={app_version}
AppPublisher={app_publisher}
{setup_icon_line}DefaultDirName={{autopf}}\\{safe_name}
DefaultGroupName={app_title}
OutputDir=dist
OutputBaseFilename={safe_name}_Installer
Compression=lzma
SolidCompression=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64

[Files]
Source: "{os.path.abspath(dist_dir)}\\*"; DestDir: "{{app}}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{{group}}\\{app_title}"; Filename: "{{app}}\\{safe_exe_name}"
Name: "{{autodesktop}}\\{app_title}"; Filename: "{{app}}\\{safe_exe_name}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"

[Run]
Filename: "{{app}}\\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Visual C++ Redistributable..."; Flags: waituntilterminated
"""
    iss_dir = "dist"
    if not os.path.exists(iss_dir):
        os.makedirs(iss_dir)
        
    iss_path = os.path.join(iss_dir, "installer.iss")
    with open(iss_path, "w") as f:
        f.write(iss_content.strip())
        
    print(f"\n[Success] Created installer generation script at: {os.path.abspath(iss_path)}")
    
    iscc_path = find_inno_setup()
    
    if not iscc_path:
        if install_inno_setup_if_needed():
            iscc_path = find_inno_setup() # Check again after installation
            
    if iscc_path:
        print(f"\n -> Found Inno Setup Compiler at: {iscc_path}")
        print(" -> Compiling installer executable. Please wait...")
        try:
            subprocess.run([iscc_path, iss_path], check=True)
            installer_exe = os.path.abspath(os.path.join(iss_dir, "ESDSuite_Installer.exe"))
            print(f"\n[Success] Your installer has been successfully built: {installer_exe}")
        except subprocess.CalledProcessError as e:
            print(f"\n[Error] Failed to compile installer: {e}")
    else:
        print("\n" + "-"*50)
        print(" TO FINISH BUILDING YOUR SETUP.EXE:")
        print(" 1. Download & Install 'Inno Setup' from: https://jrsoftware.org/isdl.php")
        print(f" 2. Right-click the file '{iss_path}'")
        print(" 3. Select 'Compile'")
        print(" 4. Your Setup.exe will magically appear in the 'dist' folder!")
        print("-" * 50)

def main():
    while True:
        print_header("ESD Suite - Distribution Build Manager")
        print(" 1. Installer         (Creates a Standalone app + generated Setup.exe script)")
        print(" 2. Sandbox/Standalone(Builds a portable folder with embedded Python - No setup required)")
        print(" 3. Regular           (Natively builds .exe - Local Dev Testing ONLY)")
        print(" 4. Exit")
        
        choice = input("\nSelect build type (1/2/3/4): ").strip()
        
        if choice == '1':
            build_installer()
            break
        elif choice == '2':
            build_standalone()
            break
        elif choice == '3':
            build_regular()
            break
        elif choice == '4':
            sys.exit(0)
        else:
            print("Invalid selection. Please type a number.")

if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\nExiting builder.")
        sys.exit(0)