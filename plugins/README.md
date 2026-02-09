# Biko Plugins

Place plugin DLLs (*.dll) in this folder. They will be loaded automatically when Biko starts.

## Developing Plugins

See `src/Extension/BikoPluginAPI.h` for the plugin API.

### Required Exports

Every plugin must export these functions:

```c
BikoPluginInfo* BikoPlugin_GetInfo(void);
BOOL BikoPlugin_Init(const BikoHostServices* pHost);
void BikoPlugin_Shutdown(void);
```

### Optional Exports

To add menu items:

```c
int BikoPlugin_GetMenuItemCount(void);
BOOL BikoPlugin_GetMenuItem(int index, BikoMenuItem* pItem);
BOOL BikoPlugin_OnCommand(UINT cmdId);
```

### Building

```cmd
cl /LD /DBIKO_PLUGIN_EXPORTS myplugin.c /Fe:myplugin.dll
```

Include path must contain `src/Extension/BikoPluginAPI.h`.

## Sample Plugin

See `sample_plugin.c` for a complete example that demonstrates:
- Plugin info export
- Menu item registration
- Command handling
- Using host services (text manipulation, undo, status bar)
