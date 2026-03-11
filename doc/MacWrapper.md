/******************************************************************************
*
* Bikode
*
* MacWrapper.md
*   Compatibility distribution path for macOS.
*
******************************************************************************/

# Bikode on macOS

The easiest route that stays easy to update is:

1. Keep shipping one real app build: the Windows `Bikode.exe`
2. Wrap that binary in a thin macOS launcher app
3. Run it through Wine or CrossOver on Mac

This is a compatibility path, not a native Mac port.

## Why this route

- It keeps Windows as the single source of truth
- It avoids a second UI codebase
- Updating Mac releases becomes mostly a repackaging step
- It is much smaller in scope than rewriting the editor shell for Qt, wxWidgets, or Cocoa

## What the wrapper does

The wrapper app bundles:

- `Bikode.exe`
- `Bikode.ini`
- a tiny macOS launcher script
- optionally, an embedded Wine runtime for a one-download distribution

At runtime the launcher:

- prefers a bundled Wine runtime when one was embedded in the app
- looks for `wine64`, `wine`, or CrossOver's bundled wine binary
- creates a dedicated prefix under `~/Library/Application Support/Bikode/`
- launches the same Windows Bikode build from the app bundle

## Build the Mac wrapper

Preferred route from this repo on Windows or any machine with Python 3:

```powershell
python scripts/build-mac-wrapper.py --app-version 1.0.0
```

For a one-download Mac build that does not require users to install Wine separately:

```powershell
python scripts/build-mac-wrapper.py `
  --app-version 1.0.0 `
  --runtime-dir C:\path\to\wine-runtime
```

If you are starting from a runtime archive such as a Wineskin-compatible engine:

```powershell
python scripts/build-mac-wrapper.py `
  --app-version 1.0.0 `
  --runtime-archive C:\path\to\WS11Wine10.0_3.tar.xz
```

If you are packaging on a Mac and want the shell-based build path:

```bash
chmod +x scripts/build-mac-wrapper.sh
./scripts/build-mac-wrapper.sh --app-version 1.0.0
```

Default inputs:

- Windows build: `bin/x64/Release/Bikode.exe`
- INI: `bin/x64/Release/Bikode.ini`
- Output zip: `dist/Bikode-mac-wrapper-<version>.zip`
- Embedded runtime: optional, provided through `--runtime-dir`

You can override them with the Python packager:

```powershell
python scripts/build-mac-wrapper.py `
  --source-dir C:\path\to\windows-release `
  --output-dir C:\path\to\dist `
  --app-version 1.0.0
```

## Release/update flow

For each Bikode release:

1. Build the normal Windows release
2. Reuse that exact `Bikode.exe` for the Mac wrapper
3. Run `python scripts/build-mac-wrapper.py --app-version <version>`
4. For the best user experience, provide `--runtime-dir` so the app is self-contained
5. Upload the produced zip as a separate release asset

That means the Mac update path stays aligned with Windows updates instead of drifting.

## Windows-side release shortcut

For the repeatable release path from this repo on Windows:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/publish-release-assets.ps1
```

That script:

- builds the Windows installer unless you pass `-SkipBuild`
- downloads the configured runtime archive
- builds the self-contained Mac zip
- uploads both assets to the GitHub release
- syncs the website download files

## Limits

- This is not a native `.app` implementation of the editor
- Users need no extra install only when the app was built with an embedded runtime
- Proper signing/notarization still needs to happen on a Mac host if you want a polished public distribution

## When to replace this

Only replace this route if Bikode is ready for a real cross-platform port.
Until then, the wrapper path is the least expensive way to support Mac users without doubling the maintenance burden.
