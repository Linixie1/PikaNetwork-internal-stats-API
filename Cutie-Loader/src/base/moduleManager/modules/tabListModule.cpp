#include "tabListModule.h"
#include "../../../menu/menu.h"
#include "../../../configManager/settings.h"
#include "../../../util/logger.h"
#include "../../../sdk/jni_safety.h"
#include "../../../java/java.h"
#include "../../../json/json.hpp"
#include "../../../base.h"
#include "../../../../xorstr.h"
#include <windows.h>
#include <winhttp.h>
#include <sstream>
#include <vector>
#include <algorithm>
#include <initializer_list>
#include <set>
#include <unordered_map>
#include <jvmti.h>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

namespace RuntimeBindings {
    jclass minecraft_class = nullptr;
    jmethodID minecraft_getMinecraft = nullptr;
    jfieldID minecraft_ingameGUI = nullptr;
    jfieldID minecraft_theWorld = nullptr;
    jmethodID minecraft_getNetHandler = nullptr;

    jclass guiIngameClass = nullptr;
    jfieldID guiIngame_overlayPlayerList = nullptr;
    jmethodID guiIngame_getTabList = nullptr;

    jclass guiPlayerTabOverlayClass = nullptr;
    jfieldID guiPlayerTabOverlay_footer = nullptr;

    jclass netHandlerClass = nullptr;
    jfieldID netHandlerPlayClient_playerInfoList = nullptr;
    jmethodID netHandlerPlayClient_getPlayerInfoMap = nullptr;

    jclass networkInfoClass = nullptr;
    jmethodID networkPlayerInfo_getGameProfile = nullptr;
    jmethodID networkPlayerInfo_getDisplayName = nullptr;
    jmethodID networkPlayerInfo_setDisplayName = nullptr;
    jmethodID networkPlayerInfo_getResponseTime = nullptr;
    jmethodID networkPlayerInfo_getPlayerTeam = nullptr;

    jclass gameProfile_class = nullptr;
    jmethodID gameProfile_getName = nullptr;

    jclass worldClass = nullptr;
    jmethodID world_getScoreboard = nullptr;

    jclass scoreboardClass = nullptr;
    jmethodID scoreboard_getObjectiveInDisplaySlot = nullptr;
    jmethodID scoreboard_getSortedScores = nullptr;
    jmethodID scoreboard_getPlayersTeam = nullptr;

    jclass scoreObjectiveClass = nullptr;
    jmethodID scoreObjective_getDisplayName = nullptr;

    jclass scoreClass = nullptr;
    jmethodID score_getPlayerName = nullptr;

    jclass scorePlayerTeam_class = nullptr;
    jmethodID scorePlayerTeam_formatPlayerName = nullptr;
    jmethodID scorePlayerTeam_getColorPrefix = nullptr;
    jmethodID scorePlayerTeam_getColorSuffix = nullptr;

    jclass ichatcomponent_class = nullptr;
    jmethodID ichatcomponent_getFormattedText = nullptr;
    jclass chatComponentText_class = nullptr;
    jmethodID chatComponentText_init = nullptr;

    jmethodID chatComponent_appendSibling = nullptr;
    jmethodID chatComponent_createCopy = nullptr;

    jclass map_class = nullptr;
    jmethodID map_values = nullptr;
    jclass collection_class = nullptr;
    jmethodID collection_iterator = nullptr;
    jclass iterator_class = nullptr;
    jmethodID iterator_hasNext = nullptr;
    jmethodID iterator_next = nullptr;

    bool initialized = false;
    std::mutex mutex;

    static bool clear() {
        if (Java::env && Java::env->ExceptionCheck()) {
            Java::env->ExceptionClear();
            return true;
        }
        return false;
    }

}


namespace runtime_resolver {
    struct MethodMeta { jmethodID id = nullptr; std::string sig; jint mods = 0; };
    struct FieldMeta { jfieldID id = nullptr; std::string sig; jint mods = 0; };

    static bool clear() {
        if (Java::env && Java::env->ExceptionCheck()) {
            Java::env->ExceptionClear();
            return true;
        }
        return false;
    }

    static bool isObjectSig(const std::string& s) {
        return s.size() > 2 && s.front() == 'L' && s.back() == ';';
    }

    static int argc(const std::string& sig) {
        if (sig.empty() || sig.front() != '(') return -1;
        size_t p = sig.find(')');
        if (p == std::string::npos) return -1;
        int n = 0;
        for (size_t i = 1; i < p;) {
            char c = sig[i++];
            if (c == 'L') {
                size_t e = sig.find(';', i);
                if (e == std::string::npos) return -1;
                i = e + 1;
            } else if (c == '[') {
                while (i < p && sig[i] == '[') ++i;
                if (i < p && sig[i] == 'L') {
                    size_t e = sig.find(';', i);
                    if (e == std::string::npos) return -1;
                    i = e + 1;
                }
            }
            ++n;
        }
        return n;
    }

    static std::string ret(const std::string& sig) {
        size_t p = sig.find(')');
        return p == std::string::npos ? std::string() : sig.substr(p + 1);
    }

    static bool isStatic(jvmtiEnv* jvmti, jmethodID id) {
        jint mods = 0;
        return jvmti && id && jvmti->GetMethodModifiers(id, &mods) == JVMTI_ERROR_NONE && (mods & JVM_ACC_STATIC) != 0;
    }

    static std::string classSig(jvmtiEnv* jvmti, jclass cls) {
        char* sig = nullptr;
        char* gen = nullptr;
        if (!jvmti || !cls || jvmti->GetClassSignature(cls, &sig, &gen) != JVMTI_ERROR_NONE) return {};
        std::string out = sig ? sig : "";
        if (sig) jvmti->Deallocate(reinterpret_cast<unsigned char*>(sig));
        if (gen) jvmti->Deallocate(reinterpret_cast<unsigned char*>(gen));
        return out;
    }

    static jclass global(jclass cls) {
        return cls ? reinterpret_cast<jclass>(Java::env->NewGlobalRef(cls)) : nullptr;
    }

    static std::vector<MethodMeta> methods(jvmtiEnv* jvmti, jclass cls) {
        std::vector<MethodMeta> out;
        if (!jvmti || !cls) return out;
        jint count = 0;
        jmethodID* ids = nullptr;
        if (jvmti->GetClassMethods(cls, &count, &ids) != JVMTI_ERROR_NONE || !ids) return out;
        out.reserve(count);
        for (jint i = 0; i < count; ++i) {
            char* name = nullptr; char* sig = nullptr; char* gen = nullptr;
            if (jvmti->GetMethodName(ids[i], &name, &sig, &gen) == JVMTI_ERROR_NONE) {
                jint mods = 0;
                jvmti->GetMethodModifiers(ids[i], &mods);
                out.push_back({ids[i], sig ? sig : "", mods});
            }
            if (name) jvmti->Deallocate(reinterpret_cast<unsigned char*>(name));
            if (sig) jvmti->Deallocate(reinterpret_cast<unsigned char*>(sig));
            if (gen) jvmti->Deallocate(reinterpret_cast<unsigned char*>(gen));
        }
        jvmti->Deallocate(reinterpret_cast<unsigned char*>(ids));
        return out;
    }

    static std::vector<FieldMeta> fields(jvmtiEnv* jvmti, jclass cls) {
        std::vector<FieldMeta> out;
        if (!jvmti || !cls) return out;
        jint count = 0;
        jfieldID* ids = nullptr;
        if (jvmti->GetClassFields(cls, &count, &ids) != JVMTI_ERROR_NONE || !ids) return out;
        out.reserve(count);
        for (jint i = 0; i < count; ++i) {
            char* name = nullptr; char* sig = nullptr; char* gen = nullptr;
            if (jvmti->GetFieldName(cls, ids[i], &name, &sig, &gen) == JVMTI_ERROR_NONE) {
                jint mods = 0;
                jvmti->GetFieldModifiers(cls, ids[i], &mods);
                out.push_back({ids[i], sig ? sig : "", mods});
            }
            if (name) jvmti->Deallocate(reinterpret_cast<unsigned char*>(name));
            if (sig) jvmti->Deallocate(reinterpret_cast<unsigned char*>(sig));
            if (gen) jvmti->Deallocate(reinterpret_cast<unsigned char*>(gen));
        }
        jvmti->Deallocate(reinterpret_cast<unsigned char*>(ids));
        return out;
    }

    static jmethodID method(jvmtiEnv* jvmti, jclass cls, int args, const std::string& result, bool wantStatic) {
        for (const auto& m : methods(jvmti, cls)) {
            if (isStatic(jvmti, m.id) != wantStatic) continue;
            if (argc(m.sig) != args) continue;
            if (!result.empty() && ret(m.sig) != result) continue;
            return m.id;
        }
        return nullptr;
    }

    static jfieldID field(jvmtiEnv* jvmti, jclass cls, const std::string& sig) {
        for (const auto& f : fields(jvmti, cls))
            if (!(f.mods & JVM_ACC_STATIC) && f.sig == sig) return f.id;
        return nullptr;
    }

    static bool collections() {
        RuntimeBindings::map_class = reinterpret_cast<jclass>(Java::env->FindClass("java/util/Map"));
        clear();
        RuntimeBindings::collection_class = reinterpret_cast<jclass>(Java::env->FindClass("java/util/Collection"));
        clear();
        RuntimeBindings::iterator_class = reinterpret_cast<jclass>(Java::env->FindClass("java/util/Iterator"));
        clear();
        if (!RuntimeBindings::map_class || !RuntimeBindings::collection_class || !RuntimeBindings::iterator_class) return false;
        RuntimeBindings::map_values = Java::env->GetMethodID(RuntimeBindings::map_class, "values", "()Ljava/util/Collection;"); clear();
        RuntimeBindings::collection_iterator = Java::env->GetMethodID(RuntimeBindings::collection_class, "iterator", "()Ljava/util/Iterator;"); clear();
        RuntimeBindings::iterator_hasNext = Java::env->GetMethodID(RuntimeBindings::iterator_class, "hasNext", "()Z"); clear();
        RuntimeBindings::iterator_next = Java::env->GetMethodID(RuntimeBindings::iterator_class, "next", "()Ljava/lang/Object;"); clear();
        return RuntimeBindings::map_values && RuntimeBindings::collection_iterator && RuntimeBindings::iterator_hasNext && RuntimeBindings::iterator_next;
    }

    static jclass findClientSingleton(jvmtiEnv* jvmti, const std::vector<jclass>& loaded, jmethodID& getter) {
        jclass best = nullptr;
        int bestScore = 0;
        for (jclass cls : loaded) {
            const std::string sig = classSig(jvmti, cls);
            if (sig.empty()) continue;
            jmethodID selfGetter = method(jvmti, cls, 0, sig, true);
            if (!selfGetter) continue;
            int score = 100;
            int objectFields = 0;
            int objectReturns = 0;
            for (const auto& f : fields(jvmti, cls)) if (!(f.mods & JVM_ACC_STATIC) && isObjectSig(f.sig)) ++objectFields;
            for (const auto& m : methods(jvmti, cls)) if (!isStatic(jvmti, m.id) && argc(m.sig) == 0 && isObjectSig(ret(m.sig))) ++objectReturns;
            score += std::min(objectFields, 24) + std::min(objectReturns * 2, 20);
            if (score > bestScore) { bestScore = score; best = cls; getter = selfGetter; }
        }
        return best;
    }

    static void deriveClientFields(jvmtiEnv* jvmti, jobject mc, jclass mcClass, const std::vector<jclass>& loaded) {
        struct Candidate { jfieldID field = nullptr; jclass cls = nullptr; int world = -1; int gui = -1; };
        Candidate world{}, gui{};
        std::unordered_map<std::string, jclass> bySig;
        for (jclass c : loaded) {
            const auto sig = classSig(jvmti, c);
            if (!sig.empty()) bySig.emplace(sig, c);
        }
        for (const auto& f : fields(jvmti, mcClass)) {
            if (f.mods & JVM_ACC_STATIC || !isObjectSig(f.sig)) continue;
            auto it = bySig.find(f.sig);
            if (it == bySig.end()) continue;
            jobject value = Java::env->GetObjectField(mc, f.id);
            if (clear() || !value) continue;
            int ws = 0, gs = 0;
            for (const auto& m : methods(jvmti, it->second)) {
                if (isStatic(jvmti, m.id)) continue;
                if (argc(m.sig) == 0 && isObjectSig(ret(m.sig))) ws += 2;
                if (ret(m.sig) == "Ljava/lang/String;") gs += 3;
            }
            for (const auto& sf : fields(jvmti, it->second)) {
                if (sf.mods & JVM_ACC_STATIC) continue;
                if (sf.sig == "Ljava/lang/String;") gs += 3;
                if (sf.sig.find("Ljava/util/") == 0) ws += 2;
            }
            if (ws > world.world) {
                if (world.cls) Java::env->DeleteGlobalRef(world.cls);
                world = {f.id, global(it->second), ws, 0};
            }
            if (gs > gui.gui) {
                if (gui.cls) Java::env->DeleteGlobalRef(gui.cls);
                gui = {f.id, global(it->second), 0, gs};
            }
            Java::env->DeleteLocalRef(value);
        }
        RuntimeBindings::minecraft_theWorld = world.field;
        RuntimeBindings::worldClass = world.cls;
        RuntimeBindings::minecraft_ingameGUI = gui.field;
        RuntimeBindings::guiIngameClass = gui.cls;
    }

    static jfieldID findChatField(jobject tabOverlay) {
        if (!Java::env || !tabOverlay || !RuntimeBindings::ichatcomponent_class) return nullptr;
        JavaVM* vm = nullptr;
        if (Java::env->GetJavaVM(&vm) != JNI_OK || !vm) return nullptr;
        jvmtiEnv* jvmti = nullptr;
        if (vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK || !jvmti) return nullptr;

        jclass tabClass = Java::env->GetObjectClass(tabOverlay);
        if (clear() || !tabClass) return nullptr;
        const std::string chatSig = classSig(jvmti, RuntimeBindings::ichatcomponent_class);
        jfieldID result = nullptr;
        for (const auto& f : fields(jvmti, tabClass)) {
            if (!(f.mods & JVM_ACC_STATIC) && f.sig == chatSig) {
                result = f.id;
                break;
            }
        }
        Java::env->DeleteLocalRef(tabClass);
        return result;
    }

    static void deriveGraph(jvmtiEnv* jvmti, const std::vector<jclass>& loaded, jobject mc) {
        if (RuntimeBindings::minecraft_theWorld) {
            jobject world = Java::env->GetObjectField(mc, RuntimeBindings::minecraft_theWorld);
            if (!clear() && world) {
                RuntimeBindings::worldClass = global(Java::env->GetObjectClass(world));
                for (const auto& m : methods(jvmti, RuntimeBindings::worldClass)) {
                    if (isStatic(jvmti, m.id) || argc(m.sig) != 0 || !isObjectSig(ret(m.sig))) continue;
                    jobject probe = Java::env->CallObjectMethod(world, m.id);
                    if (clear() || !probe) continue;
                    jclass pc = Java::env->GetObjectClass(probe);
                    if (pc) {
                        int score = 0;
                        for (const auto& pm : methods(jvmti, pc)) {
                            if (argc(pm.sig) == 1 && ret(pm.sig).find("L") == 0) ++score;
                            if (ret(pm.sig) == "Ljava/util/Collection;") score += 2;
                        }
                        if (score >= 2) {
                            RuntimeBindings::world_getScoreboard = m.id;
                            RuntimeBindings::scoreboardClass = global(pc);
                            Java::env->DeleteLocalRef(pc);
                            Java::env->DeleteLocalRef(probe);
                            break;
                        }
                        Java::env->DeleteLocalRef(pc);
                    }
                    Java::env->DeleteLocalRef(probe);
                }
                Java::env->DeleteLocalRef(world);
            }
        }

        if (RuntimeBindings::minecraft_getNetHandler) {
            jobject nh = Java::env->CallObjectMethod(mc, RuntimeBindings::minecraft_getNetHandler);
            if (!clear() && nh) {
                RuntimeBindings::netHandlerClass = global(Java::env->GetObjectClass(nh));
                RuntimeBindings::netHandlerPlayClient_playerInfoList = field(jvmti, RuntimeBindings::netHandlerClass, "Ljava/util/Map;");
                RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap = method(jvmti, RuntimeBindings::netHandlerClass, 0, "Ljava/util/Collection;", false);
                jobject col = nullptr;
                if (RuntimeBindings::netHandlerPlayClient_playerInfoList) {
                    jobject map = Java::env->GetObjectField(nh, RuntimeBindings::netHandlerPlayClient_playerInfoList);
                    if (!clear() && map) col = Java::env->CallObjectMethod(map, RuntimeBindings::map_values);
                    if (map) Java::env->DeleteLocalRef(map);
                }
                if (!col && RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap)
                    col = Java::env->CallObjectMethod(nh, RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap);
                if (!clear() && col) {
                    jobject it = Java::env->CallObjectMethod(col, RuntimeBindings::collection_iterator);
                    if (!clear() && it && Java::env->CallBooleanMethod(it, RuntimeBindings::iterator_hasNext)) {
                        jobject pi = Java::env->CallObjectMethod(it, RuntimeBindings::iterator_next);
                        if (!clear() && pi) {
                            RuntimeBindings::networkInfoClass = global(Java::env->GetObjectClass(pi));
                            RuntimeBindings::networkPlayerInfo_getResponseTime = method(jvmti, RuntimeBindings::networkInfoClass, 0, "I", false);
                            for (const auto& m : methods(jvmti, RuntimeBindings::networkInfoClass)) {
                                if (isStatic(jvmti, m.id) || argc(m.sig) != 0 || !isObjectSig(ret(m.sig))) continue;
                                auto loadedIt = std::find_if(loaded.begin(), loaded.end(), [&](jclass c){ return classSig(jvmti, c) == ret(m.sig); });
                                if (loadedIt == loaded.end()) continue;
                                jclass rc = *loadedIt;
                                int stringReturns = 0;
                                int stringArgMethods = 0;
                                for (const auto& rm : methods(jvmti, rc)) {
                                    if (ret(rm.sig) == "Ljava/lang/String;") ++stringReturns;
                                    if (argc(rm.sig) == 1 && rm.sig.find("Ljava/lang/String;") != std::string::npos) ++stringArgMethods;
                                }
                                if (!RuntimeBindings::networkPlayerInfo_getPlayerTeam && stringArgMethods > 0) {
                                    RuntimeBindings::networkPlayerInfo_getPlayerTeam = m.id;
                                    RuntimeBindings::scorePlayerTeam_class = global(rc);
                                }
                                if (!RuntimeBindings::networkPlayerInfo_getDisplayName && stringReturns > 0) {
                                    RuntimeBindings::networkPlayerInfo_getDisplayName = m.id;
                                    RuntimeBindings::ichatcomponent_class = global(rc);
                                }
                            }
                            RuntimeBindings::networkPlayerInfo_getGameProfile = method(jvmti, RuntimeBindings::networkInfoClass, 0, {}, false);
                            RuntimeBindings::networkPlayerInfo_setDisplayName = method(jvmti, RuntimeBindings::networkInfoClass, 1, {}, false);
                            Java::env->DeleteLocalRef(pi);
                        }
                    }
                    if (it) Java::env->DeleteLocalRef(it);
                    Java::env->DeleteLocalRef(col);
                }
                Java::env->DeleteLocalRef(nh);
            }
        }

        if (RuntimeBindings::ichatcomponent_class) {
            RuntimeBindings::ichatcomponent_getFormattedText = method(jvmti, RuntimeBindings::ichatcomponent_class, 0, "Ljava/lang/String;", false);
            RuntimeBindings::chatComponent_createCopy = method(jvmti, RuntimeBindings::ichatcomponent_class, 0, {}, false);
            RuntimeBindings::chatComponent_appendSibling = method(jvmti, RuntimeBindings::ichatcomponent_class, 1, {}, false);
            RuntimeBindings::chatComponentText_class = global(RuntimeBindings::ichatcomponent_class);
        }
        if (RuntimeBindings::scorePlayerTeam_class) {
            RuntimeBindings::scorePlayerTeam_getColorPrefix = method(jvmti, RuntimeBindings::scorePlayerTeam_class, 0, "Ljava/lang/String;", false);
            RuntimeBindings::scorePlayerTeam_getColorSuffix = RuntimeBindings::scorePlayerTeam_getColorPrefix;
            RuntimeBindings::scorePlayerTeam_formatPlayerName = method(jvmti, RuntimeBindings::scorePlayerTeam_class, 1, "Ljava/lang/String;", true);
        }
        if (RuntimeBindings::scoreboardClass) {
            RuntimeBindings::scoreboard_getObjectiveInDisplaySlot = method(jvmti, RuntimeBindings::scoreboardClass, 1, {}, false);
            RuntimeBindings::scoreboard_getSortedScores = method(jvmti, RuntimeBindings::scoreboardClass, 1, "Ljava/util/Collection;", false);
            RuntimeBindings::scoreboard_getPlayersTeam = method(jvmti, RuntimeBindings::scoreboardClass, 1, {}, false);
        }
        if (RuntimeBindings::worldClass && !RuntimeBindings::world_getScoreboard)
            RuntimeBindings::world_getScoreboard = method(jvmti, RuntimeBindings::worldClass, 0, {}, false);
        if (RuntimeBindings::guiIngameClass) {
            RuntimeBindings::guiIngame_overlayPlayerList = field(jvmti, RuntimeBindings::guiIngameClass, "Ljava/lang/Object;");
            RuntimeBindings::guiIngame_getTabList = method(jvmti, RuntimeBindings::guiIngameClass, 0, {}, false);
        }
    }
}

