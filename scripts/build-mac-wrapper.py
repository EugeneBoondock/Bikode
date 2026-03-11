#!/usr/bin/env python3
"""
Build a macOS compatibility wrapper around the Windows Bikode release.

This does not create a native macOS app. It packages the existing
Windows Bikode.exe inside a .app bundle that launches through Wine or
CrossOver on the Mac.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import tarfile
import textwrap
import time
import zipfile
from pathlib import Path


APP_NAME = "Bikode"
DEFAULT_BUNDLE_ID = "com.boondocklabs.bikode"
LAUNCHER_NAME = "bikode-launcher"


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parent.parent
    default_source = repo_root / "bin" / "x64" / "Release"
    default_output = repo_root / "dist"

    parser = argparse.ArgumentParser(
        description=(
            "Build a macOS .app compatibility wrapper around the Windows "
            "Bikode.exe release output."
        )
    )
    parser.add_argument("--source-dir", default=str(default_source))
    parser.add_argument("--exe")
    parser.add_argument("--ini")
    parser.add_argument("--output-dir", default=str(default_output))
    parser.add_argument("--app-version")
    parser.add_argument("--bundle-id", default=DEFAULT_BUNDLE_ID)
    parser.add_argument(
        "--runtime-dir",
        help=(
            "Optional folder containing a bundled Wine runtime. If provided, "
            "the generated .app becomes self-contained and will prefer the "
            "embedded runtime before checking the Mac system."
        ),
    )
    parser.add_argument(
        "--runtime-archive",
        help=(
            "Optional runtime archive to extract into the app bundle. Useful "
            "for Wine engine archives such as WS11Wine10.0_3.tar.xz."
        ),
    )
    return parser.parse_args()


def resolve_inputs(args: argparse.Namespace) -> tuple[Path, Path | None, Path, str, Path | None, Path | None]:
    source_dir = Path(args.source_dir).resolve()
    exe_path = Path(args.exe).resolve() if args.exe else source_dir / f"{APP_NAME}.exe"
    ini_path = Path(args.ini).resolve() if args.ini else source_dir / f"{APP_NAME}.ini"
    output_dir = Path(args.output_dir).resolve()
    runtime_dir = Path(args.runtime_dir).resolve() if args.runtime_dir else None
    runtime_archive = Path(args.runtime_archive).resolve() if args.runtime_archive else None

    if not exe_path.is_file():
        raise FileNotFoundError(f"{APP_NAME}.exe not found: {exe_path}")
    if runtime_dir and runtime_archive:
        raise ValueError("Use either --runtime-dir or --runtime-archive, not both.")
    if runtime_dir and not runtime_dir.is_dir():
        raise FileNotFoundError(f"Runtime directory not found: {runtime_dir}")
    if runtime_archive and not runtime_archive.is_file():
        raise FileNotFoundError(f"Runtime archive not found: {runtime_archive}")

    resolved_ini = ini_path if ini_path.is_file() else None
    if args.app_version:
        version = args.app_version.strip()
    else:
        version = detect_version(exe_path) or "0.0.0"

    return exe_path, resolved_ini, output_dir, version, runtime_dir, runtime_archive


def detect_version(exe_path: Path) -> str | None:
    command = [
        "powershell",
        "-NoLogo",
        "-NoProfile",
        "-Command",
        f"(Get-Item '{exe_path}').VersionInfo.FileVersion",
    ]
    try:
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            check=True,
        )
    except Exception:
        return None

    version = result.stdout.strip()
    return version or None


def launcher_script() -> str:
    return textwrap.dedent(
        """\
        #!/usr/bin/env bash
        set -euo pipefail

        APP_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
        WINDOWS_DIR="$APP_ROOT/Resources/windows"
        RUNTIME_DIR="$APP_ROOT/Resources/runtime"
        RUNTIME_BUNDLE_DIR="$RUNTIME_DIR/wswine.bundle"
        EXE_PATH="$WINDOWS_DIR/Bikode.exe"
        PREFIX_BASE="$HOME/Library/Application Support/Bikode"
        WINE_PREFIX_PATH="$PREFIX_BASE/wineprefix"

        show_message() {
          local message="$1"
          if command -v osascript >/dev/null 2>&1; then
            osascript -e "display dialog \\"${message//\\"/\\\\\\"}\\" buttons {\\"OK\\"} default button \\"OK\\" with title \\"Bikode\\""
          else
            echo "$message" >&2
          fi
        }

        find_wine() {
          local candidate
          for candidate in \
            "$RUNTIME_DIR/bin/wine64" \
            "$RUNTIME_DIR/bin/wine" \
            "$RUNTIME_BUNDLE_DIR/bin/wine64" \
            "$RUNTIME_BUNDLE_DIR/bin/wine" \
            "${WINE_BIN:-}" \
            "$(command -v wine64 2>/dev/null || true)" \
            "$(command -v wine 2>/dev/null || true)" \
            "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine" \
            "/opt/homebrew/bin/wine64" \
            "/usr/local/bin/wine64" \
            "/opt/homebrew/bin/wine" \
            "/usr/local/bin/wine"; do
            if [[ -n "$candidate" && -x "$candidate" ]]; then
              printf '%s\\n' "$candidate"
              return 0
            fi
          done
          return 1
        }

        if [[ ! -f "$EXE_PATH" ]]; then
          show_message "Bikode.exe is missing from the app bundle."
          exit 1
        fi

        if ! WINE_CMD="$(find_wine)"; then
          show_message "Bikode on macOS currently needs Wine or CrossOver. Install one of them, then relaunch Bikode."
          exit 1
        fi

        mkdir -p "$PREFIX_BASE"
        export WINEPREFIX="$WINE_PREFIX_PATH"
        export WINEDLLOVERRIDES="mscoree,mshtml="

        cd "$WINDOWS_DIR"
        exec "$WINE_CMD" "$EXE_PATH"
        """
    )


def info_plist(bundle_id: str, version: str) -> str:
    return textwrap.dedent(
        f"""\
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
          <key>CFBundleName</key>
          <string>{APP_NAME}</string>
          <key>CFBundleDisplayName</key>
          <string>{APP_NAME}</string>
          <key>CFBundleIdentifier</key>
          <string>{bundle_id}</string>
          <key>CFBundleVersion</key>
          <string>{version}</string>
          <key>CFBundleShortVersionString</key>
          <string>{version}</string>
          <key>CFBundlePackageType</key>
          <string>APPL</string>
          <key>CFBundleExecutable</key>
          <string>{LAUNCHER_NAME}</string>
          <key>LSMinimumSystemVersion</key>
          <string>12.0</string>
          <key>NSHighResolutionCapable</key>
          <true/>
        </dict>
        </plist>
        """
    )


def readme_text(version: str, has_embedded_runtime: bool) -> str:
    requirement_line = (
        "- No extra install required. This build includes an embedded Wine runtime."
        if has_embedded_runtime
        else "- Wine or CrossOver installed on the Mac"
    )
    return textwrap.dedent(
        f"""\
        Bikode macOS compatibility wrapper
        =================================

        Version: {version}

        This package launches the Windows Bikode build through Wine or CrossOver.
        It is not a native macOS port.

        Requirements:
        {requirement_line}

        Bundle contents:
        - The same Bikode.exe used for the Windows release
        - A thin macOS .app launcher that starts that binary

        First launch:
        - Extract the zip
        - Move Bikode.app into Applications if you want
        - Open Bikode.app
        - If macOS warns that the app is from an unidentified developer, use
          Control-click > Open the first time
        """
    )


def write_text(path: Path, content: str, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")
    os.chmod(path, mode)


def copy_file(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def cleanup_tree(path: Path) -> None:
    if not path.exists():
        return
    if os.name == "nt":
        subprocess.run(
            ["cmd", "/c", "rmdir", "/s", "/q", str(path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        return
    shutil.rmtree(path, ignore_errors=True)


def long_path_str(path: Path) -> str:
    raw = str(path)
    if os.name != "nt":
        return raw
    if raw.startswith("\\\\?\\"):
        return raw
    if raw.startswith("\\\\"):
        return "\\\\?\\UNC\\" + raw[2:]
    return "\\\\?\\" + raw


def add_directory_entry(archive: zipfile.ZipFile, arcname: str, mode: int = 0o755) -> None:
    if not arcname.endswith("/"):
        arcname = f"{arcname}/"
    info = zipfile.ZipInfo(arcname)
    info.create_system = 3
    info.external_attr = ((0o040000 | mode) << 16) | 0x10
    archive.writestr(info, b"")


def add_file_entry(archive: zipfile.ZipFile, path: Path, arcname: str, mode: int) -> None:
    stat_info = os.stat(long_path_str(path))
    info = zipfile.ZipInfo(arcname)
    info.date_time = time.localtime(stat_info.st_mtime)[:6]
    info.create_system = 3
    info.external_attr = (mode & 0xFFFF) << 16
    with open(long_path_str(path), "rb") as handle:
        archive.writestr(info, handle.read(), compress_type=zipfile.ZIP_DEFLATED)


def add_bytes_entry(archive: zipfile.ZipFile, data: bytes, arcname: str, mode: int, mtime: float | None = None) -> None:
    info = zipfile.ZipInfo(arcname)
    timestamp = mtime if mtime is not None else time.time()
    info.date_time = time.localtime(timestamp)[:6]
    info.create_system = 3
    info.external_attr = (mode & 0xFFFF) << 16
    archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)


def build_bundle(
    exe_path: Path,
    ini_path: Path | None,
    output_dir: Path,
    version: str,
    bundle_id: str,
    runtime_dir: Path | None,
    runtime_archive: Path | None,
) -> Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    artifact_stem = f"{APP_NAME}-mac-{version}" if (runtime_dir or runtime_archive) else f"{APP_NAME}-mac-wrapper-{version}"
    zip_path = output_dir / f"{artifact_stem}.zip"

    temp_parent = None
    if os.name == "nt":
        temp_parent = f"{exe_path.drive}\\"

    temp_dir = Path(tempfile.mkdtemp(prefix="bkm", dir=temp_parent))
    try:
        app_dir = temp_dir / f"{APP_NAME}.app"
        contents_dir = app_dir / "Contents"
        macos_dir = contents_dir / "MacOS"
        resources_dir = contents_dir / "Resources"
        windows_dir = resources_dir / "windows"
        bundled_runtime_dir = resources_dir / "runtime"

        macos_dir.mkdir(parents=True, exist_ok=True)
        resources_dir.mkdir(parents=True, exist_ok=True)
        windows_dir.mkdir(parents=True, exist_ok=True)

        launcher_path = macos_dir / LAUNCHER_NAME
        write_text(launcher_path, launcher_script(), mode=0o755)
        write_text(contents_dir / "Info.plist", info_plist(bundle_id, version))
        write_text(resources_dir / "README-mac.txt", readme_text(version, (runtime_dir is not None) or (runtime_archive is not None)))

        repo_root = Path(__file__).resolve().parent.parent
        copy_file(exe_path, windows_dir / f"{APP_NAME}.exe")
        if ini_path:
            copy_file(ini_path, windows_dir / f"{APP_NAME}.ini")
        copy_file(repo_root / "LICENSE", resources_dir / "LICENSE.txt")
        if runtime_dir:
            shutil.copytree(runtime_dir, bundled_runtime_dir, dirs_exist_ok=True)

        if zip_path.exists():
            zip_path.unlink()

        with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            directory_entries = [
                f"{APP_NAME}.app/",
                f"{APP_NAME}.app/Contents/",
                f"{APP_NAME}.app/Contents/MacOS/",
                f"{APP_NAME}.app/Contents/Resources/",
                f"{APP_NAME}.app/Contents/Resources/windows/",
            ]
            if runtime_dir or runtime_archive:
                directory_entries.append(f"{APP_NAME}.app/Contents/Resources/runtime/")
            for entry in directory_entries:
                add_directory_entry(archive, entry)

            file_modes = {
                launcher_path: 0o755,
                contents_dir / "Info.plist": 0o644,
                resources_dir / "README-mac.txt": 0o644,
                resources_dir / "LICENSE.txt": 0o644,
                windows_dir / f"{APP_NAME}.exe": 0o644,
            }
            if ini_path:
                file_modes[windows_dir / f"{APP_NAME}.ini"] = 0o644
            if runtime_dir:
                for runtime_file in bundled_runtime_dir.rglob("*"):
                    if runtime_file.is_dir():
                        add_directory_entry(
                            archive,
                            runtime_file.relative_to(temp_dir).as_posix(),
                        )
                    else:
                        relative_runtime_parts = runtime_file.relative_to(bundled_runtime_dir).parts
                        runtime_mode = 0o644
                        if "bin" in relative_runtime_parts:
                            runtime_mode = 0o755
                        elif runtime_file.suffix == "":
                            runtime_mode = 0o755
                        if runtime_file.name.startswith("wine"):
                            runtime_mode = 0o755
                        file_modes[runtime_file] = runtime_mode
            elif runtime_archive:
                with tarfile.open(runtime_archive, "r:*") as runtime_tar:
                    for member in runtime_tar.getmembers():
                        member_name = member.name.strip("/")
                        if not member_name:
                            continue
                        arcname = f"{APP_NAME}.app/Contents/Resources/runtime/{member_name}"
                        if member.isdir():
                            add_directory_entry(archive, arcname)
                            continue
                        if not member.isfile():
                            continue
                        relative_runtime_parts = Path(member_name).parts
                        runtime_mode = 0o644
                        if "bin" in relative_runtime_parts:
                            runtime_mode = 0o755
                        elif "." not in Path(member_name).name:
                            runtime_mode = 0o755
                        if Path(member_name).name.startswith("wine"):
                            runtime_mode = 0o755
                        extracted = runtime_tar.extractfile(member)
                        if extracted is None:
                            continue
                        add_bytes_entry(
                            archive,
                            extracted.read(),
                            arcname,
                            runtime_mode,
                            member.mtime,
                        )

            for file_path, mode in file_modes.items():
                arcname = file_path.relative_to(temp_dir).as_posix()
                add_file_entry(archive, file_path, arcname, mode)
    finally:
        cleanup_tree(temp_dir)

    return zip_path


def main() -> int:
    args = parse_args()
    try:
        exe_path, ini_path, output_dir, version, runtime_dir, runtime_archive = resolve_inputs(args)
        zip_path = build_bundle(
            exe_path,
            ini_path,
            output_dir,
            version,
            args.bundle_id,
            runtime_dir,
            runtime_archive,
        )
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    print(zip_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
