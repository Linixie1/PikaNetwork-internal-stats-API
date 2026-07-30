# Cutie-Loader & Stats API

A high-performance statistics integration and modular overlay for PikaNetwork. This project provides both a standalone loader interface and the raw modification DLL.

![Loader Menu](loader_preview.png)

## Core Features
- **Statistics Integration**: Real-time stats calculation displayed directly in-game.
- **Tablist Enhancements**: Dynamic modifications to the network tablist.
- **Modular ClickGUI**: Easy configuration of features, stats API endpoints, and visual overlays.
- **Embedded Injection Engine**: Self-contained manual mapping loader with zero configuration needed.

## Setup & Loading Options

The release package includes both `cutie-loader.exe` and `cutie.dll`. You can load the modification using either method:

### Option 1: Standalone Loader (Recommended)
1. Download `cutie-loader.exe`.
2. Launch the game client.
3. Run `cutie-loader.exe`. It will automatically initialize, unpack the embedded DLL/configs, and inject them.

### Option 2: Custom Injection
1. Download `cutie.dll` from the latest release.
2. Use any standard DLL injector (e.g., Process Hacker, Cheat Engine, or a custom manual mapper).
3. Inject `cutie.dll` into the game process.

---

## Status Displays & Indicators
When viewing statistics in-game or via the tablist, the following status indicators are used:
- **NICK**: The player is nicked (using a nickname/disguise).
- **OFF**: Statistics tracking is turned off or disabled.
- **N/A**: Statistics are not available (e.g., a new account with all 0 stats).

---

## Previews & Interface

### In-Game ClickGUI
![ClickGUI Configuration](clickgui_preview.png)

### Stats API Module Configuration
![Stats API Config](stats_api_preview.png)

### Tablist Modification Injection
![Tablist Injection](tablist_injection_preview.png)

### Features & Display Settings
![Settings Interface](settings_preview.png)

### Live Stats Tablist Display
![Stats Display](stats_display_preview.png)

---

## Developer Support
For inquiries, bug reporting, or custom integration support:
- **Discord**: `linixie.`