bool RuntimeBindings::init() {
    std::lock_guard<std::mutex> lock(mutex);
    if (initialized) return minecraft_class != nullptr;
    initialized = true;
    if (!Java::env) return false;
    JavaVM* vm = nullptr;
    if (Java::env->GetJavaVM(&vm) != JNI_OK || !vm) return false;
    jvmtiEnv* jvmti = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK || !jvmti) return false;
    if (!runtime_resolver::collections()) return false;

    jint count = 0;
    jclass* raw = nullptr;
    if (jvmti->GetLoadedClasses(&count, &raw) != JVMTI_ERROR_NONE || !raw) return false;
    std::vector<jclass> loaded(raw, raw + count);
    jmethodID getter = nullptr;
    jclass mc = runtime_resolver::findClientSingleton(jvmti, loaded, getter);
    if (!mc || !getter) {
        jvmti->Deallocate(reinterpret_cast<unsigned char*>(raw));
        return false;
    }
    minecraft_class = runtime_resolver::global(mc);
    minecraft_getMinecraft = getter;
    jobject instance = Java::env->CallStaticObjectMethod(minecraft_class, minecraft_getMinecraft);
    if (runtime_resolver::clear() || !instance) {
        jvmti->Deallocate(reinterpret_cast<unsigned char*>(raw));
        return false;
    }
    runtime_resolver::deriveClientFields(jvmti, instance, mc, loaded);

    const std::string mcSig = runtime_resolver::classSig(jvmti, mc);
    for (const auto& m : runtime_resolver::methods(jvmti, mc)) {
        if (runtime_resolver::isStatic(jvmti, m.id) || runtime_resolver::argc(m.sig) != 0 || !runtime_resolver::isObjectSig(runtime_resolver::ret(m.sig))) continue;
        auto it = std::find_if(loaded.begin(), loaded.end(), [&](jclass c){ return runtime_resolver::classSig(jvmti, c) == runtime_resolver::ret(m.sig); });
        if (it == loaded.end()) continue;
        jclass c = *it;
        int mapFields = 0;
        int collectionReturns = 0;
        for (const auto& f : runtime_resolver::fields(jvmti, c)) if (f.sig.find("Ljava/util/Map;") == 0) ++mapFields;
        for (const auto& cm : runtime_resolver::methods(jvmti, c)) if (runtime_resolver::ret(cm.sig) == "Ljava/util/Collection;") ++collectionReturns;
        if (mapFields || collectionReturns >= 1) { minecraft_getNetHandler = m.id; netHandlerClass = runtime_resolver::global(c); break; }
    }

    runtime_resolver::deriveGraph(jvmti, loaded, instance);

    if (networkInfoClass) {
        auto profile = runtime_resolver::method(jvmti, networkInfoClass, 0, {}, false);
        networkPlayerInfo_getGameProfile = profile;
        if (profile) {
            for (const auto& m : runtime_resolver::methods(jvmti, networkInfoClass)) {
                if (runtime_resolver::isStatic(jvmti, m.id) || runtime_resolver::argc(m.sig) != 0) continue;
                if (runtime_resolver::ret(m.sig) == "I") networkPlayerInfo_getResponseTime = m.id;
            }
        }
    }

    jvmti->Deallocate(reinterpret_cast<unsigned char*>(raw));
    Java::env->DeleteLocalRef(instance);
    return minecraft_class && minecraft_getMinecraft && minecraft_getNetHandler;
}

std::map<std::string, TabListStatsData> TabList::nameCache;
std::mutex TabList::cacheMutex;
std::queue<std::string> TabList::fetchQueue;
std::mutex TabList::queueMutex;
std::atomic<bool> TabList::threadRunning{ false };
std::vector<std::thread> TabList::workerThreads;

std::deque<FetchTask> TabList::highPriorityQueue;
std::deque<FetchTask> TabList::lowPriorityQueue;
std::mutex TabList::taskQueueMutex;
std::condition_variable TabList::taskCv;

HINTERNET TabList::hHttpSession = NULL;
HINTERNET TabList::hHttpConnect = NULL;
std::mutex TabList::httpInitMutex;

bool s_injecting = false;
static bool s_tabWatermarkActive = false;
static std::string s_lastServerFooter;
static const std::string s_watermarkText = "\xC2\xA7""d\xC2\xA7""lCutie \xC2\xA7""8\xC2\xBB \xC2\xA7""bdiscord: \xC2\xA7""dlinixie.";

static bool clearJNI() {
    if (!Java::env) return false;
    if (Java::env->ExceptionCheck()) { Java::env->ExceptionClear(); return true; }
    return false;
}

static bool isValidName(const std::string& n) {
    if (n.length() < 3 || n.length() > 16) return false;
    for (char c : n) if (!isalnum((unsigned char)c) && c != '_') return false;
    return true;
}

static int ParseStatFromLB(const json& lb, const std::string& statName) {
    if (!lb.contains(statName)) return 0;
    auto& obj = lb[statName];
    if (!obj.contains("entries") || !obj["entries"].is_array() || obj["entries"].empty()) return 0;
    auto& entry = obj["entries"][0];
    if (!entry.contains("value")) return 0;
    std::string v = entry["value"].get<std::string>();
    v.erase(std::remove(v.begin(), v.end(), ','), v.end());
    try { return std::stoi(v); } catch (...) { return 0; }
}

static std::string ClosestMCColor(float* rgb) {
    float r = rgb[0], g = rgb[1], b = rgb[2];
    struct MC { std::string c; float r, g, b; };
    static const MC c[] = {
        {"\xC2\xA7""0",0,0,0},{"\xC2\xA7""1",0,0,.66f},{"\xC2\xA7""2",0,.66f,0},{"\xC2\xA7""3",0,.66f,.66f},
        {"\xC2\xA7""4",.66f,0,0},{"\xC2\xA7""5",.66f,0,.66f},{"\xC2\xA7""6",1,.66f,0},{"\xC2\xA7""7",.66f,.66f,.66f},
        {"\xC2\xA7""8",.33f,.33f,.33f},{"\xC2\xA7""9",.33f,.33f,1},{"\xC2\xA7""a",.33f,1,.33f},{"\xC2\xA7""b",.33f,1,1},
        {"\xC2\xA7""c",1,.33f,.33f},{"\xC2\xA7""d",1,.33f,1},{"\xC2\xA7""e",1,1,.33f},{"\xC2\xA7""f",1,1,1}
    };
    std::string best = "\xC2\xA7""7"; float md = 9999;
    for (auto& x : c) { float d = (r - x.r)*(r - x.r) + (g - x.g)*(g - x.g) + (b - x.b)*(b - x.b); if (d < md) { md = d; best = x.c; } }
    return best;
}

static bool hasMatchingKeyword(const std::string& clean) {
    static const char* defaults[] = {
        "sale", "shop", "buy", "purchase", "store", "discount", "offer", "deal", "coupon",
        "promo", "promotion", "limited time", "flash sale", "server ip", "server address",
        "connect to", "ip:", "store.", "shop.", "buy.", "purchase.", ".net", ".com", ".org", ".gg", "mc."
    };
    for (const char* kw : defaults) {
        if (clean.find(kw) != std::string::npos) return true;
    }
    for (const auto& kw : settings::AntiSpam_Keywords) {
        std::string kwLower;
        for (char c : kw) kwLower += (char)tolower((unsigned char)c);
        if (!kwLower.empty() && clean.find(kwLower) != std::string::npos) return true;
    }
    return false;
}

static void stripSectionAndLower(const std::string& in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ) {
        if ((unsigned char)in[i] == 0xC2 && i + 1 < in.size() && (unsigned char)in[i+1] == 0xA7) {
            i += 2; continue;
        }
        out += (char)tolower((unsigned char)in[i]);
        i++;
    }
}

std::string TabList::BuildStatSuffix(const TabListStatsData& s) {
    if (!s.isValid) return "";

    // Wait for both requests unless leaderboard data is already ready.
    if (!s.hasValidStats && (!s.leaderboardFetched || !s.profileFetched)) return "";

    // A 404 from the profile endpoint means nicked.
    if (s.profileFetched && !s.profileExists) {
        return " \xC2\xA7""c[NICK]";
    }

    bool isZeroStats = (s.hasValidStats && s.finalKills == 0 && s.kills == 0 && s.wins == 0 && s.bedsBroken == 0);

    // No usable stats or a fresh account.
    if (!s.hasValidStats || isZeroStats) {
        std::string res = " ";
        
        // Level 0/1 is not useful here.
        if (settings::TL_showLevel && s.level > 1) {
            res += ClosestMCColor(settings::TL_Col_Level) + "L" + std::to_string(s.level) + " ";
        }
        
        res += (!s.hasValidStats) ? "\xC2\xA7""c[OFF]" : "\xC2\xA7""7[N/A]";
        return res;
    }

    auto tr = [](float v) {
        if (!settings::TL_UseThresholdColors) return std::string("");
        if (v >= settings::TL_Thresh_Ratio_High) return ClosestMCColor(settings::TL_Col_Ratio_High);
        if (v >= settings::TL_Thresh_Ratio_Med)  return ClosestMCColor(settings::TL_Col_Ratio_Med);
        if (v >= settings::TL_Thresh_Ratio_Low)  return ClosestMCColor(settings::TL_Col_Ratio_Low);
        return ClosestMCColor(settings::TL_Col_Ratio_Def);
    };
    
    auto tw = [](float v) {
        if (!settings::TL_UseThresholdColors) return std::string("");
        if (v >= settings::TL_Thresh_WS_High) return ClosestMCColor(settings::TL_Col_WS_High);
        if (v >= settings::TL_Thresh_WS_Med)  return ClosestMCColor(settings::TL_Col_WS_Med);
        if (v >= settings::TL_Thresh_WS_Low)  return ClosestMCColor(settings::TL_Col_WS_Low);
        return ClosestMCColor(settings::TL_Col_WS_Def);
    };

    if (settings::TL_FormatMode == 0) {
        std::string r;
        auto a = [&](bool sp, std::string st) { if (sp && !r.empty()) r += " "; r += st; };
        
        if (settings::TL_showLevel && s.level > 0) a(true, ClosestMCColor(settings::TL_Col_Level) + "L" + std::to_string(s.level));
        
        if (settings::TL_showFkdr) { char b[24]; snprintf(b, 24, "%.2f", s.fkdr); a(true, ClosestMCColor(settings::TL_Col_Pref_Fkdr) + "F" + tr(s.fkdr) + b); }
        if (settings::TL_showKdr) { char b[24]; snprintf(b, 24, "%.2f", s.kdr); a(true, ClosestMCColor(settings::TL_Col_Pref_Kdr) + "K" + tr(s.kdr) + b); }
        if (settings::TL_showWlr) { char b[24]; snprintf(b, 24, "%.2f", s.wlr); a(true, ClosestMCColor(settings::TL_Col_Pref_Kdr) + "WLR" + tr(s.wlr) + b); }
        if (settings::TL_showWS) { a(true, ClosestMCColor(settings::TL_Col_Pref_WS) + "WS" + tw((float)s.winstreak) + std::to_string(s.winstreak)); }
        if (settings::TL_showWins) a(true, ClosestMCColor(settings::TL_Col_Wins) + "W" + std::to_string(s.wins));
        if (settings::TL_showLosses) a(true, ClosestMCColor(settings::TL_Col_Losses) + "L" + std::to_string(s.losses));
        if (settings::TL_showBeds) a(true, ClosestMCColor(settings::TL_Col_Beds) + "B" + std::to_string(s.bedsBroken));
        if (settings::TL_showGames) a(true, ClosestMCColor(settings::TL_Col_Games) + "G" + std::to_string(s.gamesPlayed));
        if (settings::TL_showFinalKills) a(true, ClosestMCColor(settings::TL_Col_Pref_Fkdr) + "FK" + std::to_string(s.finalKills));
        if (settings::TL_showFinalDeaths) a(true, ClosestMCColor(settings::TL_Col_Losses) + "FD" + std::to_string(s.finalDeaths));
        if (settings::TL_showKills) a(true, ClosestMCColor(settings::TL_Col_Pref_Kdr) + "K" + std::to_string(s.kills));
        if (settings::TL_showDeaths) a(true, ClosestMCColor(settings::TL_Col_Losses) + "D" + std::to_string(s.deaths));
        if (settings::TL_showArrows) a(true, ClosestMCColor(settings::TL_Col_Arrows) + "A" + std::to_string(s.arrowsShot));
        if (settings::TL_showArrowsHit) a(true, ClosestMCColor(settings::TL_Col_Arrows) + "AH" + std::to_string(s.arrowsHit));
        if (settings::TL_showMissedShots) a(true, ClosestMCColor(settings::TL_Col_Losses) + "MS" + std::to_string(s.missedShots));
        if (settings::TL_showBowKills) a(true, ClosestMCColor(settings::TL_Col_Pref_Kdr) + "BK" + std::to_string(s.bowKills));
        if (settings::TL_showVoidKills) a(true, ClosestMCColor(settings::TL_Col_Pref_Kdr) + "VK" + std::to_string(s.voidKills));
        if (settings::TL_showMeleeKills) a(true, ClosestMCColor(settings::TL_Col_Pref_Kdr) + "MK" + std::to_string(s.meleeKills));
        if (settings::TL_showGuild && !s.clanTag.empty()) a(true, ClosestMCColor(settings::TL_Col_Guild) + "[" + s.clanTag + "]");
        return " " + r;
    }
    else {
        std::string res = settings::TL_FormatString;
        auto rp = [](std::string& s, const std::string& f, const std::string& t) { size_t p = 0; while ((p = s.find(f, p)) != std::string::npos) { s.replace(p, f.length(), t); p += t.length(); } };
        char b[24];
        snprintf(b, 24, "%.2f", s.fkdr); rp(res, "{fkdr}", b);
        snprintf(b, 24, "%.2f", s.kdr);  rp(res, "{kdr}", b);
        snprintf(b, 24, "%.2f", s.wlr);  rp(res, "{wlr}", b);
        
        if (s.level > 0) rp(res, "{level}", std::to_string(s.level));
        else rp(res, "{level}", "?");
        
        rp(res, "{winstreak}", std::to_string(s.winstreak));
        rp(res, "{wins}", std::to_string(s.wins)); rp(res, "{losses}", std::to_string(s.losses));
        rp(res, "{games}", std::to_string(s.gamesPlayed)); rp(res, "{beds}", std::to_string(s.bedsBroken));
        rp(res, "{finalKills}", std::to_string(s.finalKills)); rp(res, "{finalDeaths}", std::to_string(s.finalDeaths));
        rp(res, "{kills}", std::to_string(s.kills)); rp(res, "{deaths}", std::to_string(s.deaths));
        rp(res, "{guild}", s.clanTag);
        rp(res, "{clan}", s.clanTag);
        rp(res, "&&", "\xC2\xA7");
        return " " + res;
    }
}

namespace {
static bool CMReadPlayerName(jobject pi, std::string& name);
static void CMClearOriginalDisplayNames();
}

TabList::TabList() {
    threadRunning = true;
    InitHttpPool();
    for (int i = 0; i < 8; ++i) {
        workerThreads.emplace_back(&TabList::WorkerThreadFunc);
    }
}

TabList::~TabList() {
    threadRunning = false;
    taskCv.notify_all();
    for (auto& t : workerThreads) {
        if (t.joinable()) t.join();
    }
    workerThreads.clear();
    CloseHttpPool();
    CMClearOriginalDisplayNames();
}

