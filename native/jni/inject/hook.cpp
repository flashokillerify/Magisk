#include <dlfcn.h>
#include <xhook.h>
#include <utils.hpp>
#include <flags.hpp>
#include <daemon.hpp>

#include "inject.hpp"
#include "memory.hpp"

using namespace std;
using jni_hook::hash_map;
using jni_hook::tree_map;
using xstring = jni_hook::string;

struct SpecializeAppProcessArgs {
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jint &mount_external;
    jstring &se_info;
    jstring &nice_name;
    jstring &instruction_set;
    jstring &app_data_dir;

    /* Optional */
    jboolean *is_child_zygote = nullptr;
    jboolean *is_top_app = nullptr;
    jobjectArray *pkg_data_info_list = nullptr;
    jobjectArray *whitelisted_data_info_list = nullptr;
    jboolean *mount_data_dirs = nullptr;
    jboolean *mount_storage_dirs = nullptr;

    SpecializeAppProcessArgs(
            jint &uid, jint &gid, jintArray &gids, jint &runtime_flags,
            jint &mount_external, jstring &se_info, jstring &nice_name,
            jstring &instruction_set, jstring &app_data_dir) :
            uid(uid), gid(gid), gids(gids), runtime_flags(runtime_flags),
            mount_external(mount_external), se_info(se_info), nice_name(nice_name),
            instruction_set(instruction_set), app_data_dir(app_data_dir) {}
};

struct ForkSystemServerArgs {
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jlong &permitted_capabilities;
    jlong &effective_capabilities;

    ForkSystemServerArgs(
            jint &uid, jint &gid, jintArray &gids, jint &runtime_flags,
            jlong &permitted_capabilities, jlong &effective_capabilities) :
            uid(uid), gid(gid), gids(gids), runtime_flags(runtime_flags),
            permitted_capabilities(permitted_capabilities),
            effective_capabilities(effective_capabilities) {}
};

struct HookContext {
    int pid;
    bool do_hide;
    union {
        SpecializeAppProcessArgs *args;
        ForkSystemServerArgs *server_args;
        void *raw_args;
    };
};
struct vtable_t;

static vector<tuple<const char *, const char *, void **>> *xhook_list;
static vector<JNINativeMethod> *jni_hook_list;
static hash_map<xstring, tree_map<xstring, tree_map<xstring, void *>>> *jni_method_map;

static HookContext *current_ctx;
static JavaVM *g_jvm;
static vtable_t *gAppRuntimeVTable;
static const JNINativeInterface *old_functions;
static JNINativeInterface *new_functions;

