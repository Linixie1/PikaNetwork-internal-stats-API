# Cutie - PikaNetwork Internal Stats API & Tablist Overlay

If you are looking for **overlays for Pika Network**, Cutie is an internal stats modification for Minecraft 1.8.9 that provides a far superior alternative. Instead of drawing clunky external screen overlays on top of your game window, Cutie acts as a native Pika Network overlay by rendering real-time statistics directly inside the Minecraft tab list.

![Loader Menu](loader_preview.png)

> **Is this a cheat / bannable?**  
> Cutie is not a cheat. It acts purely as a stats viewer and offers zero gameplay advantages. Traditional Pika Network stats overlays have never been bannable. To prove it is safe, the project is published under a strict Source-Available Restricted License purely for transparency and security auditing. The license explicitly forbids any malicious or harmful use.  
> 
> That being said, Cutie works by injecting directly into Minecraft's memory so it can render the stats overlay right inside the native tablist. Because it injects like a traditional client modification, people might get banned if they are screenshared by staff. Besides screenshares though, you generally should not have any issues. Just keep the injection risk in mind and use it at your own discretion.

## Supported Clients (1.8.9)
Cutie is built for **Minecraft 1.8.9** and natively supports:  

- <img src="https://www.google.com/s2/favicons?domain=minecraft.net&sz=16" width="16" height="16"/> **Vanilla**
- <img src="https://www.google.com/s2/favicons?domain=minecraftforge.net&sz=16" width="16" height="16"/> **Forge**
- <img src="https://www.google.com/s2/favicons?domain=lunarclient.com&sz=16" width="16" height="16"/> **Lunar Client** (OptiFine & Forge)
- <img src="https://www.google.com/s2/favicons?domain=badlion.net&sz=16" width="16" height="16"/> **Badlion Client** (Badlion and Optifine)
- <img src="https://www.google.com/s2/favicons?domain=labymod.net&sz=16" width="16" height="16"/> **LabyMod** (Vanilla & Forge)
- <img src="https://www.google.com/s2/favicons?domain=cm-pack.pl&sz=16" width="16" height="16"/> **CM Client**

*Note: Other 1.8.9 clients may work, but the ones listed above are fully tested. Also, Silent Client is explicitly NOT supported.*

---

## Setup & Loading Options

You can load Cutie using the standalone loader executable or inject the DLL manually using your own injector.

### Option 1: Standalone Loader (Recommended)
1. Download `cutie-loader.exe` from the latest release.
2. Open `cutie-loader.exe` and launch your Minecraft client. (Doesn't matter which order you open them in; the loader will just sit in the background until MC is running).
3. The loader will automatically unpack the internal DLL and inject your in-game overlay.

*Note: You don't need to be in a world to inject, and you can do whatever you want in-game while waiting. If your Minecraft client starts slow, the injection might fail, in such cases open the loader after the game is fully loaded (in main menu).*

### Option 2: Manual DLL Injection
1. Download `cutie.dll` from the release assets.
2. Inject `cutie.dll` into your game process using any standard 64-bit injector (e.g., Process Hacker).

---

## Antivirus False Positives

Because the loader reads game memory and injects code into Minecraft, **Windows Defender or other antivirus software might falsely flag `cutie-loader.exe` and delete it.** The standalone `cutie.dll` usually doesn't trigger this, but if you're using the loader, you'll need to whitelist it.

*Note: Fully turning off your antivirus is NOT recommended and completely unnecessary. Only exclude the specific folder you put the loader in.*

**Quick Fix for Windows Defender:**
1. Create a new folder on your Desktop named `Cutie`.
2. Open Windows PowerShell as **Administrator**.
3. Copy, paste, and run this command to automatically exclude that folder from Defender:
   ```powershell
   Add-MpPreference -ExclusionPath "$env:USERPROFILE\Desktop\Cutie"
      ```

## Logs & Troubleshooting

If you run into injection issues, crashes, or rendering bugs with the stats overlay:
- **Loader Logs**: Saved in the folder where `cutie-loader.exe` was executed.
- **DLL Logs**: Saved at `%userprofile%\.cutie\log.txt`.

---

## Tablist Status Indicators

When viewing stats in-game or via the tab list overlay:
- **NICK**: Player is using a nickname.
- **OFF**: Player has hidden their stats on the API.
- **N/A**: Player stats are unavailable (e.g., brand-new account with 0 stats).

---

## Previews

Unlike traditional external overlays, configuring your stats display is done entirely in-game.

| In-Game ClickGUI | Display Settings |
| :---: | :---: |
| ![ClickGUI Configuration](clickgui_preview.png) | ![Stats API Config](stats_api_preview.png) |
| **Tablist Modification Injection** | **Stats API Module Configuration** |
| ![Tablist Injection](tablist_injection_preview.png) | ![Settings Interface](settings_preview.png) |

### Live Stats Tablist Display
![Stats Display](stats_display_preview.png)

---

## Support
For bugs or questions, reach out on Discord: `linixie.`

---

### Credits & Acknowledgements
- [<img src="https://avatars.githubusercontent.com/u/1010356?s=32&v=4" width="16" height="16" />](https://github.com/nlohmann/json) **[nlohmann/json](https://github.com/nlohmann/json)** - JSON for Modern C++. 
- [<img src="https://avatars.githubusercontent.com/u/8225057?s=32&v=4" width="16" height="16" />](https://github.com/ocornut/imgui) **[Dear ImGui](https://github.com/ocornut/imgui)** - Bloat-free Immediate Mode Graphical User Interface for C++ with minimal dependencies.
- [<img src="https://github.com/YuriSizuku.png?size=32" width="16" height="16" />](https://github.com/YuriSizuku/OnscripterYuri) **[ONScripter-Yuri](https://github.com/YuriSizuku/OnscripterYuri)** - An enhanced ONScripter project porting to many platforms.