void TabList::InitHttpPool() {
    std::lock_guard<std::mutex> lock(httpInitMutex);
    if (hHttpConnect != NULL) return;

    hHttpSession = WinHttpOpen(
        L"Cutie/2.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (!hHttpSession) return;

    WinHttpSetTimeouts(hHttpSession, 3000, 3000, 3000, 3000);

    hHttpConnect = WinHttpConnect(
        hHttpSession,
        L"stats.pika-network.net",
        INTERNET_DEFAULT_HTTPS_PORT,
        0
    );
}

void TabList::CloseHttpPool() {
    std::lock_guard<std::mutex> lock(httpInitMutex);
    if (hHttpConnect) {
        WinHttpCloseHandle(hHttpConnect);
        hHttpConnect = NULL;
    }
    if (hHttpSession) {
        WinHttpCloseHandle(hHttpSession);
        hHttpSession = NULL;
    }
}

std::string TabList::WinHttpGetPath(const std::string& path, int& sc) {
    std::string response;
    sc = 0;

    InitHttpPool();

    HINTERNET hConn = hHttpConnect;
    if (!hConn) return response;

    std::wstring wPath(path.begin(), path.end());

    HINTERNET hRequest = WinHttpOpenRequest(
        hConn,
        L"GET",
        wPath.c_str(),
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );

    if (!hRequest) return response;

    const wchar_t* headers = L"Accept: application/json\r\nConnection: keep-alive\r\n";

    if (WinHttpSendRequest(hRequest, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {

        DWORD sz = sizeof(DWORD);
        WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &sc,
            &sz,
            WINHTTP_NO_HEADER_INDEX
        );

        DWORD avail = 0;
        char buffer[4096];
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            DWORD read = 0;
            DWORD toRead = min(avail, (DWORD)sizeof(buffer));
            if (WinHttpReadData(hRequest, buffer, toRead, &read) && read > 0) {
                response.append(buffer, read);
            } else {
                break;
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    return response;
}

std::string TabList::WinHttpGet(const std::string& url, int& sc) {
    size_t pos = url.find("stats.pika-network.net");
    if (pos != std::string::npos) {
        std::string path = url.substr(pos + strlen("stats.pika-network.net"));
        return WinHttpGetPath(path, sc);
    }
    return "";
}

void TabList::PushTask(const std::string& name, FetchTaskType type, int delayMs) {
    if (name.empty() || name.length() > 16) return;
    FetchTask t;
    t.name = name;
    t.type = type;
    t.executeAfter = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);

    std::lock_guard<std::mutex> lock(taskQueueMutex);
    if (type == FetchTaskType::LEADERBOARD) {
        for (const auto& existing : highPriorityQueue) {
            if (existing.name == name) return;
        }
        highPriorityQueue.push_back(t);
    } else {
        for (const auto& existing : lowPriorityQueue) {
            if (existing.name == name) return;
        }
        lowPriorityQueue.push_back(t);
    }
    taskCv.notify_one();
}

bool TabList::PopTask(FetchTask& task) {
    std::unique_lock<std::mutex> lock(taskQueueMutex);
    while (threadRunning) {
        auto now = std::chrono::steady_clock::now();

        for (auto it = highPriorityQueue.begin(); it != highPriorityQueue.end(); ++it) {
            if (it->executeAfter <= now) {
                task = *it;
                highPriorityQueue.erase(it);
                return true;
            }
        }

        for (auto it = lowPriorityQueue.begin(); it != lowPriorityQueue.end(); ++it) {
            if (it->executeAfter <= now) {
                task = *it;
                lowPriorityQueue.erase(it);
                return true;
            }
        }

        if (highPriorityQueue.empty() && lowPriorityQueue.empty()) {
            taskCv.wait(lock);
        } else {
            taskCv.wait_for(lock, std::chrono::milliseconds(25));
        }
    }
    return false;
}

void TabList::ClearTaskQueues() {
    std::lock_guard<std::mutex> lock(taskQueueMutex);
    highPriorityQueue.clear();
    lowPriorityQueue.clear();
}

void TabList::EnqueuePlayer(const std::string& name) {
    PushTask(name, FetchTaskType::LEADERBOARD, 0);
    PushTask(name, FetchTaskType::PROFILE, 0);
}

void TabList::WorkerThreadFunc() {
    while (threadRunning) {
        FetchTask task;
        if (PopTask(task)) {
            if (task.type == FetchTaskType::LEADERBOARD) {
                ExecuteLeaderboardTask(task.name);
            } else {
                ExecuteProfileTask(task.name);
            }
        }
    }
}

void TabList::ExecuteLeaderboardTask(const std::string& name) {
    if (name.empty() || name.length() > 16) return;
    {
        std::lock_guard<std::mutex> l(cacheMutex);
        auto it = nameCache.find(name);
        if (it != nameCache.end() && it->second.leaderboardFetched) return;
    }
    static const char* intervalApiKeys[] = { "lifetime", "monthly", "weekly", "yearly" };
    static const char* modeApiKeys[] = { "SOLO", "DOUBLES", "QUADS", "ALL_MODES" };
    int intervalIdx = settings::TL_Interval;
    int modeIdx = settings::TL_Mode;
    if (intervalIdx < 0 || intervalIdx > 3) intervalIdx = 0;
    if (modeIdx < 0 || modeIdx > 3) modeIdx = 3;

    std::string path = "/api/profile/" + name + "/leaderboard?type=bedwars&interval=" + intervalApiKeys[intervalIdx] + "&mode=" + modeApiKeys[modeIdx];
    int code = 0;
    std::string jsonStr = WinHttpGetPath(path, code);

    if (code == 429) {
        PushTask(name, FetchTaskType::LEADERBOARD, 1200);
        return;
    }

    TabListStatsData s;
    s.isValid = true;
    s.fetchTime = std::chrono::system_clock::now();
    s.leaderboardFetched = true;

    if (code == 200 && !jsonStr.empty()) {
        try {
            json lb = json::parse(jsonStr);
            bool hasAnyField = lb.contains("Final kills") || lb.contains("Kills") || lb.contains("Wins") || lb.contains("Beds destroyed") || lb.contains("Games played");
            if (!lb.empty() && !lb.contains("error") && hasAnyField) {
                s.hasValidStats = true;
                s.finalKills = ParseStatFromLB(lb, "Final kills");
                s.finalDeaths = ParseStatFromLB(lb, "Final deaths");
                s.kills = ParseStatFromLB(lb, "Kills");
                s.deaths = ParseStatFromLB(lb, "Deaths");
                s.wins = ParseStatFromLB(lb, "Wins");
                s.losses = ParseStatFromLB(lb, "Losses");
                s.bedsBroken = ParseStatFromLB(lb, "Beds destroyed");
                s.gamesPlayed = ParseStatFromLB(lb, "Games played");
                s.winstreak = ParseStatFromLB(lb, "Highest winstreak reached");
                s.arrowsShot = ParseStatFromLB(lb, "Arrows shot");
                s.arrowsHit = ParseStatFromLB(lb, "Arrows hit");
                s.bowKills = ParseStatFromLB(lb, "Bow kills");
                s.voidKills = ParseStatFromLB(lb, "Void kills");
                s.meleeKills = ParseStatFromLB(lb, "Melee kills");
                s.missedShots = s.arrowsShot - s.arrowsHit;
                if (s.missedShots < 0) s.missedShots = 0;
                s.fkdr = s.finalDeaths > 0 ? (float)s.finalKills / s.finalDeaths : (float)s.finalKills;
                s.kdr = s.deaths > 0 ? (float)s.kills / s.deaths : (float)s.kills;
                s.wlr = s.losses > 0 ? (float)s.wins / s.losses : (float)s.wins;
            } else {
                s.hasValidStats = false;
            }
        } catch (...) {
            s.hasValidStats = false;
        }
    } else {
        // Stats unavailable.
        s.hasValidStats = false;
    }

    {
        std::lock_guard<std::mutex> l(cacheMutex);
        auto it = nameCache.find(name);
        if (it != nameCache.end()) {
            s.level = it->second.level;
            s.clanTag = it->second.clanTag;
            s.profileFetched = it->second.profileFetched;
            s.profileExists = it->second.profileExists;
            s.fetchTime = it->second.fetchTime;
        }
        nameCache[name] = s;
    }
}

void TabList::ExecuteProfileTask(const std::string& name) {
    if (name.empty() || name.length() > 16) return;
    {
        std::lock_guard<std::mutex> l(cacheMutex);
        auto it = nameCache.find(name);
        if (it != nameCache.end() && it->second.profileFetched) return;
    }

    std::string path = "/api/profile/" + name;
    int code = 0;
    std::string jsonStr = WinHttpGetPath(path, code);

    if (code == 429) {
        PushTask(name, FetchTaskType::PROFILE, 1200);
        return;
    }

    bool pExists = false;
    int level = 0;
    std::string clan;
    std::string resolvedName = name;

    if (code == 200 && !jsonStr.empty()) {
        pExists = true;
        try {
            json p = json::parse(jsonStr);
            if (p.contains("username") && p["username"].is_string()) resolvedName = p["username"].get<std::string>();
            if (p.contains("rank") && p["rank"].is_object() && p["rank"].contains("level")) level = p["rank"]["level"].get<int>();
            if (p.contains("clan") && p["clan"].is_object()) {
                if (p["clan"].contains("tag") && p["clan"]["tag"].is_string() && !p["clan"]["tag"].get<std::string>().empty()) clan = p["clan"]["tag"].get<std::string>();
                else if (p["clan"].contains("name") && p["clan"]["name"].is_string()) clan = p["clan"]["name"].get<std::string>();
            }
        } catch (...) {}
    } else if (code == 404) {
        pExists = false;
    } else {
        pExists = false; 
    }

    {
        std::lock_guard<std::mutex> l(cacheMutex);
        auto it = nameCache.find(name);
        if (it != nameCache.end()) {
            it->second.level = level;
            it->second.clanTag = clan;
            it->second.profileExists = pExists;
            it->second.profileFetched = true;
        } else {
            TabListStatsData s;
            s.isValid = true;
            s.fetchTime = std::chrono::system_clock::now();
            s.level = level;
            s.clanTag = clan;
            s.profileExists = pExists;
            s.profileFetched = true;
            nameCache[name] = s;
        }
        if (resolvedName != name) {
            auto it2 = nameCache.find(resolvedName);
            if (it2 != nameCache.end()) {
                it2->second.level = level;
                it2->second.clanTag = clan;
                it2->second.profileExists = pExists;
                it2->second.profileFetched = true;
            }
        }
    }
}

void TabList::FetchLeaderboardStats(const std::string& name) {
    ExecuteLeaderboardTask(name);
}

void TabList::FetchProfileStats(const std::string& name) {
    ExecuteProfileTask(name);
}

void TabList::FetchPlayerStats(const std::string& name) {
    EnqueuePlayer(name);
}

namespace {

static void suppressTitleSpam();
static std::string CMString(jstring value);
static jobject CMNewChatComponent(const std::string& text);
static jobject CMGetMinecraft();
static jobject CMGetTabOverlay(jobject mc);

struct CM189 {
    bool initialized = false;
    bool valid = false;

    jclass minecraftClass = nullptr;
    jmethodID minecraftGetMinecraft = nullptr;
    jfieldID minecraftIngameGUI = nullptr;
    jfieldID minecraftWorld = nullptr;
    jmethodID minecraftGetNetHandler = nullptr;

    jclass guiIngameClass = nullptr;
    jfieldID guiOverlayPlayerList = nullptr;
    
    std::vector<jfieldID> guiStringFields;
    jmethodID guiDisplayTitleMethod = nullptr;
    jfieldID guiTitlesTimer = nullptr;

    jclass tabClass = nullptr;
    jfieldID tabFooter = nullptr;
    jfieldID tabHeader = nullptr;
    jmethodID tabGetPlayerName = nullptr;
    jmethodID tabSetFooter = nullptr;
    jmethodID tabSetHeader = nullptr;
    jmethodID tabUpdatePlayerList = nullptr;

    jclass netHandlerClass = nullptr;
    jfieldID playerInfoMap = nullptr;
    jmethodID getPlayerInfoMap = nullptr;

    jclass networkInfoClass = nullptr;
    jmethodID networkGetGameProfile = nullptr;
    jmethodID networkGetDisplayName = nullptr;
    jmethodID networkSetDisplayName = nullptr;
    jmethodID networkGetResponseTime = nullptr;
    jmethodID networkGetPlayerTeam = nullptr;

    jclass gameProfileClass = nullptr;
    jmethodID gameProfileGetName = nullptr;

    jclass worldClass = nullptr;
    jmethodID worldGetScoreboard = nullptr;

    jclass scoreboardClass = nullptr;
    jmethodID scoreboardGetObjectiveInDisplaySlot = nullptr;
    jmethodID scoreboardGetSortedScores = nullptr;

    jclass scoreObjectiveClass = nullptr;
    jmethodID scoreObjectiveGetDisplayName = nullptr;

    jclass scoreClass = nullptr;
    jmethodID scoreGetPlayerName = nullptr;

    jclass scorePlayerTeamClass = nullptr;
    jmethodID scorePlayerTeamFormatPlayerName = nullptr;

    jclass chatComponentClass = nullptr;
    jmethodID chatGetFormattedText = nullptr;
    jmethodID chatAppendSibling = nullptr;
    jmethodID chatCreateCopy = nullptr;

    jclass chatComponentTextClass = nullptr;
    jmethodID chatComponentTextCtor = nullptr;

    jclass mapClass = nullptr;
    jmethodID mapValues = nullptr;

    jclass collectionClass = nullptr;
    jmethodID collectionIterator = nullptr;

    jclass iteratorClass = nullptr;
    jmethodID iteratorHasNext = nullptr;
    jmethodID iteratorNext = nullptr;

    bool initMethod(jclass cls, jmethodID& out, const char* name, const char* sig) {
        if (!cls || !Java::env) return false;
        out = Java::env->GetMethodID(cls, name, sig);
        if (Java::env->ExceptionCheck()) Java::env->ExceptionClear();
        return out != nullptr;
    }

    bool initStaticMethod(jclass cls, jmethodID& out, const char* name, const char* sig) {
        if (!cls || !Java::env) return false;
        out = Java::env->GetStaticMethodID(cls, name, sig);
        if (Java::env->ExceptionCheck()) Java::env->ExceptionClear();
        return out != nullptr;
    }

    bool initField(jclass cls, jfieldID& out, const char* name, const char* sig) {
        if (!cls || !Java::env) return false;
        out = Java::env->GetFieldID(cls, name, sig);
        if (Java::env->ExceptionCheck()) Java::env->ExceptionClear();
        return out != nullptr;
    }

    jclass findGlobalClass(const char* name) {
        if (!Java::env || !name) return nullptr;
        jclass local = Java::env->FindClass(name);
        if (!local || Java::env->ExceptionCheck()) {
            Java::env->ExceptionClear();
            if (local) Java::env->DeleteLocalRef(local);
            return nullptr;
        }
        jclass global = reinterpret_cast<jclass>(Java::env->NewGlobalRef(local));
        Java::env->DeleteLocalRef(local);
        return global;
    }

    bool init() {
        if (initialized) return valid;
        initialized = true;
        if (!Java::env) return false;

        RuntimeBindings::init();

        minecraftClass = RuntimeBindings::minecraft_class;
        minecraftGetMinecraft = RuntimeBindings::minecraft_getMinecraft;
        minecraftIngameGUI = RuntimeBindings::minecraft_ingameGUI;
        minecraftWorld = RuntimeBindings::minecraft_theWorld;
        minecraftGetNetHandler = RuntimeBindings::minecraft_getNetHandler;

        guiIngameClass = RuntimeBindings::guiIngameClass;
        guiOverlayPlayerList = RuntimeBindings::guiIngame_overlayPlayerList;

        netHandlerClass = RuntimeBindings::netHandlerClass;
        playerInfoMap = RuntimeBindings::netHandlerPlayClient_playerInfoList;
        getPlayerInfoMap = RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap;

        networkInfoClass = RuntimeBindings::networkInfoClass;
        networkGetGameProfile = RuntimeBindings::networkPlayerInfo_getGameProfile;
        networkGetDisplayName = RuntimeBindings::networkPlayerInfo_getDisplayName;
        networkSetDisplayName = RuntimeBindings::networkPlayerInfo_setDisplayName;
        networkGetResponseTime = RuntimeBindings::networkPlayerInfo_getResponseTime;
        networkGetPlayerTeam = RuntimeBindings::networkPlayerInfo_getPlayerTeam;

        worldClass = RuntimeBindings::worldClass;
        worldGetScoreboard = RuntimeBindings::world_getScoreboard;
        scoreboardClass = RuntimeBindings::scoreboardClass;
        scoreboardGetObjectiveInDisplaySlot = RuntimeBindings::scoreboard_getObjectiveInDisplaySlot;
        scoreboardGetSortedScores = RuntimeBindings::scoreboard_getSortedScores;
        scorePlayerTeamClass = RuntimeBindings::scorePlayerTeam_class;
        scorePlayerTeamFormatPlayerName = RuntimeBindings::scorePlayerTeam_formatPlayerName;

        chatComponentClass = RuntimeBindings::ichatcomponent_class;
        chatGetFormattedText = RuntimeBindings::ichatcomponent_getFormattedText;
        chatAppendSibling = RuntimeBindings::chatComponent_appendSibling;
        chatCreateCopy = RuntimeBindings::chatComponent_createCopy;
        chatComponentTextClass = RuntimeBindings::chatComponentText_class;

        mapClass = RuntimeBindings::map_class;
        mapValues = RuntimeBindings::map_values;
        collectionClass = RuntimeBindings::collection_class;
        collectionIterator = RuntimeBindings::collection_iterator;
        iteratorClass = RuntimeBindings::iterator_class;
        iteratorHasNext = RuntimeBindings::iterator_hasNext;
        iteratorNext = RuntimeBindings::iterator_next;

        if (guiIngameClass) {
            Java::env->GetObjectClass(nullptr);
            Java::env->ExceptionClear();
        }

        valid = minecraftClass && minecraftGetMinecraft && minecraftGetNetHandler &&
                networkInfoClass && worldClass && scoreboardClass &&
                chatComponentClass && mapValues && collectionIterator &&
                iteratorHasNext && iteratorNext;
        return valid;
    }
};

static CM189 g_cm189;

static void suppressTitleSpam() {
    if (!settings::AntiSpam_Enabled || !Java::env) return;

    if (RuntimeBindings::minecraft_class &&
        RuntimeBindings::minecraft_getMinecraft &&
        RuntimeBindings::minecraft_ingameGUI) {
        jobject mc = Java::env->CallStaticObjectMethod(
            RuntimeBindings::minecraft_class,
            RuntimeBindings::minecraft_getMinecraft
        );
        if (clearJNI() || !mc) return;

        jobject guiObj = Java::env->GetObjectField(
            mc, RuntimeBindings::minecraft_ingameGUI
        );
        if (clearJNI() || !guiObj) {
            Java::env->DeleteLocalRef(mc);
            return;
        }

        jclass guiClass = Java::env->GetObjectClass(guiObj);
        static jfieldID displayedTitleFid = nullptr;
        static jfieldID displayedSubTitleFid = nullptr;
        static jfieldID titleDisplayTicksFid = nullptr;
        static bool searchedFields = false;

        if (!searchedFields) {
            searchedFields = true;

        }

        std::string titleClean, subClean;
        bool hasTitle = false, hasSub = false;

        if (displayedTitleFid) {
            jstring title = (jstring)Java::env->GetObjectField(guiObj, displayedTitleFid);
            if (!clearJNI() && title) {
                const char* tc = Java::env->GetStringUTFChars(title, nullptr);
                if (tc) { std::string ts(tc); Java::env->ReleaseStringUTFChars(title, tc); stripSectionAndLower(ts, titleClean); hasTitle = true; }
                Java::env->DeleteLocalRef(title);
            }
        }
        if (displayedSubTitleFid) {
            jstring sub = (jstring)Java::env->GetObjectField(guiObj, displayedSubTitleFid);
            if (!clearJNI() && sub) {
                const char* sc = Java::env->GetStringUTFChars(sub, nullptr);
                if (sc) { std::string ss(sc); Java::env->ReleaseStringUTFChars(sub, sc); stripSectionAndLower(ss, subClean); hasSub = true; }
                Java::env->DeleteLocalRef(sub);
            }
        }

        if (hasTitle || hasSub) {
            bool clearTitle = hasTitle && hasMatchingKeyword(titleClean);
            bool clearSub = hasSub && hasMatchingKeyword(subClean);
            if (clearTitle || clearSub) {
                jstring emptyStr = Java::env->NewStringUTF("");
                if (emptyStr) {
                    if (clearTitle && displayedTitleFid) Java::env->SetObjectField(guiObj, displayedTitleFid, emptyStr);
                    if (clearSub && displayedSubTitleFid) Java::env->SetObjectField(guiObj, displayedSubTitleFid, emptyStr);
                    Java::env->DeleteLocalRef(emptyStr);
                }
                if (titleDisplayTicksFid) Java::env->SetIntField(guiObj, titleDisplayTicksFid, 0);
                clearJNI();
            }
        }

        Java::env->DeleteLocalRef(guiClass);
        Java::env->DeleteLocalRef(guiObj);
        Java::env->DeleteLocalRef(mc);
        return;
    }

    if (!g_cm189.initialized)
        g_cm189.init();

    if (!g_cm189.minecraftClass || !g_cm189.minecraftGetMinecraft ||
        !g_cm189.minecraftIngameGUI || !g_cm189.guiIngameClass)
        return;

    jobject mc = Java::env->CallStaticObjectMethod(
        g_cm189.minecraftClass, g_cm189.minecraftGetMinecraft
    );
    if (clearJNI() || !mc) return;

    jobject guiObj = Java::env->GetObjectField(mc, g_cm189.minecraftIngameGUI);
    if (clearJNI() || !guiObj) {
        Java::env->DeleteLocalRef(mc);
        return;
    }

    bool foundSpam = false;

    for (jfieldID fid : g_cm189.guiStringFields) {
        jstring val = (jstring)Java::env->GetObjectField(guiObj, fid);
        if (!clearJNI() && val) {
            const char* tc = Java::env->GetStringUTFChars(val, nullptr);
            if (tc) { 
                std::string ts(tc); 
                Java::env->ReleaseStringUTFChars(val, tc); 
                std::string clean; 
                stripSectionAndLower(ts, clean); 
                
                if (hasMatchingKeyword(clean)) {
                    foundSpam = true;
                } 
            }
            Java::env->DeleteLocalRef(val);
        }
        if (foundSpam) break;
    }

    if (foundSpam) {
        jstring emptyStr = Java::env->NewStringUTF("");
        if (emptyStr) {
            if (g_cm189.guiDisplayTitleMethod) {
                Java::env->CallVoidMethod(guiObj, g_cm189.guiDisplayTitleMethod, emptyStr, emptyStr, 0, 0, 0);
                clearJNI();
            }
            
            for (jfieldID fid : g_cm189.guiStringFields) {
                Java::env->SetObjectField(guiObj, fid, emptyStr);
                clearJNI();
            }
            
            Java::env->DeleteLocalRef(emptyStr);
        }
        if (g_cm189.guiTitlesTimer) {
            Java::env->SetIntField(guiObj, g_cm189.guiTitlesTimer, 0);
            clearJNI();
        }
    }

    Java::env->DeleteLocalRef(guiObj);
    Java::env->DeleteLocalRef(mc);
}

static std::unordered_map<std::string, jobject> g_cmOriginalDisplayNames;

static void CMClearOriginalDisplayNames() {
    if (!Java::env) {
        g_cmOriginalDisplayNames.clear();
        return;
    }

    for (auto& entry : g_cmOriginalDisplayNames) {
        if (entry.second)
            Java::env->DeleteGlobalRef(entry.second);
    }

    g_cmOriginalDisplayNames.clear();
}

static jobject CMCreateCopy(jobject component) {
    if (!component || !g_cm189.chatCreateCopy)
        return nullptr;

    jobject copy = Java::env->CallObjectMethod(
        component,
        g_cm189.chatCreateCopy
    );

    if (clearJNI())
        return nullptr;

    return copy;
}

static jobject CMGetCurrentDisplay(jobject pi, std::string* formatted) {
    if (formatted)
        formatted->clear();

    if (!pi || !g_cm189.networkGetDisplayName)
        return nullptr;

    jobject display = Java::env->CallObjectMethod(
        pi,
        g_cm189.networkGetDisplayName
    );

    if (clearJNI() || !display)
        return nullptr;

    if (formatted && g_cm189.chatGetFormattedText) {
        jstring value = reinterpret_cast<jstring>(
            Java::env->CallObjectMethod(
                display,
                g_cm189.chatGetFormattedText
            )
        );

        if (!clearJNI() && value)
            *formatted = CMString(value);

        if (value)
            Java::env->DeleteLocalRef(value);
    }

    return display;
}

static std::string CMGetVanillaRenderedTabName(jobject pi) {
    if (!pi ||
        !g_cm189.tabClass ||
        !g_cm189.tabGetPlayerName)
        return {};

    jobject mc = CMGetMinecraft();
    if (!mc)
        return {};

    jobject tab = CMGetTabOverlay(mc);

    if (!tab) {
        Java::env->DeleteLocalRef(mc);
        return {};
    }

    jstring rendered =
        reinterpret_cast<jstring>(
            Java::env->CallObjectMethod(
                tab,
                g_cm189.tabGetPlayerName,
                pi
            )
        );

    std::string result;

    if (!clearJNI() && rendered) {
        result = CMString(rendered);
        Java::env->DeleteLocalRef(rendered);
    } else {
        clearJNI();
    }

    Java::env->DeleteLocalRef(tab);
    Java::env->DeleteLocalRef(mc);

    return result;
}

static jobject CMBuildOriginalFromTabOverlay(
    jobject pi,
    const std::string& rawName
) {
    std::string rendered =
        CMGetVanillaRenderedTabName(pi);

    if (rendered.empty())
        rendered = rawName;

    if (rendered.empty())
        return nullptr;

    return CMNewChatComponent(rendered);
}

static jobject CMBuildOriginalFromTeam(
    jobject pi,
    const std::string& rawName
) {
    if (!pi ||
        !g_cm189.networkGetPlayerTeam ||
        !g_cm189.scorePlayerTeamClass ||
        !g_cm189.scorePlayerTeamFormatPlayerName)
        return nullptr;

    jobject team = Java::env->CallObjectMethod(
        pi,
        g_cm189.networkGetPlayerTeam
    );

    if (clearJNI())
        return nullptr;

    if (!team)
        return CMNewChatComponent(rawName);

    jstring raw = Java::env->NewStringUTF(rawName.c_str());
    if (!raw) {
        Java::env->DeleteLocalRef(team);
        return nullptr;
    }

    jstring formatted =
        reinterpret_cast<jstring>(
            Java::env->CallStaticObjectMethod(
                g_cm189.scorePlayerTeamClass,
                g_cm189.scorePlayerTeamFormatPlayerName,
                team,
                raw
            )
        );

    Java::env->DeleteLocalRef(raw);
    Java::env->DeleteLocalRef(team);

    if (clearJNI() || !formatted)
        return nullptr;

    const char* chars =
        Java::env->GetStringUTFChars(formatted, nullptr);

    jobject component = nullptr;

    if (chars) {
        component = CMNewChatComponent(chars);
        Java::env->ReleaseStringUTFChars(
            formatted,
            chars
        );
    }

    Java::env->DeleteLocalRef(formatted);
    clearJNI();

    return component;
}

static jobject CMSaveOriginalOrRecover(
    jobject pi,
    const std::string& rawName,
    jobject current,
    const std::string& currentFormatted
) {
    const std::string marker = " \xC2\xA7r\xC2\xA7r ";

    jobject rebuilt = CMBuildOriginalFromTeam(pi, rawName);
    if (rebuilt)
        return rebuilt;

    rebuilt = CMBuildOriginalFromTabOverlay(pi, rawName);
    if (rebuilt)
        return rebuilt;

    if (current && currentFormatted.find(marker) == std::string::npos) {
        jobject copy = CMCreateCopy(current);
        if (copy)
            return copy;
    }

    return nullptr;
}

static bool IsCMClient() {
    if (!Java::env)
        return false;
        
    if (RuntimeBindings::minecraft_class != nullptr && RuntimeBindings::minecraft_getMinecraft != nullptr) {
        return false;
    }

    if (!g_cm189.initialized)
        g_cm189.init();

    return g_cm189.minecraftClass &&
           g_cm189.minecraftGetMinecraft &&
           g_cm189.minecraftGetNetHandler;
}

static std::string CMString(jstring value) {
    if (!value || !Java::env)
        return {};

    const char* chars = Java::env->GetStringUTFChars(value, nullptr);
    if (!chars) {
        clearJNI();
        return {};
    }

    std::string result(chars);
    Java::env->ReleaseStringUTFChars(value, chars);
    return result;
}

static jobject CMGetMinecraft() {
    if (!IsCMClient())
        return nullptr;

    jobject mc = Java::env->CallStaticObjectMethod(
        g_cm189.minecraftClass,
        g_cm189.minecraftGetMinecraft
    );

    if (clearJNI())
        return nullptr;

    return mc;
}

static jobject CMGetPlayerInfoCollection(jobject netHandler) {
    if (!netHandler)
        return nullptr;

    if (g_cm189.playerInfoMap && g_cm189.mapValues) {
        jobject map = Java::env->GetObjectField(
            netHandler,
            g_cm189.playerInfoMap
        );

        if (!clearJNI() && map) {
            jobject collection = Java::env->CallObjectMethod(
                map,
                g_cm189.mapValues
            );
            Java::env->DeleteLocalRef(map);

            if (!clearJNI() && collection)
                return collection;

            clearJNI();
        }
    }

    if (g_cm189.getPlayerInfoMap) {
        jobject collection = Java::env->CallObjectMethod(
            netHandler,
            g_cm189.getPlayerInfoMap
        );

        if (!clearJNI() && collection)
            return collection;

        clearJNI();
    }

    return nullptr;
}

static jfieldID CMGetFooterField(jobject tabOverlay) {
    if (!tabOverlay || !g_cm189.tabClass)
        return nullptr;

    jobject footer = nullptr;
    jobject header = nullptr;

    if (g_cm189.tabFooter)
        footer = Java::env->GetObjectField(tabOverlay, g_cm189.tabFooter);
    clearJNI();

    if (g_cm189.tabHeader)
        header = Java::env->GetObjectField(tabOverlay, g_cm189.tabHeader);
    clearJNI();

    if (footer && !header) {
        Java::env->DeleteLocalRef(footer);
        return g_cm189.tabFooter;
    }

    if (header && !footer) {
        Java::env->DeleteLocalRef(header);
        return g_cm189.tabHeader;
    }

    if (footer)
        Java::env->DeleteLocalRef(footer);

    if (header)
        Java::env->DeleteLocalRef(header);

    return g_cm189.tabFooter;
}

static jobject CMGetTabOverlay(jobject mc) {
    if (!mc || !g_cm189.minecraftIngameGUI || !g_cm189.guiOverlayPlayerList)
        return nullptr;

    jobject gui = Java::env->GetObjectField(
        mc,
        g_cm189.minecraftIngameGUI
    );

    if (clearJNI() || !gui)
        return nullptr;

    jobject tab = Java::env->GetObjectField(
        gui,
        g_cm189.guiOverlayPlayerList
    );

    Java::env->DeleteLocalRef(gui);
    clearJNI();

    return tab;
}

static jobject CMNewChatComponent(const std::string& text) {
    if (!g_cm189.chatComponentTextClass ||
        !g_cm189.chatComponentTextCtor)
        return nullptr;

    jstring value = Java::env->NewStringUTF(text.c_str());
    if (!value)
        return nullptr;

    jobject component = Java::env->NewObject(
        g_cm189.chatComponentTextClass,
        g_cm189.chatComponentTextCtor,
        value
    );

    Java::env->DeleteLocalRef(value);

    if (clearJNI())
        return nullptr;

    return component;
}

static void CMInjectTabWatermarkFooter() {
    if (!settings::TL_Enabled)
        return;

    jobject mc = CMGetMinecraft();
    if (!mc)
        return;

    jobject tab = CMGetTabOverlay(mc);
    if (!tab) {
        Java::env->DeleteLocalRef(mc);
        return;
    }

    jfieldID footerField = CMGetFooterField(tab);
    if (!footerField) {
        Java::env->DeleteLocalRef(tab);
        Java::env->DeleteLocalRef(mc);
        return;
    }

    jobject current = Java::env->GetObjectField(tab, footerField);
    clearJNI();

    std::string currentText;

    if (current && g_cm189.chatGetFormattedText) {
        jstring text = reinterpret_cast<jstring>(
            Java::env->CallObjectMethod(
                current,
                g_cm189.chatGetFormattedText
            )
        );

        if (!clearJNI() && text)
            currentText = CMString(text);

        if (text)
            Java::env->DeleteLocalRef(text);
    }

    if (current)
        Java::env->DeleteLocalRef(current);

    if (currentText.find(s_watermarkText) == std::string::npos)
        s_lastServerFooter = currentText;

    std::string kept;
    std::stringstream ss(s_lastServerFooter);
    std::string line;

    while (std::getline(ss, line)) {
        std::string clean;
        stripSectionAndLower(line, clean);

        if (hasMatchingKeyword(clean))
            continue;

        if (!kept.empty())
            kept += "\n";

        kept += line;
    }

    std::string finalText = kept;

    if (!finalText.empty())
        finalText += "\n";

    finalText += s_watermarkText;

    jobject component = CMNewChatComponent(finalText);

    if (component) {
        Java::env->SetObjectField(tab, footerField, component);
        clearJNI();
        Java::env->DeleteLocalRef(component);
        s_tabWatermarkActive = true;
    }

    Java::env->DeleteLocalRef(tab);
    Java::env->DeleteLocalRef(mc);
}

static void CMClearTabWatermarkFooter() {
    if (!s_tabWatermarkActive)
        return;

    jobject mc = CMGetMinecraft();
    if (!mc)
        return;

    jobject tab = CMGetTabOverlay(mc);
    if (!tab) {
        Java::env->DeleteLocalRef(mc);
        return;
    }

    jfieldID footerField = CMGetFooterField(tab);

    if (footerField) {
        if (!s_lastServerFooter.empty()) {
            jobject original = CMNewChatComponent(s_lastServerFooter);

            if (original) {
                Java::env->SetObjectField(tab, footerField, original);
                clearJNI();
                Java::env->DeleteLocalRef(original);
            }
        } else {
            Java::env->SetObjectField(tab, footerField, nullptr);
            clearJNI();
        }
    }

    s_lastServerFooter.clear();
    s_tabWatermarkActive = false;

    Java::env->DeleteLocalRef(tab);
    Java::env->DeleteLocalRef(mc);
}

static void CMRestoreOriginalDisplayNames() {
    if (!Java::env || g_cmOriginalDisplayNames.empty())
        return;

    jobject mc = CMGetMinecraft();
    if (!mc)
        return;

    jobject nh = Java::env->CallObjectMethod(
        mc,
        g_cm189.minecraftGetNetHandler
    );

    if (clearJNI() || !nh) {
        Java::env->DeleteLocalRef(mc);
        return;
    }

    jobject collection = CMGetPlayerInfoCollection(nh);
    if (!collection) {
        Java::env->DeleteLocalRef(nh);
        Java::env->DeleteLocalRef(mc);
        return;
    }

    jobject iterator = Java::env->CallObjectMethod(
        collection,
        g_cm189.collectionIterator
    );

    if (clearJNI() || !iterator) {
        Java::env->DeleteLocalRef(collection);
        Java::env->DeleteLocalRef(nh);
        Java::env->DeleteLocalRef(mc);
        return;
    }

    int count = 0;
    while (count++ < 1000) {
        jboolean hasNext = Java::env->CallBooleanMethod(
            iterator,
            g_cm189.iteratorHasNext
        );

        if (clearJNI() || !hasNext)
            break;

        jobject pi = Java::env->CallObjectMethod(
            iterator,
            g_cm189.iteratorNext
        );

        if (clearJNI() || !pi)
            continue;

        std::string rawName;
        if (CMReadPlayerName(pi, rawName)) {
            auto it = g_cmOriginalDisplayNames.find(rawName);
            if (it != g_cmOriginalDisplayNames.end() && it->second) {
                Java::env->CallVoidMethod(
                    pi,
                    g_cm189.networkSetDisplayName,
                    it->second
                );
                clearJNI();
            }
        }

        Java::env->DeleteLocalRef(pi);
    }

    Java::env->DeleteLocalRef(iterator);
    Java::env->DeleteLocalRef(collection);
    Java::env->DeleteLocalRef(nh);
    Java::env->DeleteLocalRef(mc);
}

static void CMClearAllDisplayNames() {
    jobject mc = CMGetMinecraft();
    if (!mc)
        return;

    jobject nh = Java::env->CallObjectMethod(
        mc,
        g_cm189.minecraftGetNetHandler
    );

    if (clearJNI() || !nh) {
        Java::env->DeleteLocalRef(mc);
        return;
    }

    jobject collection = CMGetPlayerInfoCollection(nh);

    if (!collection) {
        Java::env->DeleteLocalRef(nh);
        Java::env->DeleteLocalRef(mc);
        return;
    }

    jobject iterator = Java::env->CallObjectMethod(
        collection,
        g_cm189.collectionIterator
    );

    if (clearJNI() || !iterator) {
        Java::env->DeleteLocalRef(collection);
        Java::env->DeleteLocalRef(nh);
        Java::env->DeleteLocalRef(mc);
        return;
    }

    int count = 0;

    while (count++ < 1000) {
        jboolean hasNext = Java::env->CallBooleanMethod(
            iterator,
            g_cm189.iteratorHasNext
        );

        if (clearJNI() || !hasNext)
            break;

        jobject pi = Java::env->CallObjectMethod(
            iterator,
            g_cm189.iteratorNext
        );

        if (clearJNI())
            break;

        if (!pi)
            continue;

        Java::env->CallVoidMethod(
            pi,
            g_cm189.networkSetDisplayName,
            nullptr
        );

        clearJNI();
        Java::env->DeleteLocalRef(pi);
    }

    Java::env->DeleteLocalRef(iterator);
    Java::env->DeleteLocalRef(collection);
    Java::env->DeleteLocalRef(nh);
    Java::env->DeleteLocalRef(mc);
}

static bool CMReadPlayerName(jobject pi, std::string& name) {
    name.clear();

    if (!pi || !g_cm189.networkGetGameProfile)
        return false;

    jobject profile = Java::env->CallObjectMethod(
        pi,
        g_cm189.networkGetGameProfile
    );

    if (clearJNI() || !profile)
        return false;

    jmethodID getName = g_cm189.gameProfileGetName;

    if (!getName) {
        jclass runtimeClass = Java::env->GetObjectClass(profile);

        if (!clearJNI() && runtimeClass) {
            getName = Java::env->GetMethodID(
                runtimeClass,
                "getName",
                "()Ljava/lang/String;"
            );

            if (Java::env->ExceptionCheck())
                Java::env->ExceptionClear();

            Java::env->DeleteLocalRef(runtimeClass);

            g_cm189.gameProfileGetName = getName;
        }
    }

    if (getName) {
        jstring nameJ = reinterpret_cast<jstring>(
            Java::env->CallObjectMethod(
                profile,
                getName
            )
        );

        if (!clearJNI() && nameJ)
            name = CMString(nameJ);

        if (nameJ)
            Java::env->DeleteLocalRef(nameJ);
    }

    Java::env->DeleteLocalRef(profile);

    return isValidName(name);
}

static std::string CMReadDisplayName(jobject pi, const std::string& fallback) {
    std::string formatted;
    jobject display = CMGetCurrentDisplay(pi, &formatted);

    if (display)
        Java::env->DeleteLocalRef(display);

    if (formatted.empty())
        return fallback;

    return formatted;
}

static std::string CMGetCurrentTeamFormattedName(
    jobject pi,
    const std::string& rawName
) {
    if (!pi || rawName.empty())
        return {};

    if (!Java::env ||
        !g_cm189.networkGetPlayerTeam ||
        !g_cm189.scorePlayerTeamClass ||
        !g_cm189.scorePlayerTeamFormatPlayerName)
        return {};

    jobject team = Java::env->CallObjectMethod(
        pi,
        g_cm189.networkGetPlayerTeam
    );

    if (clearJNI())
        return {};

    if (!team)
        return rawName;

    jstring raw = Java::env->NewStringUTF(rawName.c_str());
    if (!raw) {
        Java::env->DeleteLocalRef(team);
        return {};
    }

    jstring formatted = reinterpret_cast<jstring>(
        Java::env->CallStaticObjectMethod(
            g_cm189.scorePlayerTeamClass,
            g_cm189.scorePlayerTeamFormatPlayerName,
            team,
            raw
        )
    );

    Java::env->DeleteLocalRef(raw);
    Java::env->DeleteLocalRef(team);

    if (clearJNI() || !formatted)
        return {};

    const char* chars =
        Java::env->GetStringUTFChars(formatted, nullptr);

    std::string result;
    if (chars) {
        result = chars;
        Java::env->ReleaseStringUTFChars(formatted, chars);
    }

    Java::env->DeleteLocalRef(formatted);
    clearJNI();

    return result.empty() ? rawName : result;
}

static void CMInjectAllTabStats() {
    static auto lastRun =
        std::chrono::steady_clock::now() -
        std::chrono::milliseconds(50);

    const auto now = std::chrono::steady_clock::now();

    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastRun).count() < 100)
        return;

    lastRun = now;

    jobject mc = CMGetMinecraft();
    if (!mc)
        return;

    jobject nh = Java::env->CallObjectMethod(
        mc,
        g_cm189.minecraftGetNetHandler
    );

    if (clearJNI() || !nh) {
        Java::env->DeleteLocalRef(mc);
        return;
    }

    jobject collection = CMGetPlayerInfoCollection(nh);

    if (!collection) {
        Java::env->DeleteLocalRef(nh);
        Java::env->DeleteLocalRef(mc);
        return;
    }

    jobject iterator = Java::env->CallObjectMethod(
        collection,
        g_cm189.collectionIterator
    );

    if (clearJNI() || !iterator) {
        Java::env->DeleteLocalRef(collection);
        Java::env->DeleteLocalRef(nh);
        Java::env->DeleteLocalRef(mc);
        return;
    }

    const std::string marker = " \xC2\xA7r\xC2\xA7r ";

    int seen = 0;
    int injected = 0;
    int unchanged = 0;
    int waiting = 0;
    int failed = 0;

    while (seen++ < 1000) {
        jboolean hasNext =
            Java::env->CallBooleanMethod(
                iterator,
                g_cm189.iteratorHasNext
            );

        if (clearJNI() || !hasNext)
            break;

        jobject pi =
            Java::env->CallObjectMethod(
                iterator,
                g_cm189.iteratorNext
            );

        if (clearJNI() || !pi)
            continue;

        std::string rawName;

        if (!CMReadPlayerName(pi, rawName)) {
            ++failed;
            Java::env->DeleteLocalRef(pi);
            continue;
        }

        TabListStatsData stats;
        bool haveStats = false;

        {
            std::lock_guard<std::mutex> lock(TabList::cacheMutex);

            auto it = TabList::nameCache.find(rawName);

            if (it != TabList::nameCache.end() &&
                it->second.isValid &&
                (it->second.hasValidStats || (it->second.leaderboardFetched && it->second.profileFetched))) {
                stats = it->second;
                haveStats = true;
            }
        }

        if (!haveStats) {
            ++waiting;
            Java::env->DeleteLocalRef(pi);
            continue;
        }

        const std::string suffix =
            TabList::BuildStatSuffix(stats);

        if (suffix.empty()) {
            ++waiting;
            Java::env->DeleteLocalRef(pi);
            continue;
        }

        jobject currentDisplay = nullptr;
        std::string currentFormatted;

        currentDisplay =
            CMGetCurrentDisplay(
                pi,
                &currentFormatted
            );

        if (!currentDisplay)
            currentFormatted.clear();

        const std::string expectedSuffix =
            marker + suffix;

        const std::string currentTeamName =
            CMGetCurrentTeamFormattedName(pi, rawName);

        const size_t markerPos =
            currentFormatted.rfind(marker);

        std::string existingBase = currentFormatted;
        if (markerPos != std::string::npos)
            existingBase = currentFormatted.substr(0, markerPos);

        const bool teamBaseMatches =
            !currentTeamName.empty() &&
            existingBase.find(currentTeamName) != std::string::npos;

        if (markerPos != std::string::npos) {
            const std::string existingSuffix =
                currentFormatted.substr(markerPos);

            if (existingSuffix == expectedSuffix && teamBaseMatches) {
                ++unchanged;

                Java::env->DeleteLocalRef(currentDisplay);
                Java::env->DeleteLocalRef(pi);
                continue;
            }
        }

        jobject original = nullptr;

        // Keep the server team formatting when possible.
        if (!currentTeamName.empty())
            original = CMNewChatComponent(currentTeamName);

        // Fall back to the saved display if needed.
        if (!original) {
            auto savedOriginal = g_cmOriginalDisplayNames.find(rawName);
            if (savedOriginal != g_cmOriginalDisplayNames.end() && savedOriginal->second)
                original = CMCreateCopy(savedOriginal->second);
        }

        if (!original)
            original = CMSaveOriginalOrRecover(
                pi,
                rawName,
                currentDisplay,
                currentFormatted
            );

        if (!original)
            original = CMNewChatComponent(rawName);

        if (original && g_cmOriginalDisplayNames.find(rawName) == g_cmOriginalDisplayNames.end()) {
            jobject saved = Java::env->NewGlobalRef(original);
            if (saved)
                g_cmOriginalDisplayNames.emplace(rawName, saved);
        }

        if (!original) {
            ++failed;
            if (currentDisplay) Java::env->DeleteLocalRef(currentDisplay);
            Java::env->DeleteLocalRef(pi);
            continue;
        }

        jobject displayCopy =
            CMCreateCopy(original);

        if (!displayCopy || !g_cm189.chatAppendSibling) {
            ++failed;

            if (displayCopy)
                Java::env->DeleteLocalRef(displayCopy);

            Java::env->DeleteLocalRef(original);
            Java::env->DeleteLocalRef(currentDisplay);
            Java::env->DeleteLocalRef(pi);
            continue;
        }

        jobject statsComponent =
            CMNewChatComponent(expectedSuffix);

        if (!statsComponent) {
            ++failed;
            Java::env->DeleteLocalRef(displayCopy);
            Java::env->DeleteLocalRef(original);
            Java::env->DeleteLocalRef(currentDisplay);
            Java::env->DeleteLocalRef(pi);
            continue;
        }

        Java::env->CallObjectMethod(
            displayCopy,
            g_cm189.chatAppendSibling,
            statsComponent
        );

        if (clearJNI()) {
            ++failed;

            Java::env->DeleteLocalRef(statsComponent);
            Java::env->DeleteLocalRef(displayCopy);
            Java::env->DeleteLocalRef(original);
            Java::env->DeleteLocalRef(currentDisplay);
            Java::env->DeleteLocalRef(pi);
            continue;
        }

        Java::env->CallVoidMethod(
            pi,
            g_cm189.networkSetDisplayName,
            displayCopy
        );

        if (clearJNI()) {
            ++failed;
        } else {
            ++injected;
            s_injecting = true;
        }

        Java::env->DeleteLocalRef(statsComponent);
        Java::env->DeleteLocalRef(displayCopy);
        Java::env->DeleteLocalRef(original);
        Java::env->DeleteLocalRef(currentDisplay);
        Java::env->DeleteLocalRef(pi);
    }

    Java::env->DeleteLocalRef(iterator);
    Java::env->DeleteLocalRef(collection);
    Java::env->DeleteLocalRef(nh);
    Java::env->DeleteLocalRef(mc);
}

static bool CMShouldShowStats(jobject world) {
    if (!world || !g_cm189.worldGetScoreboard)
        return true;

    jobject scoreboard = Java::env->CallObjectMethod(
        world,
        g_cm189.worldGetScoreboard
    );

    if (clearJNI() || !scoreboard)
        return true;

    jobject objective = Java::env->CallObjectMethod(
        scoreboard,
        g_cm189.scoreboardGetObjectiveInDisplaySlot,
        1
    );

    if (clearJNI() || !objective) {
        Java::env->DeleteLocalRef(scoreboard);
        return true;
    }

    bool hasExplicitBedwars = false;
    bool hasExplicitNonBedwars = false;
    bool hasMap = false;

    if (g_cm189.scoreObjectiveGetDisplayName) {
        jstring displayName = reinterpret_cast<jstring>(
            Java::env->CallObjectMethod(
                objective,
                g_cm189.scoreObjectiveGetDisplayName
            )
        );

        if (!clearJNI() && displayName) {
            std::string objectiveText = CMString(displayName);

            std::string upper = objectiveText;
            for (char& c : upper)
                c = static_cast<char>(
                    std::toupper(static_cast<unsigned char>(c))
                );

            if (upper.find("BEDWARS") != std::string::npos)
                hasExplicitBedwars = true;

            if (upper.find("SKYWARS") != std::string::npos ||
                upper.find("DUELS") != std::string::npos ||
                upper.find("ARCADE") != std::string::npos ||
                upper.find("SURVIVAL") != std::string::npos ||
                upper.find("UHC") != std::string::npos ||
                upper.find("MURDER") != std::string::npos)
                hasExplicitNonBedwars = true;

            Java::env->DeleteLocalRef(displayName);
        } else {
            clearJNI();
        }
    }

    if (g_cm189.scoreboardGetSortedScores &&
        g_cm189.scoreGetPlayerName &&
        g_cm189.collectionIterator &&
        g_cm189.iteratorHasNext &&
        g_cm189.iteratorNext) {

        jobject scores = Java::env->CallObjectMethod(
            scoreboard,
            g_cm189.scoreboardGetSortedScores,
            objective
        );

        if (!clearJNI() && scores) {
            jobject iterator =
                Java::env->CallObjectMethod(
                    scores,
                    g_cm189.collectionIterator
                );

            if (!clearJNI() && iterator) {
                int count = 0;

                while (count++ < 1000) {
                    jboolean hasNext =
                        Java::env->CallBooleanMethod(
                            iterator,
                            g_cm189.iteratorHasNext
                        );

                    if (clearJNI() || !hasNext)
                        break;

                    jobject score =
                        Java::env->CallObjectMethod(
                            iterator,
                            g_cm189.iteratorNext
                        );

                    if (clearJNI() || !score)
                        continue;

                    jstring playerName =
                        reinterpret_cast<jstring>(
                            Java::env->CallObjectMethod(
                                score,
                                g_cm189.scoreGetPlayerName
                            )
                        );

                    if (!clearJNI() && playerName) {
                        std::string line = CMString(playerName);
                        std::string upper = line;

                        for (char& c : upper)
                            c = static_cast<char>(
                                std::toupper(
                                    static_cast<unsigned char>(c)
                                )
                            );

                        if (upper.find("MAP") != std::string::npos)
                            hasMap = true;

                        if (upper.find("BEDWARS") != std::string::npos)
                            hasExplicitBedwars = true;

                        if (upper.find("SKYWARS") != std::string::npos ||
                            upper.find("DUELS") != std::string::npos ||
                            upper.find("ARCADE") != std::string::npos ||
                            upper.find("SURVIVAL") != std::string::npos ||
                            upper.find("UHC") != std::string::npos ||
                            upper.find("MURDER") != std::string::npos)
                            hasExplicitNonBedwars = true;

                        Java::env->DeleteLocalRef(playerName);
                    } else {
                        clearJNI();
                    }

                    Java::env->DeleteLocalRef(score);

                    if (hasExplicitNonBedwars)
                        break;
                }

                Java::env->DeleteLocalRef(iterator);
            } else {
                clearJNI();
            }

            Java::env->DeleteLocalRef(scores);
        } else {
            clearJNI();
        }
    }

    Java::env->DeleteLocalRef(objective);
    Java::env->DeleteLocalRef(scoreboard);

    if (hasExplicitNonBedwars) {
        return false;
    }

    if (settings::TL_HideInLobby && !hasMap)
        return false;

    return true;
}

static void CMUpdate(TabList*) {
    if (!settings::TL_Enabled || !IsCMClient())
        return;

    jobject mc = CMGetMinecraft();
    if (!mc)
        return;

    static jobject lastWorld = nullptr;
    static auto lastRescan = std::chrono::steady_clock::now() -
        std::chrono::milliseconds(250);

    jobject world = nullptr;

    if (g_cm189.minecraftWorld) {
        world = Java::env->GetObjectField(mc, g_cm189.minecraftWorld);
        if (clearJNI())
            world = nullptr;
    }

    if (!world) {
        CMClearOriginalDisplayNames();
        TabList::ClearTaskQueues();
        Java::env->DeleteLocalRef(mc);
        return;
    }

    const bool worldChanged =
        !lastWorld ||
        Java::env->IsSameObject(lastWorld, world) == JNI_FALSE;

    if (worldChanged) {
        if (!g_cmOriginalDisplayNames.empty())
            CMRestoreOriginalDisplayNames();
        CMClearOriginalDisplayNames();

        if (lastWorld)
            Java::env->DeleteGlobalRef(lastWorld);

        lastWorld = Java::env->NewGlobalRef(world);
        TabList::ClearTaskQueues();
        lastRescan = std::chrono::steady_clock::now() -
            std::chrono::milliseconds(250);
    }

    if (!Base::isCompromised)
        CMInjectTabWatermarkFooter();

    // Keep the CM lobby state latched across tab refreshes.
    int realPlayerCount = 0;
    int npcCount = 0;
    bool foundLobbyGuildBracket = false;
    std::string bracketPlayer;
    std::vector<std::string> currentRoster;

    jobject nhCount = Java::env->CallObjectMethod(
        mc,
        g_cm189.minecraftGetNetHandler
    );

    if (!clearJNI() && nhCount) {
        jobject collection = CMGetPlayerInfoCollection(nhCount);

        if (collection) {
            jobject iterator = Java::env->CallObjectMethod(
                collection,
                g_cm189.collectionIterator
            );

            if (!clearJNI() && iterator) {
                int scanned = 0;

                while (scanned++ < 1000) {
                    jboolean hasNext = Java::env->CallBooleanMethod(
                        iterator,
                        g_cm189.iteratorHasNext
                    );

                    if (clearJNI() || !hasNext)
                        break;

                    jobject pi = Java::env->CallObjectMethod(
                        iterator,
                        g_cm189.iteratorNext
                    );

                    if (clearJNI() || !pi)
                        continue;

                    std::string rawName;
                    bool isNpc = false;

                    if (CMReadPlayerName(pi, rawName)) {
                        std::string upperName = rawName;
                        for (char& c : upperName)
                            c = static_cast<char>(
                                std::toupper(static_cast<unsigned char>(c))
                            );

                        if (upperName.find("NPC") != std::string::npos)
                            isNpc = true;

                        // CM can also mark NPCs through the rendered name.
                        std::string rendered;
                        if (g_cm189.networkGetDisplayName &&
                            g_cm189.chatGetFormattedText) {
                            jobject dc = Java::env->CallObjectMethod(
                                pi,
                                g_cm189.networkGetDisplayName
                            );

                            if (!clearJNI() && dc) {
                                jstring df = reinterpret_cast<jstring>(
                                    Java::env->CallObjectMethod(
                                        dc,
                                        g_cm189.chatGetFormattedText
                                    )
                                );

                                if (!clearJNI() && df) {
                                    rendered = CMString(df);
                                    std::string renderedUpper = rendered;

                                    for (char& c : renderedUpper)
                                        c = static_cast<char>(
                                            std::toupper(
                                                static_cast<unsigned char>(c)
                                            )
                                        );

                                    if (renderedUpper.find("NPC") != std::string::npos)
                                        isNpc = true;

                                    Java::env->DeleteLocalRef(df);
                                } else {
                                    clearJNI();
                                }

                                Java::env->DeleteLocalRef(dc);
                            } else {
                                clearJNI();
                            }
                        }

                        if (isNpc) {
                            ++npcCount;
                        } else {
                            ++realPlayerCount;
                            currentRoster.push_back(rawName);

                            // Strip our suffix before checking the guild tag.
                            const std::string marker = " \xC2\xA7r\xC2\xA7r ";
                            const size_t markerPos = rendered.find(marker);
                            const std::string baseDisplay =
                                markerPos == std::string::npos
                                    ? rendered
                                    : rendered.substr(0, markerPos);

                            // A bracketed guild/tag marks the lobby.
                            const size_t open = baseDisplay.find('[');
                            const size_t close =
                                open == std::string::npos
                                    ? std::string::npos
                                    : baseDisplay.find(']', open + 1);

                            if (open != std::string::npos &&
                                close != std::string::npos &&
                                close > open + 1) {
                                foundLobbyGuildBracket = true;
                                if (bracketPlayer.empty())
                                    bracketPlayer = rawName;
                            }
                        }
                    }

                    Java::env->DeleteLocalRef(pi);
                }

                Java::env->DeleteLocalRef(iterator);
            } else {
                clearJNI();
            }

            Java::env->DeleteLocalRef(collection);
        }

        Java::env->DeleteLocalRef(nhCount);
    } else {
        clearJNI();
    }

    static bool cmLobbyLocked = false;
    static std::vector<std::string> cmLobbyRoster;

    auto rosterOverlap = [](const std::vector<std::string>& a,
                            const std::vector<std::string>& b) -> int {
        int common = 0;
        for (const auto& x : a) {
            if (std::find(b.begin(), b.end(), x) != b.end())
                ++common;
        }
        return common;
    };

    // Once a guild bracket or >16 players has confirmed a lobby, latch that
    // state. We only unlock it when the current 1..16-player roster is clearly
    if (!cmLobbyLocked && (foundLobbyGuildBracket || realPlayerCount > 16)) {
        cmLobbyLocked = true;
        cmLobbyRoster = currentRoster;

        LOG_INFO(
            "[TabList][CM] LOBBY LOCKED: players=%d npc=%d reason=%s%s",
            realPlayerCount,
            npcCount,
            foundLobbyGuildBracket ? "guild-bracket" : "player-count-over-16",
            bracketPlayer.empty() ? "" : (" player=" + bracketPlayer).c_str()
        );
    }

    if (cmLobbyLocked &&
        !foundLobbyGuildBracket &&
        realPlayerCount >= 1 &&
        realPlayerCount <= 16 &&
        !cmLobbyRoster.empty()) {
        const int common = rosterOverlap(cmLobbyRoster, currentRoster);
        const int total = static_cast<int>(cmLobbyRoster.size() + currentRoster.size() - common);

        // A genuine BedWars join replaces the lobby pool with a new player
        // pool. The old lobby players may include the local player, so require
        // the two pools to be clearly different rather than requiring zero
        // overlap.
        const bool newPool = total > 0 && (common * 2 < total);

        if (newPool) {
            cmLobbyLocked = false;
            cmLobbyRoster.clear();

            LOG_INFO(
                "[TabList][CM] LOBBY CLEARED: new game pool detected players=%d common=%d/%d",
                realPlayerCount,
                common,
                total
            );
        }
    }

    // Reset the CM state on world changes.
    if (worldChanged) {
        cmLobbyLocked = false;
        cmLobbyRoster.clear();

        if (foundLobbyGuildBracket || realPlayerCount > 16) {
            cmLobbyLocked = true;
            cmLobbyRoster = currentRoster;
        }
    }

    const bool inGame =
        !cmLobbyLocked &&
        realPlayerCount >= 1 &&
        realPlayerCount <= 16;

    static bool lastInGame = false;
    static int lastRealPlayerCount = -1;
    static bool lastFoundBracket = false;

    if (worldChanged ||
        inGame != lastInGame ||
        realPlayerCount != lastRealPlayerCount ||
        foundLobbyGuildBracket != lastFoundBracket) {
        if (inGame) {
            LOG_INFO(
                "[TabList][CM] GAME ACTIVE: players=%d npc=%d guildBracket=%s",
                realPlayerCount,
                npcCount,
                foundLobbyGuildBracket ? "YES" : "NO"
            );
        } else if (foundLobbyGuildBracket) {
            LOG_INFO(
                "[TabList][CM] LOBBY: players=%d npc=%d reason=guild-bracket player=%s",
                realPlayerCount,
                npcCount,
                bracketPlayer.empty() ? "?" : bracketPlayer.c_str()
            );
        } else if (realPlayerCount > 16) {
            LOG_INFO(
                "[TabList][CM] LOBBY: players=%d npc=%d reason=player-count-over-16",
                realPlayerCount,
                npcCount
            );
        }

        lastInGame = inGame;
        lastRealPlayerCount = realPlayerCount;
        lastFoundBracket = foundLobbyGuildBracket;
    }

    static bool cmWasInGame = false;

    if (!inGame) {
        if (cmWasInGame || s_injecting) {
            // Restore the original names when leaving the match.
            CMRestoreOriginalDisplayNames();
            CMClearOriginalDisplayNames();
            s_injecting = false;
        }

        // Do not fetch while the CM lobby state is active.
        TabList::ClearTaskQueues();
        cmWasInGame = false;

        Java::env->DeleteLocalRef(world);
        Java::env->DeleteLocalRef(mc);
        return;
    }

    cmWasInGame = true;

    const auto now = std::chrono::steady_clock::now();

    // Fetch while CM considers the player list an active game.
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastRescan
        ).count() >= 100) {
        lastRescan = now;

        jobject nh = Java::env->CallObjectMethod(
            mc,
            g_cm189.minecraftGetNetHandler
        );

        if (!clearJNI() && nh) {
            jobject collection = CMGetPlayerInfoCollection(nh);

            if (collection) {
                jobject iterator = Java::env->CallObjectMethod(
                    collection,
                    g_cm189.collectionIterator
                );

                if (!clearJNI() && iterator) {
                    int discovered = 0;

                    while (discovered++ < 1000) {
                        jboolean hasNext = Java::env->CallBooleanMethod(
                            iterator,
                            g_cm189.iteratorHasNext
                        );

                        if (clearJNI() || !hasNext)
                            break;

                        jobject pi = Java::env->CallObjectMethod(
                            iterator,
                            g_cm189.iteratorNext
                        );

                        if (clearJNI() || !pi)
                            continue;

                        std::string name;
                        if (CMReadPlayerName(pi, name)) {
                            std::string upperName = name;
                            for (char& c : upperName)
                                c = static_cast<char>(
                                    std::toupper(static_cast<unsigned char>(c))
                                );

                            bool isNpc =
                                upperName.find("NPC") != std::string::npos;

                            if (!isNpc &&
                                g_cm189.networkGetDisplayName &&
                                g_cm189.chatGetFormattedText) {
                                jobject dc = Java::env->CallObjectMethod(
                                    pi,
                                    g_cm189.networkGetDisplayName
                                );

                                if (!clearJNI() && dc) {
                                    jstring df = reinterpret_cast<jstring>(
                                        Java::env->CallObjectMethod(
                                            dc,
                                            g_cm189.chatGetFormattedText
                                        )
                                    );

                                    if (!clearJNI() && df) {
                                        std::string rendered = CMString(df);
                                        std::string renderedUpper = rendered;

                                        for (char& c : renderedUpper)
                                            c = static_cast<char>(
                                                std::toupper(static_cast<unsigned char>(c))
                                            );

                                        if (renderedUpper.find("NPC") != std::string::npos)
                                            isNpc = true;

                                        Java::env->DeleteLocalRef(df);
                                    } else {
                                        clearJNI();
                                    }

                                    Java::env->DeleteLocalRef(dc);
                                } else {
                                    clearJNI();
                                }
                            }

                            if (!isNpc) {
                                bool needsFetch = false;

                                {
                                    std::lock_guard<std::mutex> lock(TabList::cacheMutex);
                                    auto it = TabList::nameCache.find(name);

                                    if (it == TabList::nameCache.end()) {
                                        TabListStatsData data;
                                        data.isValid = true;
                                        data.fetchTime = std::chrono::system_clock::now();
                                        TabList::nameCache.emplace(name, data);
                                        needsFetch = true;
                                    } else if (!it->second.leaderboardFetched ||
                                               !it->second.profileFetched) {
                                        const auto age =
                                            std::chrono::duration_cast<std::chrono::seconds>(
                                                std::chrono::system_clock::now() -
                                                it->second.fetchTime
                                            ).count();

                                        if (age > 15) {
                                            it->second.fetchTime = std::chrono::system_clock::now();
                                            needsFetch = true;
                                        }
                                    }
                                }

                                if (needsFetch)
                                    TabList::EnqueuePlayer(name);
                            }
                        }

                        Java::env->DeleteLocalRef(pi);
                    }

                    Java::env->DeleteLocalRef(iterator);
                } else {
                    clearJNI();
                }

                Java::env->DeleteLocalRef(collection);
            }

            Java::env->DeleteLocalRef(nh);
        }

        clearJNI();
    }

    static auto lastInjection =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(50);

    if (!Base::isCompromised) {
        const auto injectNow = std::chrono::steady_clock::now();

        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                injectNow - lastInjection
            ).count() >= 100) {
            lastInjection = injectNow;
            CMInjectAllTabStats();
        }
    }

    Java::env->DeleteLocalRef(world);
    Java::env->DeleteLocalRef(mc);
}

