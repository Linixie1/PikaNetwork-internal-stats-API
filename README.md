# Cutie - PikaNetwork Internal Stats API

Cutie is an internal stats modification for Minecraft 1.8.9. Instead of drawing external screen overlays on top of your game, Cutie renders statistics directly inside the native Minecraft tab list.

![Loader Menu](loader_preview.png)

## Supported Clients (1.8.9)
Cutie is built for **Minecraft 1.8.9** and natively supports:
- **Vanilla 1.8.9**
- **Minecraft Forge 1.8.9**
- **Lunar Client** (OptiFine & Forge profiles)
- **Badlion Client**
- **LabyMod** (Vanilla & Forge setups)

*Note: Other 1.8.9 clients may work, but the ones listed above are fully tested.*

---

## Setup & Loading Options

You can load Cutie using the standalone loader executable or inject the DLL manually using your own injector.

### Option 1: Standalone Loader (Recommended)
1. Download `cutie-loader.exe` from the latest release.
2. Launch your Minecraft client and join a world or server.
3. Open `cutie-loader.exe`. It will automatically unpack the internal DLL and inject it.

### Option 2: Manual DLL Injection
1. Download `cutie.dll` from the release assets.
2. Inject `cutie.dll` into your game process using any standard 64-bit injector (e.g. Process Hacker).

---

## Logs & Troubleshooting

If you run into injection issues, crashes, or rendering bugs:
- **Loader Logs**: Saved in the folder where `cutie-loader.exe` was executed.
- **DLL Logs**: Saved at `%userprofile%\.cutie\log.txt`.

---

## Tablist Status Indicators

When viewing stats in-game or via the tab list:
- **NICK**: Player is using a nickname or disguise.
- **OFF**: Stats module is toggled off for that player or category.
- **N/A**: Player stats are unavailable (e.g., brand-new account with 0 stats).

---

## Previews

### In-Game ClickGUI
![ClickGUI Configuration](clickgui_preview.png)

### Display Settings
![Stats API Config](stats_api_preview.png)

### Tablist Modification Injection
![Tablist Injection](tablist_injection_preview.png)

### Stats API Module Configuration
![Settings Interface](settings_preview.png)

### Live Stats Tablist Display
![Stats Display](stats_display_preview.png)

---

## Support
For bugs or questions, reach out on Discord: `linixie.`
