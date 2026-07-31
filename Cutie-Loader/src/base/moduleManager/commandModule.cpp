#include "commandModule.h"
#include "base/sdk/strayCache.h"
#include "base/menu/menu.h"
#include "base/configManager/settings.h"
#include "base/java/java.h"
#include "base/util/logger.h"
#include <string>

static void AddLocalChat(std::string text) {
	if (!StrayCache::entityPlayerSP_addChatMessage) return;
	jobject player = SDK::minecraft->GetInstance() ? Java::env->GetObjectField(SDK::minecraft->GetInstance(), StrayCache::minecraft_thePlayer) : nullptr;
	if (!player) return;
	jstring jText = Java::env->NewStringUTF(text.c_str());
	if (StrayCache::chatComponentText_class && StrayCache::chatComponentText_init) {
		jobject comp = Java::env->NewObject(StrayCache::chatComponentText_class, StrayCache::chatComponentText_init, jText);
		if (comp) { Java::env->CallVoidMethod(player, StrayCache::entityPlayerSP_addChatMessage, comp); Java::env->DeleteLocalRef(comp); }
	}
	Java::env->DeleteLocalRef(jText);
	Java::env->DeleteLocalRef(player);
}
// only supports official vanila and forge, dosent support lunar/badlion or anything modified
// the GUI auto opens on start unless disabled to let users who dont have slash key set a custom keybind
static void ProcessCommand(const std::string& msg) {
	if (msg == "/gui" || msg == "/ui" || msg == "/clickgui" || msg == "/screen" || msg == "/openscreen") {
		Menu::open = true;
	} else if (msg.find("/bind gui ") == 0 || msg.find("/gui ") == 0) {
		if (msg == "/gui help") {
			char buf[128];
			snprintf(buf, sizeof(buf), "\247d[Cutie] \247fThe ClickGUI is bound to: \247e%d", settings::Menu_Keybind);
			AddLocalChat(buf); Menu::open = true;
		} else {
			std::string keyStr = msg.find("/bind gui ") == 0 ? msg.substr(10) : msg.substr(5);
			try {
				int key = std::stoi(keyStr);
				settings::Menu_Keybind = key; Menu::bPendingConfigSave = true;
				char buf[128];
				snprintf(buf, sizeof(buf), "\247d[Cutie] \247fClickGUI bound to key code: \247e%d", key);
				AddLocalChat(buf);
			} catch (...) { AddLocalChat("\247c[Cutie] Invalid key code."); }
		}
	}
}

void CommandModule::Init() {
	LOG_INFO("[CMD] Initialising command module (keybind-based)...");
}