static void CMRenderHud() {
    if (!settings::TL_Enabled ||
        !IsCMClient() ||
        !(GetAsyncKeyState(VK_TAB) & 0x8000) ||
        Menu::open)
        return;

    jobject mc = CMGetMinecraft();
    if (!mc)
        return;

    jobject nh = Java::env->CallObjectMethod(
        mc,
        g_cm189.minecraftGetNetHandler
    );

    if (clearJNI() || !nh) {
        Java::env->DeleteLocalRef(mc);
        return;
    }

    jobject collection =
        CMGetPlayerInfoCollection(nh);

    if (!collection) {
        Java::env->DeleteLocalRef(nh);
        Java::env->DeleteLocalRef(mc);
        return;
    }

    jobject iterator =
        Java::env->CallObjectMethod(
            collection,
            g_cm189.collectionIterator
        );

    if (clearJNI() || !iterator) {
        Java::env->DeleteLocalRef(collection);
        Java::env->DeleteLocalRef(nh);
        Java::env->DeleteLocalRef(mc);
        return;
    }

    struct PlayerRow {
        std::string name;
        int ping;
        std::string statsText;
    };

    std::vector<PlayerRow> rows;
    bool allStatsReady = true;
    const std::string marker = " \xC2\xA7r\xC2\xA7r ";

    int count = 0;

    while (count++ < 1000) {
        jboolean hasNext =
            Java::env->CallBooleanMethod(
                iterator,
                g_cm189.iteratorHasNext
            );

        if (clearJNI() || !hasNext)
            break;

        jobject pi =
            Java::env->CallObjectMethod(
                iterator,
                g_cm189.iteratorNext
            );

        if (clearJNI() || !pi)
            continue;

        std::string rawName;

        if (!CMReadPlayerName(pi, rawName)) {
            Java::env->DeleteLocalRef(pi);
            continue;
        }

        int ping = 0;

        ping = Java::env->CallIntMethod(
            pi,
            g_cm189.networkGetResponseTime
        );

        if (clearJNI())
            ping = 0;

        TabListStatsData stats;
        bool haveStats = false;

        {
            std::lock_guard<std::mutex> lock(TabList::cacheMutex);

            auto it = TabList::nameCache.find(rawName);

            if (it != TabList::nameCache.end()) {
                stats = it->second;
                haveStats = true;
            }
        }

        if (!haveStats || (!stats.hasValidStats && (!stats.leaderboardFetched || !stats.profileFetched)))
            allStatsReady = false;

        std::string display =
            CMReadDisplayName(pi, rawName);

        size_t markerPos = display.find(marker);

        if (markerPos != std::string::npos)
            display = display.substr(0, markerPos);

        std::string statsText;

        if (haveStats)
            statsText = TabList::BuildStatSuffix(stats);

        rows.push_back({ display, ping, statsText });

        Java::env->DeleteLocalRef(pi);
    }

    Java::env->DeleteLocalRef(iterator);
    Java::env->DeleteLocalRef(collection);
    Java::env->DeleteLocalRef(nh);
    Java::env->DeleteLocalRef(mc);
    clearJNI();

    if (rows.empty() || !allStatsReady)
        return;

    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetIO().DisplaySize.x * 0.15f, 20),
        ImGuiCond_Always
    );

    ImGui::SetNextWindowSize(
        ImVec2(
            ImGui::GetIO().DisplaySize.x * 0.7f,
            ImGui::GetIO().DisplaySize.y - 40
        ),
        ImGuiCond_Always
    );

    ImGui::PushStyleColor(
        ImGuiCol_WindowBg,
        ImVec4(0.0f, 0.0f, 0.0f, 0.85f)
    );

    ImGui::PushStyleColor(
        ImGuiCol_Border,
        ImVec4(0.3f, 0.3f, 0.3f, 1.0f)
    );

    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowRounding,
        4.0f
    );

    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(8, 4)
    );

    ImGui::Begin(
        "TabList Stats##TabListOverlay",
        nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings
    );

    ImGui::Columns(3, "tablist_cols", false);

    ImGui::SetColumnWidth(
        0,
        ImGui::GetWindowWidth() * 0.5f
    );

    ImGui::SetColumnWidth(
        1,
        ImGui::GetWindowWidth() * 0.25f
    );

    ImGui::SetColumnWidth(
        2,
        ImGui::GetWindowWidth() * 0.25f
    );

    ImGui::TextColored(
        ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
        "Player"
    );
    ImGui::NextColumn();

    ImGui::TextColored(
        ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
        "Ping"
    );
    ImGui::NextColumn();

    ImGui::TextColored(
        ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
        "Stats"
    );
    ImGui::NextColumn();

    ImGui::Separator();

    for (const auto& row : rows) {
        ImGui::Text("%s", row.name.c_str());
        ImGui::NextColumn();

        int ping = row.ping;

        float pingFrac =
            ping > 200
                ? 1.0f
                : ping / 200.0f;

        ImVec4 pingColor(
            0.0f + pingFrac,
            1.0f - pingFrac,
            0.0f,
            1.0f
        );

        char pingText[16];
        snprintf(
            pingText,
            sizeof(pingText),
            "%dms",
            ping
        );

        ImGui::TextColored(
            pingColor,
            "%s",
            pingText
        );

        ImGui::NextColumn();

        if (!row.statsText.empty()) {
            std::string clean = row.statsText;

            for (size_t i = 0; i < clean.size();) {
                if ((unsigned char)clean[i] == 0xC2 &&
                    i + 1 < clean.size() &&
                    (unsigned char)clean[i + 1] == 0xA7) {

                    clean.erase(i, 2);
                    continue;
                }

                ++i;
            }

            while (!clean.empty() && clean[0] == ' ')
                clean.erase(0, 1);

            ImGui::TextColored(
                ImVec4(0.0f, 1.0f, 1.0f, 1.0f),
                "%s",
                clean.c_str()
            );
        }

        ImGui::NextColumn();
    }

    ImGui::Columns(1);
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}
}