#define DCL_HOOK_FUNC(ret, func, ...) \
    static ret (*old_##func)(__VA_ARGS__); \
    static ret new_##func(__VA_ARGS__)

#define DCL_JNI_FUNC(name) \
    static void *name##_orig; \
    extern const JNINativeMethod name##_methods[];       \
    extern const int name##_methods_num;

namespace {
// JNI method declarations
DCL_JNI_FUNC(nativeForkAndSpecialize)
DCL_JNI_FUNC(nativeSpecializeAppProcess)
DCL_JNI_FUNC(nativeForkSystemServer)
}

#define HOOK_JNI(method) \
if (methods[i].name == #method##sv) { \
    jni_hook_list->push_back(methods[i]);              \
    method##_orig = methods[i].fnPtr; \
    for (int j = 0; j < method##_methods_num; ++j) {   \
        if (strcmp(methods[i].signature, method##_methods[j].signature) == 0) { \
            newMethods[i] = method##_methods[j];       \
            LOGI("hook: replaced #" #method "\n");     \
            ++hooked;    \
            break;       \
        }                \
    }                    \
    continue;            \
}

static unique_ptr<JNINativeMethod[]> hookAndSaveJNIMethods(
        JNIEnv *env, const char *className, const JNINativeMethod *methods, int numMethods) {
    if (g_jvm == nullptr) {
        // Save for later unhooking
        env->GetJavaVM(&g_jvm);
    }

    unique_ptr<JNINativeMethod[]> newMethods;
    int hooked = numeric_limits<int>::max();
    if (className == "com/android/internal/os/Zygote"sv) {
        hooked = 0;
        newMethods = make_unique<JNINativeMethod[]>(numMethods);
        memcpy(newMethods.get(), methods, sizeof(JNINativeMethod) * numMethods);
    }

    auto &class_map = (*jni_method_map)[className];
    for (int i = 0; i < numMethods; ++i) {
        class_map[methods[i].name][methods[i].signature] = methods[i].fnPtr;
        if (hooked < 3) {
            HOOK_JNI(nativeForkAndSpecialize);
            HOOK_JNI(nativeSpecializeAppProcess);
            HOOK_JNI(nativeForkSystemServer);
        }
    }
    return newMethods;
}

static jclass gClassRef;
static jmethodID class_getName;
static string get_class_name(JNIEnv *env, jclass clazz) {
    if (!gClassRef) {
        jclass cls = env->FindClass("java/lang/Class");
        gClassRef = (jclass) env->NewGlobalRef(cls);
        env->DeleteLocalRef(cls);
        class_getName = env->GetMethodID(gClassRef, "getName", "()Ljava/lang/String;");
    }
    auto nameRef = (jstring) env->CallObjectMethod(clazz, class_getName);
    const char *name = env->GetStringUTFChars(nameRef, nullptr);
    string className(name);
    env->ReleaseStringUTFChars(nameRef, name);
    std::replace(className.begin(), className.end(), '.', '/');
    return className;
}

// -----------------------------------------------------------------

static jint new_env_RegisterNatives(
        JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint numMethods) {
    auto className = get_class_name(env, clazz);
    LOGD("hook: JNIEnv->RegisterNatives %s\n", className.data());
    auto newMethods = hookAndSaveJNIMethods(env, className.data(), methods, numMethods);
    return old_functions->RegisterNatives(env, clazz, newMethods.get() ?: methods, numMethods);
}

DCL_HOOK_FUNC(int, jniRegisterNativeMethods,
        JNIEnv *env, const char *className, const JNINativeMethod *methods, int numMethods) {
    LOGD("hook: jniRegisterNativeMethods %s\n", className);
    auto newMethods = hookAndSaveJNIMethods(env, className, methods, numMethods);
    return old_jniRegisterNativeMethods(env, className, newMethods.get() ?: methods, numMethods);
}

DCL_HOOK_FUNC(int, fork) {
    return current_ctx ? current_ctx->pid : old_fork();
}

DCL_HOOK_FUNC(int, selinux_android_setcontext,
        uid_t uid, int isSystemServer, const char *seinfo, const char *pkgname) {
    if (current_ctx && current_ctx->do_hide) {
        // Ask magiskd to hide ourselves before switching context
        // because magiskd socket is not accessible on Android 8.0+
        remote_request_hide();
        LOGD("hook: process successfully hidden\n");
    }
    return old_selinux_android_setcontext(uid, isSystemServer, seinfo, pkgname);
}

// -----------------------------------------------------------------

// android::AndroidRuntime vtable layout
struct vtable_t {
    void *rtti;
    void *dtor;
    void (*onVmCreated)(void *self, JNIEnv* env);
    void (*onStarted)(void *self);
    void (*onZygoteInit)(void *self);
    void (*onExit)(void *self, int code);
};

// This method is a trampoline for hooking JNIEnv->RegisterNatives
DCL_HOOK_FUNC(void, onVmCreated, void *self, JNIEnv *env) {
    LOGD("hook: AppRuntime::onVmCreated\n");

    // Restore the virtual table, we no longer need it
    auto *new_table = *reinterpret_cast<vtable_t**>(self);
    *reinterpret_cast<vtable_t**>(self) = gAppRuntimeVTable;
    delete new_table;

    // Replace the function table in JNIEnv to hook RegisterNatives
    old_functions = env->functions;
    new_functions = new JNINativeInterface();
    memcpy(new_functions, env->functions, sizeof(*new_functions));
    new_functions->RegisterNatives = new_env_RegisterNatives;
    env->functions = new_functions;
    old_onVmCreated(self, env);
}

// This method is a trampoline for swizzling android::AppRuntime vtable
static bool swizzled = false;
DCL_HOOK_FUNC(void, setArgv0, void *self, const char *argv0, bool setProcName) {
    if (swizzled) {
        old_setArgv0(self, argv0, setProcName);
        return;
    }

    LOGD("hook: AndroidRuntime::setArgv0\n");

    // Swizzle C++ vtable to hook virtual function
    gAppRuntimeVTable = *reinterpret_cast<vtable_t**>(self);
    old_onVmCreated = gAppRuntimeVTable->onVmCreated;
    auto *new_table = new vtable_t();
    memcpy(new_table, gAppRuntimeVTable, sizeof(vtable_t));
    new_table->onVmCreated = &new_onVmCreated;
    *reinterpret_cast<vtable_t**>(self) = new_table;
    swizzled = true;

    old_setArgv0(self, argv0, setProcName);
}

// -----------------------------------------------------------------

static void nativeSpecializeAppProcess_pre(HookContext *ctx, JNIEnv *env, jclass clazz) {
    current_ctx = ctx;
    const char *process = env->GetStringUTFChars(ctx->args->nice_name, nullptr);
    LOGD("hook: %s %s\n", __FUNCTION__, process);

    if (ctx->args->mount_external != 0  /* TODO: Handle MOUNT_EXTERNAL_NONE cases */
        && remote_check_hide(ctx->args->uid, process)) {
        ctx->do_hide = true;
        LOGI("hook: [%s] should be hidden\n", process);
    }

    env->ReleaseStringUTFChars(ctx->args->nice_name, process);
}

static void nativeSpecializeAppProcess_post(HookContext *ctx, JNIEnv *env, jclass clazz) {
    current_ctx = nullptr;
    LOGD("hook: %s\n", __FUNCTION__);

    if (ctx->do_hide)
        self_unload();
}

// -----------------------------------------------------------------

static int sigmask(int how, int signum) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, signum);
    return sigprocmask(how, &set, nullptr);
}

// Do our own fork before loading any 3rd party code
// First block SIGCHLD, unblock after original fork is done
#define PRE_FORK() \
    current_ctx = ctx; \
    sigmask(SIG_BLOCK, SIGCHLD); \
    ctx->pid = old_fork();       \
    if (ctx->pid != 0)           \
        return;

// Unblock SIGCHLD in case the original method didn't
#define POST_FORK() \
    current_ctx = nullptr; \
    sigmask(SIG_UNBLOCK, SIGCHLD); \
    if (ctx->pid != 0)\
        return;

static void nativeForkAndSpecialize_pre(HookContext *ctx, JNIEnv *env, jclass clazz) {
    PRE_FORK();
    nativeSpecializeAppProcess_pre(ctx, env, clazz);
}

static void nativeForkAndSpecialize_post(HookContext *ctx, JNIEnv *env, jclass clazz) {
    POST_FORK();
    nativeSpecializeAppProcess_post(ctx, env, clazz);
}

// -----------------------------------------------------------------

static void nativeForkSystemServer_pre(HookContext *ctx, JNIEnv *env, jclass clazz) {
    if (env->functions == new_functions) {
        // Restore JNIEnv
        env->functions = old_functions;
        if (gClassRef) {
            env->DeleteGlobalRef(gClassRef);
            gClassRef = nullptr;
            class_getName = nullptr;
        }
    }

    PRE_FORK();
    LOGD("hook: %s\n", __FUNCTION__);
}

static void nativeForkSystemServer_post(HookContext *ctx, JNIEnv *env, jclass clazz) {
    POST_FORK();
    LOGD("hook: %s\n", __FUNCTION__);
}

#undef PRE_FORK
#undef POST_FORK

// -----------------------------------------------------------------

static bool hook_refresh() {
    if (xhook_refresh(0) == 0) {
        xhook_clear();
        LOGI("hook: xhook success\n");
        return true;
    } else {
        LOGE("hook: xhook failed\n");
        return false;
    }
}

static int hook_register(const char *path, const char *symbol, void *new_func, void **old_func) {
    int ret = xhook_register(path, symbol, new_func, old_func);
    if (ret != 0) {
        LOGE("hook: Failed to register hook \"%s\"\n", symbol);
        return ret;
    }
    xhook_list->emplace_back(path, symbol, old_func);
    return 0;
}

template<class T>
static inline void default_new(T *&p) { p = new T(); }

#define XHOOK_REGISTER_SYM(PATH_REGEX, SYM, NAME) \
    hook_register(PATH_REGEX, SYM, (void*) new_##NAME, (void **) &old_##NAME)

#define XHOOK_REGISTER(PATH_REGEX, NAME) \
    XHOOK_REGISTER_SYM(PATH_REGEX, #NAME, NAME)

#define ANDROID_RUNTIME ".*/libandroid_runtime.so$"
#define APP_PROCESS "^/system/bin/app_process.*"

void hook_functions() {
#ifdef MAGISK_DEBUG
    xhook_enable_debug(1);
    xhook_enable_sigsegv_protection(0);
#endif
    default_new(xhook_list);
    default_new(jni_hook_list);
    default_new(jni_method_map);

    XHOOK_REGISTER(ANDROID_RUNTIME, fork);
    XHOOK_REGISTER(ANDROID_RUNTIME, selinux_android_setcontext);
    XHOOK_REGISTER(ANDROID_RUNTIME, jniRegisterNativeMethods);
    hook_refresh();

    // Remove unhooked methods
    xhook_list->erase(
            std::remove_if(xhook_list->begin(), xhook_list->end(),
            [](auto &t) { return *std::get<2>(t) == nullptr;}),
            xhook_list->end());

    if (old_jniRegisterNativeMethods == nullptr) {
        LOGD("hook: jniRegisterNativeMethods not used\n");

        // android::AndroidRuntime::setArgv0(const char *, bool)
        XHOOK_REGISTER_SYM(APP_PROCESS, "_ZN7android14AndroidRuntime8setArgv0EPKcb", setArgv0);
        hook_refresh();

        // We still need old_jniRegisterNativeMethods as other code uses it
        // android::AndroidRuntime::registerNativeMethods(_JNIEnv*, const char *, const JNINativeMethod *, int)
        constexpr char sig[] = "_ZN7android14AndroidRuntime21registerNativeMethodsEP7_JNIEnvPKcPK15JNINativeMethodi";
        *(void **) &old_jniRegisterNativeMethods = dlsym(RTLD_DEFAULT, sig);
    }
}

bool unhook_functions() {
    JNIEnv* env;
    if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
        return false;

    // Do NOT call any destructors
    operator delete(jni_method_map);
    // Directly unmap the whole memory block
    jni_hook::memory_block::release();

    // Unhook JNI methods
    if (!jni_hook_list->empty() && old_jniRegisterNativeMethods(env,
            "com/android/internal/os/Zygote",
            jni_hook_list->data(), jni_hook_list->size()) != 0) {
        LOGE("hook: Failed to register JNI hook\n");
        return false;
    }
    delete jni_hook_list;

    // Unhook xhook
    for (auto &[path, sym, old_func] : *xhook_list) {
        if (xhook_register(path, sym, *old_func, nullptr) != 0) {
            LOGE("hook: Failed to register hook \"%s\"\n", sym);
            return false;
        }
    }
    delete xhook_list;
    return hook_refresh();
}

// JNI method definitions, include all method signatures of past Android versions
#include "jni_hooks.hpp"
