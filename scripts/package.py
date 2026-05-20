import os
import zipfile
import subprocess

ITEMS_TO_PACKAGE = [
    "properties.config",
    "CMakeLists.txt",
    "ui",
    "server",
    "scripts",
    "public",
    "private",
    "engine",
    "documentation"
]

# Files to exclude from the packaged zip (relative paths, normalized)
EXCLUDED_FILES = [
    os.path.normpath("scripts/package.py"),
]

REPO = "ecxogames/esdk"


def tag_exists(tag: str) -> bool:
    result = subprocess.run(["git", "tag", "-l", tag], capture_output=True, text=True)
    return bool(result.stdout.strip())


def get_tag_message(tag: str) -> str:
    """Returns the annotated tag message body, or falls back to commit message."""
    result = subprocess.run(
        ["git", "tag", "-l", "--format=%(contents)", tag],
        capture_output=True, text=True
    )
    msg = result.stdout.strip()
    if msg:
        return msg
    # Fall back to the commit message pointed to by the tag
    result = subprocess.run(
        ["git", "log", "-1", "--pretty=%B", tag],
        capture_output=True, text=True
    )
    return result.stdout.strip()


def create_zip(zip_filename: str) -> bool:
    try:
        with zipfile.ZipFile(zip_filename, "w", zipfile.ZIP_DEFLATED) as zipf:
            for item in ITEMS_TO_PACKAGE:
                if not os.path.exists(item):
                    print(f"  [Warning] Missing '{item}', skipping.")
                    continue

                if os.path.isfile(item):
                    print(f"  Adding file: {item}")
                    zipf.write(item)
                elif os.path.isdir(item):
                    print(f"  Adding directory: {item}/")
                    for root, dirs, files in os.walk(item):
                        if "__pycache__" in dirs:
                            dirs.remove("__pycache__")
                        for file in files:
                            file_path = os.path.join(root, file)
                            if os.path.normpath(file_path) in EXCLUDED_FILES:
                                print(f"    [Skip] {file_path}")
                                continue
                            zipf.write(file_path, file_path)
        return True
    except Exception as e:
        print(f"\n[Error] Failed to create zip: {e}")
        return False


def create_github_release(tag: str, version: str, zip_filename: str, notes: str) -> bool:
    title = f"Ecxo Software Development Engine - Version {version}"
    try:
        result = subprocess.run(
            [
                "gh", "release", "create", tag, zip_filename,
                "--title", title,
                "--notes", notes,
                "--repo", REPO,
            ],
            capture_output=True, text=True
        )
        if result.returncode == 0:
            print(f"[Success] GitHub release created: {result.stdout.strip()}")
            return True
        else:
            print(f"[Error] GitHub CLI failed:\n{result.stderr.strip()}")
            return False
    except FileNotFoundError:
        print("[Error] 'gh' CLI not found. Install GitHub CLI and authenticate first.")
        print("  https://cli.github.com/")
        return False


def main():
    print("=" * 50)
    print("  ESD Suite Framework Packager")
    print("=" * 50)

    if not os.path.exists("engine") or not os.path.exists("scripts"):
        print("[Error] Please run this script from the project root directory.")
        return

    version = input("\nEnter the version number for this release (e.g., 0.0.15): ").strip()
    if not version:
        print("[Error] Version cannot be empty.")
        return

    # Resolve the git tag (try both 'v{version}' and bare '{version}')
    tag = f"v{version}" if tag_exists(f"v{version}") else version
    if not tag_exists(tag):
        print(f"[Error] Git tag '{tag}' does not exist.")
        print(f"  Create it first:  git tag -a v{version} -m \"Your release notes here\"")
        return

    tag_message = get_tag_message(tag)
    if not tag_message:
        print(f"[Error] Could not read a message from tag '{tag}'. Aborting.")
        return

    zip_filename = f"esd-engine-{version}.zip"
    if os.path.exists(zip_filename):
        print(f"[Warning] '{zip_filename}' already exists and will be overwritten.")

    # Step 1 – Build zip
    print(f"\nPackaging into '{zip_filename}'...")
    if not create_zip(zip_filename):
        return
    print(f"[Success] '{zip_filename}' created.")

    # Step 2 – GitHub release
    print(f"\nCreating GitHub release for tag '{tag}'...")
    create_github_release(tag, version, zip_filename, tag_message)


if __name__ == "__main__":
    main()