bool TabList::IsEnabled() { return settings::TL_Enabled; }
void TabList::SetEnabled(bool enabled) {
    if (settings::TL_Enabled == enabled) return;
    settings::TL_Enabled = enabled;
    if (!enabled) s_injecting = false;
}
void TabList::Toggle() { SetEnabled(!settings::TL_Enabled); }
void TabList::ClearCache() {
    std::lock_guard<std::mutex> l(cacheMutex);
    nameCache.clear();
    ClearTaskQueues();
}

void TabList::ClearAllDisplayNames() {
    if (!Java::env || !RuntimeBindings::minecraft_class || !RuntimeBindings::minecraft_getMinecraft) return;
    if (!RuntimeBindings::minecraft_getNetHandler) return;
    if (!RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap && !RuntimeBindings::netHandlerPlayClient_playerInfoList) return;
    if (!RuntimeBindings::networkPlayerInfo_setDisplayName) return;
    if (!RuntimeBindings::collection_iterator || !RuntimeBindings::iterator_hasNext || !RuntimeBindings::iterator_next) return;
    try {
        jobject mc = Java::env->CallStaticObjectMethod(RuntimeBindings::minecraft_class, RuntimeBindings::minecraft_getMinecraft);
        if (clearJNI() || !mc) return;
        jobject nh = Java::env->CallObjectMethod(mc, RuntimeBindings::minecraft_getNetHandler);
        if (clearJNI() || !nh) { Java::env->DeleteLocalRef(mc); return; }
        jobject col = nullptr;
        if (RuntimeBindings::netHandlerPlayClient_playerInfoList && RuntimeBindings::map_values) {
            jobject map = Java::env->GetObjectField(nh, RuntimeBindings::netHandlerPlayClient_playerInfoList);
            if (!clearJNI() && map) { col = Java::env->CallObjectMethod(map, RuntimeBindings::map_values); Java::env->DeleteLocalRef(map); }
        }
        if (!col && RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap) {
            col = Java::env->CallObjectMethod(nh, RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap); clearJNI();
        }
        if (col) {
            jobject it = Java::env->CallObjectMethod(col, RuntimeBindings::collection_iterator);
            if (!clearJNI() && it) {
                int n = 0;
                while (n++ < 1000) {
                    if (!Java::env->CallBooleanMethod(it, RuntimeBindings::iterator_hasNext)) break;
                    if (clearJNI()) break;
                    jobject pi = Java::env->CallObjectMethod(it, RuntimeBindings::iterator_next);
                    if (clearJNI()) break;
                    if (pi) {
                        Java::env->CallVoidMethod(pi, RuntimeBindings::networkPlayerInfo_setDisplayName, nullptr);
                        clearJNI();
                        Java::env->DeleteLocalRef(pi);
                    }
                }
                Java::env->DeleteLocalRef(it);
            }
            clearJNI();
            Java::env->DeleteLocalRef(col);
        }
        Java::env->DeleteLocalRef(nh);
        Java::env->DeleteLocalRef(mc);
    }
    catch (...) {}
    clearJNI();
    s_injecting = false;
}

void TabList::InjectAllTabStats() {
    if (!Java::env) return;
    if (!RuntimeBindings::init()) return;

    if (!RuntimeBindings::minecraft_class || !RuntimeBindings::minecraft_getMinecraft || !RuntimeBindings::minecraft_getNetHandler) return;
    if (!RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap && !RuntimeBindings::netHandlerPlayClient_playerInfoList) return;
    if (!RuntimeBindings::networkPlayerInfo_getGameProfile || !RuntimeBindings::gameProfile_class || !RuntimeBindings::gameProfile_getName) return;
    if (!RuntimeBindings::chatComponentText_class || !RuntimeBindings::chatComponentText_init) return;
    if (!RuntimeBindings::map_values || !RuntimeBindings::collection_iterator || !RuntimeBindings::iterator_hasNext || !RuntimeBindings::iterator_next) return;

    const std::string MARKER = " \xC2\xA7r\xC2\xA7r ";
    int cnt = 0;

    try {
        jobject mc = Java::env->CallStaticObjectMethod(RuntimeBindings::minecraft_class, RuntimeBindings::minecraft_getMinecraft);
        if (clearJNI() || !mc) return;

        jobject nh = Java::env->CallObjectMethod(mc, RuntimeBindings::minecraft_getNetHandler);
        if (clearJNI() || !nh) {
            Java::env->DeleteLocalRef(mc);
            return;
        }

        jobject col = nullptr;
        if (RuntimeBindings::netHandlerPlayClient_playerInfoList && RuntimeBindings::map_values) {
            jobject map = Java::env->GetObjectField(nh, RuntimeBindings::netHandlerPlayClient_playerInfoList);
            if (!clearJNI() && map) { col = Java::env->CallObjectMethod(map, RuntimeBindings::map_values); Java::env->DeleteLocalRef(map); }
        }
        if (!col && RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap) {
            col = Java::env->CallObjectMethod(nh, RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap); clearJNI();
        }

        if (!col) {
            Java::env->DeleteLocalRef(nh);
            Java::env->DeleteLocalRef(mc);
            return;
        }

        jobject it = Java::env->CallObjectMethod(col, RuntimeBindings::collection_iterator);
        if (clearJNI() || !it) {
            Java::env->DeleteLocalRef(col);
            Java::env->DeleteLocalRef(nh);
            Java::env->DeleteLocalRef(mc);
            return;
        }

        int limit = 0;
        while (limit++ < 1000) {
            if (!Java::env->CallBooleanMethod(it, RuntimeBindings::iterator_hasNext)) break;
            if (clearJNI()) break;

            jobject pi = Java::env->CallObjectMethod(it, RuntimeBindings::iterator_next);
            if (clearJNI()) break;
            if (!pi) continue;

            std::string rawName;
            jobject gp = Java::env->CallObjectMethod(pi, RuntimeBindings::networkPlayerInfo_getGameProfile);
            if (!clearJNI() && gp) {
                jstring nj = (jstring)Java::env->CallObjectMethod(gp, RuntimeBindings::gameProfile_getName);
                if (!clearJNI() && nj) {
                    const char* c = Java::env->GetStringUTFChars(nj, nullptr);
                    if (c) { rawName = c; Java::env->ReleaseStringUTFChars(nj, c); }
                    Java::env->DeleteLocalRef(nj);
                }
                Java::env->DeleteLocalRef(gp);
            }

            if (!isValidName(rawName)) {
                Java::env->DeleteLocalRef(pi);
                continue;
            }

            // Always dynamically fetch native Scoreboard Team prefix/suffix first to preserve team colors
            std::string baseName = rawName;
            if (RuntimeBindings::networkPlayerInfo_getPlayerTeam && RuntimeBindings::scorePlayerTeam_getColorPrefix) {
                jobject teamObj = Java::env->CallObjectMethod(pi, RuntimeBindings::networkPlayerInfo_getPlayerTeam);
                if (!clearJNI() && teamObj != nullptr) {
                    std::string prefixStr = "";
                    std::string suffixStr = "";
                    jstring pStr = (jstring)Java::env->CallObjectMethod(teamObj, RuntimeBindings::scorePlayerTeam_getColorPrefix);
                    if (!clearJNI() && pStr) {
                        const char* pChars = Java::env->GetStringUTFChars(pStr, nullptr);
                        if (pChars) { prefixStr = pChars; Java::env->ReleaseStringUTFChars(pStr, pChars); }
                        Java::env->DeleteLocalRef(pStr);
                    }
                    jstring sStr = (jstring)Java::env->CallObjectMethod(teamObj, RuntimeBindings::scorePlayerTeam_getColorSuffix);
                    if (!clearJNI() && sStr) {
                        const char* sChars = Java::env->GetStringUTFChars(sStr, nullptr);
                        if (sChars) { suffixStr = sChars; Java::env->ReleaseStringUTFChars(sStr, sChars); }
                        Java::env->DeleteLocalRef(sStr);
                    }
                    baseName = prefixStr + rawName + suffixStr;
                    Java::env->DeleteLocalRef(teamObj);
                }
                clearJNI();
            }

            TabListStatsData st; 
            bool hs = false;
            { 
                std::lock_guard<std::mutex> l(cacheMutex); 
                auto f = nameCache.find(rawName); 
                if (f != nameCache.end() && f->second.isValid && (f->second.hasValidStats || (f->second.leaderboardFetched && f->second.profileFetched))) { 
                    st = f->second; 
                    hs = true; 
                } 
            }

            if (!hs) {
                Java::env->DeleteLocalRef(pi);
                continue;
            }

            std::string suf = BuildStatSuffix(st);
            if (suf.empty()) {
                Java::env->DeleteLocalRef(pi);
                continue;
            }

            std::string nd = baseName + MARKER + suf;
            jstring nj = Java::env->NewStringUTF(nd.c_str());
            if (nj) {
                jobject cc = Java::env->NewObject(RuntimeBindings::chatComponentText_class, RuntimeBindings::chatComponentText_init, nj);
                if (!clearJNI() && cc && RuntimeBindings::networkPlayerInfo_setDisplayName) {
                    Java::env->CallVoidMethod(pi, RuntimeBindings::networkPlayerInfo_setDisplayName, cc);
                    clearJNI();
                    Java::env->DeleteLocalRef(cc);
                    cnt++;
                }
                Java::env->DeleteLocalRef(nj);
            }
            Java::env->DeleteLocalRef(pi);
        }
        Java::env->DeleteLocalRef(it);
        Java::env->DeleteLocalRef(col);
        Java::env->DeleteLocalRef(nh);
        Java::env->DeleteLocalRef(mc);
    }
    catch (...) {
    }
    clearJNI();
    if (cnt > 0) s_injecting = true;
}

bool TabList::ShouldShowStats(jobject mc) {
    if (!settings::TL_Enabled || !mc || !Java::env) return false;
    if (!RuntimeBindings::minecraft_theWorld) return true;
    jobject world = Java::env->GetObjectField(mc, RuntimeBindings::minecraft_theWorld);
    if (clearJNI() || !world) return false;
    if (!RuntimeBindings::world_getScoreboard || !RuntimeBindings::scoreboard_getObjectiveInDisplaySlot || !RuntimeBindings::scoreObjective_getDisplayName) {
        Java::env->DeleteLocalRef(world);
        return true;
    }
    bool hasBedwars = false;
    bool hasMap = false;
    bool hasAssignedTeamColor = false;
    bool hasPika = false;
    jobject sb = Java::env->CallObjectMethod(world, RuntimeBindings::world_getScoreboard);
    if (clearJNI() || !sb) { Java::env->DeleteLocalRef(world); return true; }
    jobject obj = Java::env->CallObjectMethod(sb, RuntimeBindings::scoreboard_getObjectiveInDisplaySlot, 1);
    if (!clearJNI() && obj) {
        jstring ds = (jstring)Java::env->CallObjectMethod(obj, RuntimeBindings::scoreObjective_getDisplayName);
        if (!clearJNI() && ds) {
            const char* c = Java::env->GetStringUTFChars(ds, nullptr);
            if (c) {
                std::string dn = c; Java::env->ReleaseStringUTFChars(ds, c);
                for (auto& x : dn) x = (char)toupper((unsigned char)x);
                if (dn.find("BEDWARS") != std::string::npos) hasBedwars = true;
            }
            Java::env->DeleteLocalRef(ds);
        }
        if (hasBedwars && RuntimeBindings::scoreboard_getSortedScores && RuntimeBindings::score_getPlayerName && RuntimeBindings::scoreboard_getPlayersTeam && RuntimeBindings::scorePlayerTeam_formatPlayerName && RuntimeBindings::collection_iterator && RuntimeBindings::iterator_hasNext && RuntimeBindings::iterator_next) {
            jobject scoresCol = Java::env->CallObjectMethod(sb, RuntimeBindings::scoreboard_getSortedScores, obj);
            if (!clearJNI() && scoresCol) {
                jobject it = Java::env->CallObjectMethod(scoresCol, RuntimeBindings::collection_iterator);
                if (!clearJNI() && it) {
                    while (Java::env->CallBooleanMethod(it, RuntimeBindings::iterator_hasNext)) {
                        if (clearJNI()) break;
                        jobject scoreObj = Java::env->CallObjectMethod(it, RuntimeBindings::iterator_next);
                        if (clearJNI()) break;
                        if (scoreObj) {
                            jstring pNameJ = (jstring)Java::env->CallObjectMethod(scoreObj, RuntimeBindings::score_getPlayerName);
                            if (!clearJNI() && pNameJ) {
                                jobject teamObj = Java::env->CallObjectMethod(sb, RuntimeBindings::scoreboard_getPlayersTeam, pNameJ);
                                bool mustDeleteFormatted = false;
                                jstring formattedNameJ = nullptr;
                                if (!clearJNI()) {
                                    if (teamObj) {
                                        formattedNameJ = (jstring)Java::env->CallStaticObjectMethod(RuntimeBindings::scorePlayerTeam_class, RuntimeBindings::scorePlayerTeam_formatPlayerName, teamObj, pNameJ);
                                        mustDeleteFormatted = true;
                                    } else { formattedNameJ = pNameJ; }
                                    if (!clearJNI() && formattedNameJ) {
                                        const char* cStr = Java::env->GetStringUTFChars(formattedNameJ, nullptr);
                                        if (cStr) {
                                            std::string line = cStr; Java::env->ReleaseStringUTFChars(formattedNameJ, cStr);
                                            for (auto& x : line) x = (char)toupper((unsigned char)x);
                                            if (line.find("MAP") != std::string::npos) hasMap = true;
                                            if (line.find("PIKA") != std::string::npos) hasPika = true;
                                        }
                                        if (mustDeleteFormatted) Java::env->DeleteLocalRef(formattedNameJ);
                                    }
                                    if (teamObj) Java::env->DeleteLocalRef(teamObj);
                                }
                                Java::env->DeleteLocalRef(pNameJ);
                            }
                            Java::env->DeleteLocalRef(scoreObj);
                        }
                        if (hasMap) break;
                    }
                    Java::env->DeleteLocalRef(it);
                }
                Java::env->DeleteLocalRef(scoresCol);
            }
        }
        Java::env->DeleteLocalRef(obj);
    }

    // Non-CM only: a BedWars lobby can have the BedWars scoreboard before
    // the player receives an actual BedWars team. Treat only a bare Minecraft
    // color prefix as an assigned team color.
    if (RuntimeBindings::netHandlerPlayClient_playerInfoList &&
        RuntimeBindings::map_values &&
        RuntimeBindings::networkPlayerInfo_getPlayerTeam &&
        RuntimeBindings::scorePlayerTeam_getColorPrefix &&
        RuntimeBindings::collection_iterator &&
        RuntimeBindings::iterator_hasNext &&
        RuntimeBindings::iterator_next) {
        jobject nhTeam = Java::env->CallObjectMethod(mc, RuntimeBindings::minecraft_getNetHandler);
        if (!clearJNI() && nhTeam) {
            jobject mapTeam = Java::env->GetObjectField(nhTeam, RuntimeBindings::netHandlerPlayClient_playerInfoList);
            jobject colTeam = nullptr;
            if (!clearJNI() && mapTeam) {
                colTeam = Java::env->CallObjectMethod(mapTeam, RuntimeBindings::map_values);
                Java::env->DeleteLocalRef(mapTeam);
            }

            if (colTeam) {
                jobject itTeam = Java::env->CallObjectMethod(colTeam, RuntimeBindings::collection_iterator);
                if (!clearJNI() && itTeam) {
                    int scanned = 0;
                    while (scanned++ < 1000 && Java::env->CallBooleanMethod(itTeam, RuntimeBindings::iterator_hasNext)) {
                        if (clearJNI()) break;
                        jobject piTeam = Java::env->CallObjectMethod(itTeam, RuntimeBindings::iterator_next);
                        if (clearJNI() || !piTeam) continue;
                        jobject teamObj = Java::env->CallObjectMethod(piTeam, RuntimeBindings::networkPlayerInfo_getPlayerTeam);
                        if (!clearJNI() && teamObj) {
                            jstring prefixJ = reinterpret_cast<jstring>(Java::env->CallObjectMethod(teamObj, RuntimeBindings::scorePlayerTeam_getColorPrefix));
                            if (!clearJNI() && prefixJ) {
                                const char* chars = Java::env->GetStringUTFChars(prefixJ, nullptr);
                                if (chars) {
                                    std::string prefix = chars;
                                    Java::env->ReleaseStringUTFChars(prefixJ, chars);
                                    const bool isBareColorPrefix =
                                        prefix.size() == 3 &&
                                        static_cast<unsigned char>(prefix[0]) == 0xC2 &&
                                        static_cast<unsigned char>(prefix[1]) == 0xA7 &&
                                        ((prefix[2] >= '0' && prefix[2] <= '9') ||
                                         (prefix[2] >= 'a' && prefix[2] <= 'f') ||
                                         (prefix[2] >= 'A' && prefix[2] <= 'F'));
                                    if (isBareColorPrefix) hasAssignedTeamColor = true;
                                }
                                Java::env->DeleteLocalRef(prefixJ);
                            }
                            Java::env->DeleteLocalRef(teamObj);
                        }
                        Java::env->DeleteLocalRef(piTeam);
                        if (hasAssignedTeamColor) break;
                    }
                    Java::env->DeleteLocalRef(itTeam);
                }
                Java::env->DeleteLocalRef(colTeam);
            }
            Java::env->DeleteLocalRef(nhTeam);
        }
        clearJNI();
    }

    Java::env->DeleteLocalRef(sb);
    Java::env->DeleteLocalRef(world);
    clearJNI();
    if (!hasBedwars || !hasPika) return false;
    // Non-CM only: colored scoreboard teams can also exist in BedWars lobbies.
    // MAP is therefore the authoritative active-game marker for the lobby toggle.
    const bool inGame = hasMap;
    return inGame || !settings::TL_HideInLobby;
}

void TabList::InjectTabWatermarkFooter() {
    if (!Java::env || !settings::TL_Enabled) return;

    try {
        jobject mcObj = nullptr;
        jobject tabOverlay = nullptr;
        if (RuntimeBindings::minecraft_class && RuntimeBindings::minecraft_getMinecraft && RuntimeBindings::minecraft_ingameGUI) {
            mcObj = Java::env->CallStaticObjectMethod(RuntimeBindings::minecraft_class, RuntimeBindings::minecraft_getMinecraft);
            if (!clearJNI() && mcObj) {
                jobject gui = Java::env->GetObjectField(mcObj, RuntimeBindings::minecraft_ingameGUI);
                if (!clearJNI() && gui) {
                    if (RuntimeBindings::guiIngame_overlayPlayerList) {
                        tabOverlay = Java::env->GetObjectField(gui, RuntimeBindings::guiIngame_overlayPlayerList);
                    }
                    if (!tabOverlay && RuntimeBindings::guiIngame_getTabList) {
                        tabOverlay = Java::env->CallObjectMethod(gui, RuntimeBindings::guiIngame_getTabList);
                    }
                    clearJNI();
                    Java::env->DeleteLocalRef(gui);
                }
            }
        }
        if (!mcObj || !tabOverlay) {
            if (mcObj) Java::env->DeleteLocalRef(mcObj);
            return;
        }

        jfieldID footerField = RuntimeBindings::guiPlayerTabOverlay_footer;
        if (!footerField) {
            footerField = runtime_resolver::findChatField(tabOverlay);
            RuntimeBindings::guiPlayerTabOverlay_footer = footerField;
        }
        if (!footerField) {
            Java::env->DeleteLocalRef(tabOverlay);
            Java::env->DeleteLocalRef(mcObj);
            return;
        }

        jobject current = Java::env->GetObjectField(tabOverlay, footerField);
        clearJNI();
        std::string currentText;
        if (current && RuntimeBindings::ichatcomponent_getFormattedText) {
            jstring s = (jstring)Java::env->CallObjectMethod(current, RuntimeBindings::ichatcomponent_getFormattedText);
            if (!clearJNI() && s) {
                const char* p = Java::env->GetStringUTFChars(s, nullptr);
                currentText = p ? p : "";
                if (p) Java::env->ReleaseStringUTFChars(s, p);
                Java::env->DeleteLocalRef(s);
            }
        }
        if (currentText.find(s_watermarkText) == std::string::npos)
            s_lastServerFooter = currentText;
        if (current) Java::env->DeleteLocalRef(current);

        std::string kept;
        std::stringstream ss(s_lastServerFooter);
        std::string line;
        while (std::getline(ss, line)) {
            std::string clean;
            stripSectionAndLower(line, clean);
            if (hasMatchingKeyword(clean)) continue;
            if (!kept.empty()) kept += "\n";
            kept += line;
        }
        std::string finalText = kept;
        if (!finalText.empty()) finalText += "\n";
        finalText += s_watermarkText;

        jobject chatComp = nullptr;
        if (RuntimeBindings::chatComponentText_class && RuntimeBindings::chatComponentText_init) {
            jstring wm = Java::env->NewStringUTF(finalText.c_str());
            if (wm) {
                chatComp = Java::env->NewObject(RuntimeBindings::chatComponentText_class, RuntimeBindings::chatComponentText_init, wm);
                Java::env->DeleteLocalRef(wm);
            }
        }
        if (chatComp) {
            Java::env->SetObjectField(tabOverlay, footerField, chatComp);
            clearJNI();
            Java::env->DeleteLocalRef(chatComp);
            s_tabWatermarkActive = true;
        }
        Java::env->DeleteLocalRef(tabOverlay);
        Java::env->DeleteLocalRef(mcObj);
    }
    catch (...) {
        clearJNI();
        s_tabWatermarkActive = false;
    }
}

void TabList::ClearTabWatermarkFooter() {
    if (!s_tabWatermarkActive) return;
    if (!Java::env) return;
    if (!RuntimeBindings::minecraft_class || !RuntimeBindings::minecraft_getMinecraft) return;
    if (!RuntimeBindings::minecraft_ingameGUI) return;

    try {
        jobject mcObj = Java::env->CallStaticObjectMethod(RuntimeBindings::minecraft_class, RuntimeBindings::minecraft_getMinecraft);
        if (clearJNI() || !mcObj) return;

        jobject ingameGUI = Java::env->GetObjectField(mcObj, RuntimeBindings::minecraft_ingameGUI);
        Java::env->DeleteLocalRef(mcObj);
        if (clearJNI() || !ingameGUI) return;

        jobject tabOverlay = nullptr;
        if (RuntimeBindings::guiIngame_overlayPlayerList) {
            tabOverlay = Java::env->GetObjectField(ingameGUI, RuntimeBindings::guiIngame_overlayPlayerList);
        }
        if (!tabOverlay && RuntimeBindings::guiIngame_getTabList) {
            tabOverlay = Java::env->CallObjectMethod(ingameGUI, RuntimeBindings::guiIngame_getTabList);
        }
        clearJNI();
        Java::env->DeleteLocalRef(ingameGUI);

        if (!tabOverlay) {
            s_tabWatermarkActive = false;
            return;
        }

        jfieldID footerField = RuntimeBindings::guiPlayerTabOverlay_footer;
        if (!footerField) {
            footerField = runtime_resolver::findChatField(tabOverlay);
            RuntimeBindings::guiPlayerTabOverlay_footer = footerField;
        }

        if (footerField) {
            if (!s_lastServerFooter.empty()) {
                jobject original = nullptr;
                if (RuntimeBindings::chatComponentText_class && RuntimeBindings::chatComponentText_init) {
                    jstring txt = Java::env->NewStringUTF(s_lastServerFooter.c_str());
                    if (txt) {
                        original = Java::env->NewObject(RuntimeBindings::chatComponentText_class, RuntimeBindings::chatComponentText_init, txt);
                        Java::env->DeleteLocalRef(txt);
                    }
                }
                if (original) {
                    Java::env->SetObjectField(tabOverlay, footerField, original);
                    Java::env->DeleteLocalRef(original);
                }
            } else {
                Java::env->SetObjectField(tabOverlay, footerField, nullptr);
            }
            clearJNI();
        }
        s_lastServerFooter.clear();
        s_tabWatermarkActive = false;
        Java::env->DeleteLocalRef(tabOverlay);
    }
    catch (...) {
        clearJNI();
        s_tabWatermarkActive = false;
    }
}

void TabList::Update() {
    if (!settings::TL_Enabled) {
        if (s_injecting) ClearAllDisplayNames();
        if (s_tabWatermarkActive) ClearTabWatermarkFooter();
        return;
    }
    if (!Java::env) return;

    if (IsCMClient()) {
        CMUpdate(this);
        return;
    }

    if (!RuntimeBindings::minecraft_class || !RuntimeBindings::minecraft_getMinecraft || !RuntimeBindings::minecraft_getNetHandler) return;

    jobject mc = Java::env->CallStaticObjectMethod(RuntimeBindings::minecraft_class, RuntimeBindings::minecraft_getMinecraft);
    if (clearJNI() || !mc) return;

    if (!RuntimeBindings::minecraft_theWorld) {
        Java::env->DeleteLocalRef(mc);
        return;
    }

    jobject world = Java::env->GetObjectField(mc, RuntimeBindings::minecraft_theWorld);
    if (clearJNI() || !world) {
        if (s_injecting) ClearAllDisplayNames();
        Java::env->DeleteLocalRef(mc);
        return;
    }

    bool hasBedwars = false;
    bool hasMap = false;
    bool hasPika = false;

    if (RuntimeBindings::world_getScoreboard && RuntimeBindings::scoreboard_getObjectiveInDisplaySlot && RuntimeBindings::scoreObjective_getDisplayName) {
        jobject sb = Java::env->CallObjectMethod(world, RuntimeBindings::world_getScoreboard);
        if (!clearJNI() && sb) {
            jobject obj = Java::env->CallObjectMethod(sb, RuntimeBindings::scoreboard_getObjectiveInDisplaySlot, 1);
            if (!clearJNI() && obj) {
                jstring ds = (jstring)Java::env->CallObjectMethod(obj, RuntimeBindings::scoreObjective_getDisplayName);
                if (!clearJNI() && ds) {
                    const char* c = Java::env->GetStringUTFChars(ds, nullptr);
                    if (c) {
                        std::string dn = c; Java::env->ReleaseStringUTFChars(ds, c);
                        for (auto& x : dn) x = (char)toupper((unsigned char)x);
                        if (dn.find("BEDWARS") != std::string::npos) hasBedwars = true;
                    }
                    Java::env->DeleteLocalRef(ds);
                }
                if (hasBedwars && RuntimeBindings::scoreboard_getSortedScores && RuntimeBindings::score_getPlayerName && RuntimeBindings::scoreboard_getPlayersTeam && RuntimeBindings::scorePlayerTeam_formatPlayerName && RuntimeBindings::collection_iterator && RuntimeBindings::iterator_hasNext && RuntimeBindings::iterator_next) {
                    jobject scoresCol = Java::env->CallObjectMethod(sb, RuntimeBindings::scoreboard_getSortedScores, obj);
                    if (!clearJNI() && scoresCol) {
                        jobject it = Java::env->CallObjectMethod(scoresCol, RuntimeBindings::collection_iterator);
                        if (!clearJNI() && it) {
                            while (Java::env->CallBooleanMethod(it, RuntimeBindings::iterator_hasNext)) {
                                if (clearJNI()) break;
                                jobject scoreObj = Java::env->CallObjectMethod(it, RuntimeBindings::iterator_next);
                                if (clearJNI()) break;
                                if (scoreObj) {
                                    jstring pNameJ = (jstring)Java::env->CallObjectMethod(scoreObj, RuntimeBindings::score_getPlayerName);
                                    if (!clearJNI() && pNameJ) {
                                        jobject teamObj = Java::env->CallObjectMethod(sb, RuntimeBindings::scoreboard_getPlayersTeam, pNameJ);
                                        bool mustDeleteFormatted = false;
                                        jstring formattedNameJ = nullptr;
                                        if (!clearJNI()) {
                                            if (teamObj) {
                                                formattedNameJ = (jstring)Java::env->CallStaticObjectMethod(RuntimeBindings::scorePlayerTeam_class, RuntimeBindings::scorePlayerTeam_formatPlayerName, teamObj, pNameJ);
                                                mustDeleteFormatted = true;
                                            } else { formattedNameJ = pNameJ; }
                                            if (!clearJNI() && formattedNameJ) {
                                                const char* cStr = Java::env->GetStringUTFChars(formattedNameJ, nullptr);
                                                if (cStr) {
                                                    std::string line = cStr; Java::env->ReleaseStringUTFChars(formattedNameJ, cStr);
                                                    for (auto& x : line) x = (char)toupper((unsigned char)x);
                                                    if (line.find("MAP") != std::string::npos) hasMap = true;
                                                    if (line.find("PIKA") != std::string::npos) hasPika = true;
                                                }
                                                if (mustDeleteFormatted) Java::env->DeleteLocalRef(formattedNameJ);
                                            }
                                            if (teamObj) Java::env->DeleteLocalRef(teamObj);
                                        }
                                        Java::env->DeleteLocalRef(pNameJ);
                                    }
                                    Java::env->DeleteLocalRef(scoreObj);
                                }
                                if (hasMap) break;
                            }
                            Java::env->DeleteLocalRef(it);
                        }
                        Java::env->DeleteLocalRef(scoresCol);
                    }
                }
                Java::env->DeleteLocalRef(obj);
            }
            clearJNI();
            Java::env->DeleteLocalRef(sb);
        }
        clearJNI();
    }

    // Non-CM game state: MAP during pre-game, team names once the match starts.
    bool hasAssignedTeamColor = false;
    if (hasBedwars &&
        RuntimeBindings::scoreboard_getSortedScores &&
        RuntimeBindings::score_getPlayerName &&
        RuntimeBindings::scoreboard_getPlayersTeam &&
        RuntimeBindings::scorePlayerTeam_formatPlayerName &&
        RuntimeBindings::collection_iterator &&
        RuntimeBindings::iterator_hasNext &&
        RuntimeBindings::iterator_next) {

        jobject sbGame = Java::env->CallObjectMethod(world, RuntimeBindings::world_getScoreboard);
        if (!clearJNI() && sbGame) {
            jobject objGame = Java::env->CallObjectMethod(
                sbGame,
                RuntimeBindings::scoreboard_getObjectiveInDisplaySlot,
                1
            );

            if (!clearJNI() && objGame) {
                jobject scoresCol = Java::env->CallObjectMethod(
                    sbGame,
                    RuntimeBindings::scoreboard_getSortedScores,
                    objGame
                );

                if (!clearJNI() && scoresCol) {
                    jobject itGame = Java::env->CallObjectMethod(
                        scoresCol,
                        RuntimeBindings::collection_iterator
                    );

                    if (!clearJNI() && itGame) {
                        while (Java::env->CallBooleanMethod(itGame, RuntimeBindings::iterator_hasNext)) {
                            if (clearJNI()) break;

                            jobject scoreObj = Java::env->CallObjectMethod(
                                itGame,
                                RuntimeBindings::iterator_next
                            );
                            if (clearJNI() || !scoreObj) continue;

                            jstring pNameJ = reinterpret_cast<jstring>(
                                Java::env->CallObjectMethod(
                                    scoreObj,
                                    RuntimeBindings::score_getPlayerName
                                )
                            );

                            if (!clearJNI() && pNameJ) {
                                jobject teamObj = Java::env->CallObjectMethod(
                                    sbGame,
                                    RuntimeBindings::scoreboard_getPlayersTeam,
                                    pNameJ
                                );

                                jstring formattedJ = nullptr;
                                bool deleteFormatted = false;

                                if (!clearJNI()) {
                                    if (teamObj) {
                                        formattedJ = reinterpret_cast<jstring>(
                                            Java::env->CallStaticObjectMethod(
                                                RuntimeBindings::scorePlayerTeam_class,
                                                RuntimeBindings::scorePlayerTeam_formatPlayerName,
                                                teamObj,
                                                pNameJ
                                            )
                                        );
                                        deleteFormatted = true;
                                    } else {
                                        formattedJ = pNameJ;
                                    }

                                    if (!clearJNI() && formattedJ) {
                                        const char* chars = Java::env->GetStringUTFChars(formattedJ, nullptr);
                                        if (chars) {
                                            std::string line = chars;
                                            Java::env->ReleaseStringUTFChars(formattedJ, chars);

                                            std::string upper = line;
                                            for (char& c : upper)
                                                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

                                            // Only accept actual BedWars team labels.
                                            static const char* teamNames[] = {
                                                "RED", "BLUE", "GREEN", "YELLOW",
                                                "AQUA", "WHITE", "PINK", "GRAY",
                                                "ORANGE"
                                            };

                                            bool validTeamName = false;
                                            for (const char* teamName : teamNames) {
                                                if (upper == teamName ||
                                                    upper.find(std::string(" ") + teamName) != std::string::npos ||
                                                    upper.find(std::string(teamName) + " ") != std::string::npos) {
                                                    validTeamName = true;
                                                    break;
                                                }
                                            }

                                            const bool hasMcColor =
                                                line.find("\xC2\xA7") != std::string::npos;

                                            if (validTeamName && hasMcColor)
                                                hasAssignedTeamColor = true;
                                        }

                                        if (deleteFormatted)
                                            Java::env->DeleteLocalRef(formattedJ);
                                    }

                                    if (teamObj)
                                        Java::env->DeleteLocalRef(teamObj);
                                }

                                Java::env->DeleteLocalRef(pNameJ);
                            }

                            Java::env->DeleteLocalRef(scoreObj);

                            if (hasAssignedTeamColor)
                                break;
                        }

                        Java::env->DeleteLocalRef(itGame);
                    }

                    Java::env->DeleteLocalRef(scoresCol);
                }

                Java::env->DeleteLocalRef(objGame);
            }

            Java::env->DeleteLocalRef(sbGame);
        }

        clearJNI();
    }

    static jobject lastWorld = nullptr;
    static bool lastInGame = false;
    static bool hasFetchedForCurrentState = false;
    bool worldChanged = false;

    if (lastWorld == nullptr || !Java::env->IsSameObject(lastWorld, world)) {
        worldChanged = true;
        if (lastWorld != nullptr) { Java::env->DeleteGlobalRef(lastWorld); }
        lastWorld = Java::env->NewGlobalRef(world);
    }
    // Non-CM: MAP is pre-game, team entries are used once the match starts.
    bool inGame = hasMap || hasAssignedTeamColor;
    bool stateChanged = worldChanged || (inGame != lastInGame);
    lastInGame = inGame;

    if (stateChanged) {
        ClearAllDisplayNames();
        CMClearAllDisplayNames();
        ClearTaskQueues();
        hasFetchedForCurrentState = false;
    }
    Java::env->DeleteLocalRef(world);

    // Track the current player pool for lobby transitions.
    int realPlayerCount = 0;
    bool foundGuildBracket = false;

    jobject nhCount = Java::env->CallObjectMethod(mc, RuntimeBindings::minecraft_getNetHandler);
    if (!clearJNI() && nhCount) {
        jobject colCount = nullptr;
        if (RuntimeBindings::netHandlerPlayClient_playerInfoList && RuntimeBindings::map_values) {
            jobject map = Java::env->GetObjectField(nhCount, RuntimeBindings::netHandlerPlayClient_playerInfoList);
            if (!clearJNI() && map) { colCount = Java::env->CallObjectMethod(map, RuntimeBindings::map_values); Java::env->DeleteLocalRef(map); }
        }
        if (!colCount && RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap) {
            colCount = Java::env->CallObjectMethod(nhCount, RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap); clearJNI();
        }
        if (colCount && RuntimeBindings::collection_iterator && RuntimeBindings::iterator_hasNext && RuntimeBindings::iterator_next) {
            jobject it = Java::env->CallObjectMethod(colCount, RuntimeBindings::collection_iterator);
            if (!clearJNI() && it) {
                while (Java::env->CallBooleanMethod(it, RuntimeBindings::iterator_hasNext)) {
                    if (clearJNI()) break;
                    jobject pi = Java::env->CallObjectMethod(it, RuntimeBindings::iterator_next);
                    if (clearJNI() || !pi) continue;
                    
                    jobject gp = Java::env->CallObjectMethod(pi, RuntimeBindings::networkPlayerInfo_getGameProfile);
                    if (!clearJNI() && gp) {
                        jstring nj = (jstring)Java::env->CallObjectMethod(gp, RuntimeBindings::gameProfile_getName);
                        if (!clearJNI() && nj) {
                            std::string nm;
                            const char* c = Java::env->GetStringUTFChars(nj, nullptr);
                            if (c) { nm = c; Java::env->ReleaseStringUTFChars(nj, c); }
                            if (isValidName(nm)) {
                                bool isNpc = false;
                                std::string upperNm = nm;
                                for (auto& x : upperNm) x = (char)toupper((unsigned char)x);
                                if (upperNm.find("NPC") != std::string::npos) isNpc = true;

                                if (RuntimeBindings::networkPlayerInfo_getDisplayName && RuntimeBindings::ichatcomponent_getFormattedText) {
                                    jobject dc2 = Java::env->CallObjectMethod(pi, RuntimeBindings::networkPlayerInfo_getDisplayName);
                                    if (!clearJNI() && dc2 != nullptr) {
                                        jstring df2 = (jstring)Java::env->CallObjectMethod(dc2, RuntimeBindings::ichatcomponent_getFormattedText);
                                        if (!clearJNI() && df2 != nullptr) {
                                            const char* dcc = Java::env->GetStringUTFChars(df2, nullptr);
                                            if (dcc != nullptr) {
                                                std::string ed = dcc; Java::env->ReleaseStringUTFChars(df2, dcc);
                                                std::string upper = ed;
                                                for (auto& x : upper) x = (char)toupper((unsigned char)x);
                                                if (upper.find("NPC") != std::string::npos) isNpc = true;
                                                
                                                // Check the original display for a guild tag.
                                                size_t markerPos = ed.find(" \xC2\xA7r\xC2\xA7r ");
                                                std::string orig = (markerPos != std::string::npos) ? ed.substr(0, markerPos) : ed;
                                                if (orig.find('[') != std::string::npos) {
                                                    foundGuildBracket = true;
                                                }
                                            }
                                            Java::env->DeleteLocalRef(df2);
                                        }
                                        Java::env->DeleteLocalRef(dc2);
                                    }
                                    clearJNI();
                                }
                                
                                if (foundGuildBracket) {
                                    Java::env->DeleteLocalRef(nj);
                                    Java::env->DeleteLocalRef(gp);
                                    Java::env->DeleteLocalRef(pi);
                                    break; // SHORT CIRCUIT
                                }

                                if (!isNpc) {
                                    realPlayerCount++;
                                }
                            }
                            Java::env->DeleteLocalRef(nj);
                        }
                        Java::env->DeleteLocalRef(gp);
                    }
                    Java::env->DeleteLocalRef(pi);
                    if (foundGuildBracket) break; // Double break safety
                }
                Java::env->DeleteLocalRef(it);
            }
            clearJNI();
        }
        if (colCount) Java::env->DeleteLocalRef(colCount);
        Java::env->DeleteLocalRef(nhCount);
    }

    // Non-CM visibility is controlled here.
    bool shouldShowStats =
        hasBedwars &&
        hasPika &&
        (inGame || !settings::TL_HideInLobby);

    // Fetch only when stats are allowed to show.
    bool disableFetching = !shouldShowStats;

    static bool wasDisableFetching = false;
    if (disableFetching && !wasDisableFetching) {
        ClearTaskQueues();
    }
    wasDisableFetching = disableFetching;

    if (!shouldShowStats) {
        // Remove stale stats when entering a hidden lobby.
        ClearAllDisplayNames();
        ClearTaskQueues();
        
        if (!Base::isCompromised) {
            InjectTabWatermarkFooter();
        }
        
        Java::env->DeleteLocalRef(mc);
        return;
    }

    if (!Base::isCompromised) {
        InjectTabWatermarkFooter();
    }

    auto now = std::chrono::steady_clock::now();
    if (!hasFetchedForCurrentState) {
        if (!disableFetching) {
            jobject nh = Java::env->CallObjectMethod(mc, RuntimeBindings::minecraft_getNetHandler);
            if (!clearJNI() && nh) {
                jobject col = nullptr;
                if (RuntimeBindings::netHandlerPlayClient_playerInfoList && RuntimeBindings::map_values) {
                    jobject map = Java::env->GetObjectField(nh, RuntimeBindings::netHandlerPlayClient_playerInfoList);
                    if (!clearJNI() && map) { col = Java::env->CallObjectMethod(map, RuntimeBindings::map_values); Java::env->DeleteLocalRef(map); }
                }
                if (!col && RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap) { col = Java::env->CallObjectMethod(nh, RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap); clearJNI(); }
                if (col && RuntimeBindings::collection_iterator && RuntimeBindings::iterator_hasNext && RuntimeBindings::iterator_next) {
                    jobject it = Java::env->CallObjectMethod(col, RuntimeBindings::collection_iterator);
                    if (!clearJNI() && it) {
                        int n = 0;
                        while (n++ < 1000) {
                            if (!Java::env->CallBooleanMethod(it, RuntimeBindings::iterator_hasNext)) break;
                            if (clearJNI()) break;
                            jobject pi = Java::env->CallObjectMethod(it, RuntimeBindings::iterator_next);
                            if (clearJNI()) break;
                            if (pi) {
                                jobject gp = Java::env->CallObjectMethod(pi, RuntimeBindings::networkPlayerInfo_getGameProfile);
                                if (!clearJNI() && gp) {
                                    jstring nj = (jstring)Java::env->CallObjectMethod(gp, RuntimeBindings::gameProfile_getName);
                                    if (!clearJNI() && nj) {
                                        std::string nm;
                                        const char* c = Java::env->GetStringUTFChars(nj, nullptr);
                                        if (c) { nm = c; Java::env->ReleaseStringUTFChars(nj, c); }
                                        if (isValidName(nm)) {
                                            bool isNpc = false;
                                            std::string upperNm = nm;
                                            for (auto& x : upperNm) x = (char)toupper((unsigned char)x);
                                            if (upperNm.find("NPC") != std::string::npos) isNpc = true;

                                            if (RuntimeBindings::networkPlayerInfo_getDisplayName && RuntimeBindings::ichatcomponent_getFormattedText) {
                                                jobject dc2 = Java::env->CallObjectMethod(pi, RuntimeBindings::networkPlayerInfo_getDisplayName);
                                                if (!clearJNI() && dc2 != nullptr) {
                                                    jstring df2 = (jstring)Java::env->CallObjectMethod(dc2, RuntimeBindings::ichatcomponent_getFormattedText);
                                                    if (!clearJNI() && df2 != nullptr) {
                                                        const char* dcc = Java::env->GetStringUTFChars(df2, nullptr);
                                                        if (dcc != nullptr) {
                                                            std::string ed = dcc; Java::env->ReleaseStringUTFChars(df2, dcc);
                                                            for (auto& x : ed) x = (char)toupper((unsigned char)x);
                                                            if (ed.find("NPC") != std::string::npos) isNpc = true;
                                                        }
                                                        Java::env->DeleteLocalRef(df2);
                                                    }
                                                    Java::env->DeleteLocalRef(dc2);
                                                }
                                                clearJNI();
                                            }
                                            if (!isNpc) {
                                                bool nf = true;
                                                { 
                                                    std::lock_guard<std::mutex> lk(cacheMutex);
                                                    auto cit = nameCache.find(nm);
                                                    if (cit != nameCache.end()) {
                                                        auto diff = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::system_clock::now() - cit->second.fetchTime).count();
                                                        if (diff > 60) nameCache.erase(cit); else nf = false;
                                                    }
                                                    if (nf) {
                                                        TabListStatsData nd;
                                                        nd.isValid = true;
                                                        nd.fetchTime = std::chrono::system_clock::now();
                                                        nameCache[nm] = nd;
                                                    }
                                                }
                                                if (nf) { EnqueuePlayer(nm); }
                                            }
                                        }
                                        Java::env->DeleteLocalRef(nj);
                                    }
                                    Java::env->DeleteLocalRef(gp);
                                }
                                clearJNI();
                                Java::env->DeleteLocalRef(pi);
                            }
                        }
                        Java::env->DeleteLocalRef(it);
                    }
                    clearJNI();
                }
                if (col) Java::env->DeleteLocalRef(col);
                Java::env->DeleteLocalRef(nh);
            }
            clearJNI();
        }
        hasFetchedForCurrentState = true;
    }

    static auto li = std::chrono::steady_clock::now();
    static auto lastRescan = std::chrono::steady_clock::now();

    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - li).count() >= 100) {
        li = now;
        if (!Base::isCompromised) {
            InjectAllTabStats();
        }
    }

    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRescan).count() >= 500) {
        lastRescan = now;
        if (!disableFetching) {
            jobject nh2 = Java::env->CallObjectMethod(mc, RuntimeBindings::minecraft_getNetHandler);
            if (!clearJNI() && nh2) {
                jobject col2 = nullptr;
                if (RuntimeBindings::netHandlerPlayClient_playerInfoList && RuntimeBindings::map_values) {
                    jobject map2 = Java::env->GetObjectField(nh2, RuntimeBindings::netHandlerPlayClient_playerInfoList);
                    if (!clearJNI() && map2) { col2 = Java::env->CallObjectMethod(map2, RuntimeBindings::map_values); Java::env->DeleteLocalRef(map2); }
                }
                if (!col2 && RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap) { col2 = Java::env->CallObjectMethod(nh2, RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap); clearJNI(); }
                if (col2 && RuntimeBindings::collection_iterator && RuntimeBindings::iterator_hasNext && RuntimeBindings::iterator_next) {
                    jobject it2 = Java::env->CallObjectMethod(col2, RuntimeBindings::collection_iterator);
                    if (!clearJNI() && it2) {
                        int n2 = 0;
                        while (n2++ < 1000) {
                            if (!Java::env->CallBooleanMethod(it2, RuntimeBindings::iterator_hasNext)) break;
                            if (clearJNI()) break;
                            jobject pi2 = Java::env->CallObjectMethod(it2, RuntimeBindings::iterator_next);
                            if (clearJNI() || !pi2) continue;
                            jobject gp2 = Java::env->CallObjectMethod(pi2, RuntimeBindings::networkPlayerInfo_getGameProfile);
                            if (!clearJNI() && gp2) {
                                jstring nj2 = (jstring)Java::env->CallObjectMethod(gp2, RuntimeBindings::gameProfile_getName);
                                if (!clearJNI() && nj2) {
                                    std::string nm2;
                                    const char* c2 = Java::env->GetStringUTFChars(nj2, nullptr);
                                    if (c2) { nm2 = c2; Java::env->ReleaseStringUTFChars(nj2, c2); }
                                    if (isValidName(nm2)) {
                                        bool isNpc = false;
                                        std::string upperNm = nm2;
                                        for (auto& x : upperNm) x = (char)toupper((unsigned char)x);
                                        if (upperNm.find("NPC") != std::string::npos) isNpc = true;

                                        if (RuntimeBindings::networkPlayerInfo_getDisplayName && RuntimeBindings::ichatcomponent_getFormattedText) {
                                            jobject dc2 = Java::env->CallObjectMethod(pi2, RuntimeBindings::networkPlayerInfo_getDisplayName);
                                            if (!clearJNI() && dc2 != nullptr) {
                                                jstring df2 = (jstring)Java::env->CallObjectMethod(dc2, RuntimeBindings::ichatcomponent_getFormattedText);
                                                if (!clearJNI() && df2 != nullptr) {
                                                    const char* dcc = Java::env->GetStringUTFChars(df2, nullptr);
                                                    if (dcc != nullptr) {
                                                        std::string ed = dcc; Java::env->ReleaseStringUTFChars(df2, dcc);
                                                        for (auto& x : ed) x = (char)toupper((unsigned char)x);
                                                        if (ed.find("NPC") != std::string::npos) isNpc = true;
                                                    }
                                                    Java::env->DeleteLocalRef(df2);
                                                }
                                                Java::env->DeleteLocalRef(dc2);
                                            }
                                            clearJNI();
                                        }

                                        if (!isNpc) {
                                            bool needFetch = false;
                                            { std::lock_guard<std::mutex> lk(cacheMutex);
                                                auto itc = nameCache.find(nm2);
                                                if (itc == nameCache.end()) {
                                                    TabListStatsData nd;
                                                    nd.isValid = true;
                                                    nd.fetchTime = std::chrono::system_clock::now();
                                                    nameCache[nm2] = nd;
                                                    needFetch = true;
                                                }
                                                else if (!itc->second.leaderboardFetched || !itc->second.profileFetched) {
                                                    auto diff = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - itc->second.fetchTime).count();
                                                    if (diff > 15) { itc->second.fetchTime = std::chrono::system_clock::now(); needFetch = true; }
                                                }
                                            }
                                            if (needFetch) { EnqueuePlayer(nm2); }
                                        }
                                    }
                                    Java::env->DeleteLocalRef(nj2);
                                }
                                Java::env->DeleteLocalRef(gp2);
                            }
                            clearJNI();
                            Java::env->DeleteLocalRef(pi2);
                        }
                        Java::env->DeleteLocalRef(it2);
                    }
                    clearJNI();
                }
                if (col2) Java::env->DeleteLocalRef(col2);
                Java::env->DeleteLocalRef(nh2);
            }
            clearJNI();
        }
    }

    static auto lastTitleSuppress = std::chrono::steady_clock::now();
    if (settings::AntiSpam_Enabled && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTitleSuppress).count() >= 50) {
        lastTitleSuppress = now;
        suppressTitleSpam();
    }
    Java::env->DeleteLocalRef(mc);
}

void TabList::RenderHud() {
    if (!settings::TL_Enabled) return;
    if (!Java::env) return;

    if (IsCMClient()) {
        CMRenderHud();
        return;
    }

    if (!(GetAsyncKeyState(VK_TAB) & 0x8000)) return;
    if (Menu::open) return;

    if (!RuntimeBindings::minecraft_class || !RuntimeBindings::minecraft_getMinecraft || !RuntimeBindings::minecraft_getNetHandler) return;
    if (!RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap && !RuntimeBindings::netHandlerPlayClient_playerInfoList) return;
    if (!RuntimeBindings::networkPlayerInfo_getGameProfile || !RuntimeBindings::gameProfile_class || !RuntimeBindings::gameProfile_getName) return;
    if (!RuntimeBindings::networkPlayerInfo_getResponseTime) return;
    if (!RuntimeBindings::map_values || !RuntimeBindings::collection_iterator || !RuntimeBindings::iterator_hasNext || !RuntimeBindings::iterator_next) return;

    jobject mc = Java::env->CallStaticObjectMethod(RuntimeBindings::minecraft_class, RuntimeBindings::minecraft_getMinecraft);
    if (clearJNI() || !mc) return;

    // Keep the custom HUD in sync with the same setting.
    if (!TabList::ShouldShowStats(mc)) {
        Java::env->DeleteLocalRef(mc);
        return;
    }

    jobject nh = Java::env->CallObjectMethod(mc, RuntimeBindings::minecraft_getNetHandler);
    if (clearJNI() || !nh) {
        Java::env->DeleteLocalRef(mc);
        return;
    }

    jobject col = nullptr;
    if (RuntimeBindings::netHandlerPlayClient_playerInfoList && RuntimeBindings::map_values) {
        jobject map = Java::env->GetObjectField(nh, RuntimeBindings::netHandlerPlayClient_playerInfoList);
        if (!clearJNI() && map) { col = Java::env->CallObjectMethod(map, RuntimeBindings::map_values); Java::env->DeleteLocalRef(map); }
    }
    if (!col && RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap) {
        col = Java::env->CallObjectMethod(nh, RuntimeBindings::netHandlerPlayClient_getPlayerInfoMap); clearJNI();
    }
    clearJNI();
    Java::env->DeleteLocalRef(nh);

    if (!col) {
        Java::env->DeleteLocalRef(mc);
        return;
    }

    jobject it = Java::env->CallObjectMethod(col, RuntimeBindings::collection_iterator);
    if (clearJNI() || !it) {
        Java::env->DeleteLocalRef(col);
        Java::env->DeleteLocalRef(mc);
        return;
    }

    struct PlayerRow { std::string name; int ping; std::string statsText; };
    std::vector<PlayerRow> rows;
    bool allStatsReady = true;

    const std::string MARKER = " \xC2\xA7r\xC2\xA7r ";
    int limit = 0;
    while (limit++ < 1000) {
        if (!Java::env->CallBooleanMethod(it, RuntimeBindings::iterator_hasNext)) break;
        if (clearJNI()) break;

        jobject pi = Java::env->CallObjectMethod(it, RuntimeBindings::iterator_next);
        if (clearJNI()) break;
        if (!pi) continue;

        std::string rawName;
        jobject gp = Java::env->CallObjectMethod(pi, RuntimeBindings::networkPlayerInfo_getGameProfile);
        if (!clearJNI() && gp) {
            jstring nj = (jstring)Java::env->CallObjectMethod(gp, RuntimeBindings::gameProfile_getName);
            if (!clearJNI() && nj) {
                const char* c = Java::env->GetStringUTFChars(nj, nullptr);
                if (c) { rawName = c; Java::env->ReleaseStringUTFChars(nj, c); }
                Java::env->DeleteLocalRef(nj);
            }
            Java::env->DeleteLocalRef(gp);
        }
        if (!isValidName(rawName)) { Java::env->DeleteLocalRef(pi); continue; }

        int ping = 0;
        jint pingVal = Java::env->CallIntMethod(pi, RuntimeBindings::networkPlayerInfo_getResponseTime);
        if (!clearJNI()) ping = (int)pingVal;

        TabListStatsData st; bool hs = false;
        { std::lock_guard<std::mutex> l(cacheMutex); auto f = nameCache.find(rawName); if (f != nameCache.end()) { st = f->second; hs = true; } }
        
        if (!hs || (!st.hasValidStats && (!st.leaderboardFetched || !st.profileFetched))) allStatsReady = false;
        std::string statStr;
        if (hs) statStr = BuildStatSuffix(st);

        std::string displayName = rawName;
        if (RuntimeBindings::networkPlayerInfo_getDisplayName && RuntimeBindings::ichatcomponent_class && RuntimeBindings::ichatcomponent_getFormattedText) {
            jobject dc = Java::env->CallObjectMethod(pi, RuntimeBindings::networkPlayerInfo_getDisplayName);
            if (!clearJNI() && dc != nullptr) {
                jstring df = (jstring)Java::env->CallObjectMethod(dc, RuntimeBindings::ichatcomponent_getFormattedText);
                if (!clearJNI() && df != nullptr) {
                    const char* dcc = Java::env->GetStringUTFChars(df, nullptr);
                    if (dcc) { std::string ed = dcc; Java::env->ReleaseStringUTFChars(df, dcc);
                        if (!ed.empty() && ed.find(MARKER) == std::string::npos) displayName = ed;
                        else if (ed.find(MARKER) != std::string::npos) displayName = ed.substr(0, ed.find(MARKER));
                    }
                    Java::env->DeleteLocalRef(df);
                }
                Java::env->DeleteLocalRef(dc);
            }
            clearJNI();
        } else if (RuntimeBindings::networkPlayerInfo_getPlayerTeam && RuntimeBindings::scorePlayerTeam_getColorPrefix) {
            jobject teamObj = Java::env->CallObjectMethod(pi, RuntimeBindings::networkPlayerInfo_getPlayerTeam);
            if (!clearJNI() && teamObj != nullptr) {
                jstring pStr = (jstring)Java::env->CallObjectMethod(teamObj, RuntimeBindings::scorePlayerTeam_getColorPrefix);
                if (!clearJNI() && pStr) {
                    const char* pChars = Java::env->GetStringUTFChars(pStr, nullptr);
                    if (pChars) { displayName = std::string(pChars) + rawName; Java::env->ReleaseStringUTFChars(pStr, pChars); }
                    Java::env->DeleteLocalRef(pStr);
                }
                Java::env->DeleteLocalRef(teamObj);
            }
            clearJNI();
        }

        rows.push_back({ displayName, ping, statStr });
        Java::env->DeleteLocalRef(pi);
    }
    Java::env->DeleteLocalRef(it);
    Java::env->DeleteLocalRef(col);
    Java::env->DeleteLocalRef(mc);
    clearJNI();

    if (rows.empty() || !allStatsReady) return;

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.15f, 20), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x * 0.7f, ImGui::GetIO().DisplaySize.y - 40), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));

    char overlayTitle[64]; snprintf(overlayTitle, sizeof(overlayTitle), "TabList Stats##TabListOverlay");
    ImGui::Begin(overlayTitle, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);

    ImGui::Columns(3, "tablist_cols", false);
    ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.5f);
    ImGui::SetColumnWidth(1, ImGui::GetWindowWidth() * 0.25f);
    ImGui::SetColumnWidth(2, ImGui::GetWindowWidth() * 0.25f);

    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Player");
    ImGui::NextColumn();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Ping");
    ImGui::NextColumn();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Stats");
    ImGui::NextColumn();
    ImGui::Separator();

    for (auto& row : rows) {
        ImGui::Text("%s", row.name.c_str());
        ImGui::NextColumn();

        int pingVal = row.ping;
        float pingFrac = pingVal > 200 ? 1.0f : pingVal / 200.0f;
        ImVec4 pingColor(0.0f + pingFrac, 1.0f - pingFrac, 0.0f, 1.0f);

        char pingText[16]; snprintf(pingText, sizeof(pingText), "%dms", row.ping);
        ImGui::TextColored(pingColor, "%s", pingText);
        ImGui::NextColumn();

        if (!row.statsText.empty()) {
            std::string clean = row.statsText;
            for (size_t i = 0; i < clean.size(); ) {
                if ((unsigned char)clean[i] == 0xC2 && i + 1 < clean.size() && (unsigned char)clean[i+1] == 0xA7) { clean.erase(i, 2); continue; }
                i++;
            }
            while (!clean.empty() && clean[0] == ' ') clean.erase(0, 1);
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "%s", clean.c_str());
        }
        ImGui::NextColumn();
    }

    ImGui::Columns(1);
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void TabList::RenderMenu() {
    Menu::Checkbox("Enable TabList Stats", &settings::TL_Enabled);
    if (!settings::TL_Enabled) return;
    ImGui::Separator(); ImGui::Text("Appearance");
    if (ImGui::Combo("Format Mode", &settings::TL_FormatMode, settings::TL_FormatModeList, 2)) Menu::bPendingConfigSave = true;
    Menu::Checkbox("Hide in Lobby", &settings::TL_HideInLobby);
    Menu::Checkbox("Use Threshold Colors", &settings::TL_UseThresholdColors);
    if (settings::TL_FormatMode == 1) { if (ImGui::InputText("Custom String", settings::TL_FormatString, sizeof(settings::TL_FormatString))) Menu::bPendingConfigSave = true; }
    ImGui::Separator(); ImGui::Text("API Settings");
    if (ImGui::Combo("Interval", &settings::TL_Interval, settings::TL_IntervalList, 4)) Menu::bPendingConfigSave = true;
    if (ImGui::Combo("Mode", &settings::TL_Mode, settings::TL_ModeList, 4)) Menu::bPendingConfigSave = true;
    ImGui::Separator(); ImGui::Columns(2, "tl", false);
    Menu::Checkbox("Show Level", &settings::TL_showLevel); Menu::Checkbox("Show FKDR", &settings::TL_showFkdr);
    Menu::Checkbox("Show KDR", &settings::TL_showKdr); Menu::Checkbox("Show WLR", &settings::TL_showWlr);
    Menu::Checkbox("Show Winstreak", &settings::TL_showWS); Menu::Checkbox("Show Wins", &settings::TL_showWins);
    Menu::Checkbox("Show Losses", &settings::TL_showLosses); Menu::Checkbox("Show Games", &settings::TL_showGames);
    Menu::Checkbox("Show Beds", &settings::TL_showBeds);
    Menu::Checkbox("Show Guild", &settings::TL_showGuild);
    ImGui::NextColumn();
    Menu::Checkbox("Show Final Kills", &settings::TL_showFinalKills); Menu::Checkbox("Show Final Deaths", &settings::TL_showFinalDeaths);
    Menu::Checkbox("Show Kills", &settings::TL_showKills); Menu::Checkbox("Show Deaths", &settings::TL_showDeaths);
    Menu::Checkbox("Show Arrows", &settings::TL_showArrows); Menu::Checkbox("Show Arrows Hit", &settings::TL_showArrowsHit);
    Menu::Checkbox("Show Missed", &settings::TL_showMissedShots); Menu::Checkbox("Show Bow Kills", &settings::TL_showBowKills);
    Menu::Checkbox("Show Void Kills", &settings::TL_showVoidKills); Menu::Checkbox("Show Melee Kills", &settings::TL_showMeleeKills);
    ImGui::Columns(1); ImGui::Separator();
    if (ImGui::TreeNode("Color & Threshold Customization")) {
        ImGui::Text("Base Colors");
        if (ImGui::ColorEdit3("Level Color", settings::TL_Col_Level, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true; ImGui::SameLine();
        if (ImGui::ColorEdit3("Wins Color", settings::TL_Col_Wins, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true; ImGui::SameLine();
        if (ImGui::ColorEdit3("Losses Color", settings::TL_Col_Losses, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true;
        if (ImGui::ColorEdit3("Beds Color", settings::TL_Col_Beds, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true; ImGui::SameLine();
        if (ImGui::ColorEdit3("Games Color", settings::TL_Col_Games, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true; ImGui::SameLine();
        if (ImGui::ColorEdit3("Arrows Color", settings::TL_Col_Arrows, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true;
        if (ImGui::ColorEdit3("N/A Color", settings::TL_Col_NA, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true; ImGui::SameLine();
        if (ImGui::ColorEdit3("Nicked Color", settings::TL_Col_Nicked, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true; ImGui::SameLine();
        if (ImGui::ColorEdit3("Guild Color", settings::TL_Col_Guild, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true;
        ImGui::Separator(); ImGui::Text("FKDR/KDR/WLR Color Thresholds");
        ImGui::Text("Players with ratio ABOVE these values get that color:");
        if (ImGui::InputFloat("Ratio >= ##t1", &settings::TL_Thresh_Ratio_High, 0.5f, 1.0f, "%.1f")) Menu::bPendingConfigSave = true; ImGui::SameLine(); if (ImGui::ColorEdit3("##cr1", settings::TL_Col_Ratio_High, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true;
        if (ImGui::InputFloat("Ratio >= ##t2", &settings::TL_Thresh_Ratio_Med, 0.5f, 1.0f, "%.1f")) Menu::bPendingConfigSave = true; ImGui::SameLine(); if (ImGui::ColorEdit3("##cr2", settings::TL_Col_Ratio_Med, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true;
        if (ImGui::InputFloat("Ratio >= ##t3", &settings::TL_Thresh_Ratio_Low, 0.1f, 0.5f, "%.1f")) Menu::bPendingConfigSave = true; ImGui::SameLine(); if (ImGui::ColorEdit3("##cr3", settings::TL_Col_Ratio_Low, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true;
        ImGui::Text("Below lowest threshold:"); ImGui::SameLine(); if (ImGui::ColorEdit3("##crdef", settings::TL_Col_Ratio_Def, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true;
        ImGui::Separator(); ImGui::Text("Winstreak Color Thresholds");
        ImGui::Text("Players with WS ABOVE these values get that color:");
        if (ImGui::InputFloat("WS >= ##ws1", &settings::TL_Thresh_WS_High, 5.0f, 10.0f, "%.0f")) Menu::bPendingConfigSave = true; ImGui::SameLine(); if (ImGui::ColorEdit3("##cw1", settings::TL_Col_WS_High, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true;
        if (ImGui::InputFloat("WS >= ##ws2", &settings::TL_Thresh_WS_Med, 5.0f, 10.0f, "%.0f")) Menu::bPendingConfigSave = true; ImGui::SameLine(); if (ImGui::ColorEdit3("##cw2", settings::TL_Col_WS_Med, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true;
        if (ImGui::InputFloat("WS >= ##ws3", &settings::TL_Thresh_WS_Low, 1.0f, 5.0f, "%.0f")) Menu::bPendingConfigSave = true; ImGui::SameLine(); if (ImGui::ColorEdit3("##cw3", settings::TL_Col_WS_Low, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true;
        ImGui::Text("Below lowest threshold:"); ImGui::SameLine(); if (ImGui::ColorEdit3("##cwdef", settings::TL_Col_WS_Def, ImGuiColorEditFlags_NoInputs)) Menu::bPendingConfigSave = true;
        ImGui::TreePop();
    }
    ImGui::Separator();
    ImGui::Text("Target Warning (comma separated)");
    if (ImGui::InputText("##TW", settings::TL_TargetWarningNames, sizeof(settings::TL_TargetWarningNames))) Menu::bPendingConfigSave = true;
    ImGui::Separator();
    ImGui::Text("Anti-Spam Settings");
    Menu::Checkbox("Block Sale/Title Spam", &settings::AntiSpam_Enabled);
    static char customKeywordBuf[64] = "";
    ImGui::InputText("##keyword_input", customKeywordBuf, sizeof(customKeywordBuf));
    ImGui::SameLine();
    if (ImGui::Button("Add Keyword") && customKeywordBuf[0] != '\0') {
        settings::AntiSpam_Keywords.push_back(std::string(customKeywordBuf));
        memset(customKeywordBuf, 0, sizeof(customKeywordBuf));
        Menu::bPendingConfigSave = true;
    }
    ImGui::BeginChild("##keywords_list", ImVec2(0, 100), true);
    for (int i = 0; i < (int)settings::AntiSpam_Keywords.size(); i++) {
        ImGui::Text("%s", settings::AntiSpam_Keywords[i].c_str());
        ImGui::SameLine();
        char lb[32]; snprintf(lb, 32, "X##kw%d", i);
        if (ImGui::Button(lb)) {
            settings::AntiSpam_Keywords.erase(settings::AntiSpam_Keywords.begin() + i);
            i--;
            Menu::bPendingConfigSave = true;
        }
    }
    ImGui::EndChild();
    ImGui::Separator(); if (ImGui::Button("Clear Cache")) ClearCache();
}
