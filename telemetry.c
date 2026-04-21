#include <jni.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define PKG_EXTERA "com.exteragram.messenger"
#define PKG_AYUGRAM "com.radolyn.ayugram"

typedef struct {
    char url[512];
    char user_id[64];
    char client_version[64];
    char plugin_version[64];
    char package_name[128];
} TelemetryData;

// --- 1. Читаем реальное имя пакета из системы ---
void get_real_package_name(char* out_pkg, size_t max_len) {
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (f) {
        fgets(out_pkg, max_len, f);
        fclose(f);
        for(int i = 0; i < max_len; i++) {
            if(out_pkg[i] == '\0') break;
            if(out_pkg[i] < 32 || out_pkg[i] > 126) out_pkg[i] = '\0';
        }
    } else {
        strcpy(out_pkg, "unknown");
    }
}

// --- 2. Функция для безопасного поиска классов Telegram через JNI ---
jclass find_app_class(JNIEnv* env, const char* class_name) {
    jclass threadClass = (*env)->FindClass(env, "java/lang/Thread");
    jmethodID currentThreadMethod = (*env)->GetStaticMethodID(env, threadClass, "currentThread", "()Ljava/lang/Thread;");
    jobject currentThread = (*env)->CallStaticObjectMethod(env, threadClass, currentThreadMethod);
    jmethodID getContextClassLoaderMethod = (*env)->GetMethodID(env, threadClass, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    jobject classLoader = (*env)->CallObjectMethod(env, currentThread, getContextClassLoaderMethod);

    jclass classLoaderClass = (*env)->FindClass(env, "java/lang/ClassLoader");
    jmethodID loadClassMethod = (*env)->GetMethodID(env, classLoaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");

    jstring jClassName = (*env)->NewStringUTF(env, class_name);
    jclass result = (jclass)(*env)->CallObjectMethod(env, classLoader, loadClassMethod, jClassName);
    
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    return result;
}

// --- 3. Фоновый поток для отправки JSON-данных ---
void* http_post_worker(void* arg) {
    TelemetryData* data = (TelemetryData*)arg;
    JavaVM* vm = NULL;
    jsize count = 0;
    if (JNI_GetCreatedJavaVMs(&vm, 1, &count) != JNI_OK || count == 0) { free(data); return NULL; }

    JNIEnv* env = NULL;
    int need_detach = 0;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) == 0) need_detach = 1;
    }

    if (env != NULL) {
        char json_payload[1024];
        snprintf(json_payload, sizeof(json_payload),
                 "{\"user_id\":\"%s\",\"client_version\":\"%s\",\"client_package\":\"%s\",\"plugin_version\":\"%s\"}",
                 data->user_id, data->client_version, data->package_name, data->plugin_version);

        jclass urlClass = (*env)->FindClass(env, "java/net/URL");
        jclass httpConnClass = (*env)->FindClass(env, "java/net/HttpURLConnection");
        jclass outputStreamClass = (*env)->FindClass(env, "java/io/OutputStream");

        if (urlClass && httpConnClass && outputStreamClass) {
            jmethodID urlInit = (*env)->GetMethodID(env, urlClass, "<init>", "(Ljava/lang/String;)V");
            jstring jUrlStr = (*env)->NewStringUTF(env, data->url);
            jobject urlObj = (*env)->NewObject(env, urlClass, urlInit, jUrlStr);

            jmethodID openConn = (*env)->GetMethodID(env, urlClass, "openConnection", "()Ljava/net/URLConnection;");
            jobject connObj = (*env)->CallObjectMethod(env, urlObj, openConn);

            jmethodID setReqMethod = (*env)->GetMethodID(env, httpConnClass, "setRequestMethod", "(Ljava/lang/String;)V");
            jstring jPost = (*env)->NewStringUTF(env, "POST");
            (*env)->CallVoidMethod(env, connObj, setReqMethod, jPost);

            jmethodID setReqProp = (*env)->GetMethodID(env, httpConnClass, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
            jstring jContentType = (*env)->NewStringUTF(env, "Content-Type");
            jstring jJson = (*env)->NewStringUTF(env, "application/json");
            (*env)->CallVoidMethod(env, connObj, setReqProp, jContentType, jJson);

            jmethodID setDoOutput = (*env)->GetMethodID(env, httpConnClass, "setDoOutput", "(Z)V");
            (*env)->CallVoidMethod(env, connObj, setDoOutput, JNI_TRUE);

            jmethodID getOutStream = (*env)->GetMethodID(env, httpConnClass, "getOutputStream", "()Ljava/io/OutputStream;");
            jobject osObj = (*env)->CallObjectMethod(env, connObj, getOutStream);

            if (!(*env)->ExceptionCheck(env) && osObj != NULL) {
                jclass stringClass = (*env)->FindClass(env, "java/lang/String");
                jmethodID getBytes = (*env)->GetMethodID(env, stringClass, "getBytes", "()[B");
                jstring jPayload = (*env)->NewStringUTF(env, json_payload);
                jbyteArray payloadBytes = (jbyteArray)(*env)->CallObjectMethod(env, jPayload, getBytes);

                jmethodID write = (*env)->GetMethodID(env, outputStreamClass, "write", "([B)V");
                (*env)->CallVoidMethod(env, osObj, write, payloadBytes);

                jmethodID close = (*env)->GetMethodID(env, outputStreamClass, "close", "()V");
                (*env)->CallVoidMethod(env, osObj, close);

                jmethodID getRespCode = (*env)->GetMethodID(env, httpConnClass, "getResponseCode", "()I");
                (*env)->CallIntMethod(env, connObj, getRespCode);
            }
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        }
        if (need_detach) (*vm)->DetachCurrentThread(vm);
    }
    free(data);
    return NULL;
}

// ==========================================================
// ЭКСПОРТИРУЕМЫЕ ФУНКЦИИ ДЛЯ ПЛАГИНА (CTYPES)
// ==========================================================

// ЭКСПОРТ 1: heycheck
__attribute__((visibility("default"))) const char* heycheck() {
    char pkg[128] = {0};
    get_real_package_name(pkg, sizeof(pkg));
    if (strcmp(pkg, PKG_EXTERA) == 0 || strcmp(pkg, PKG_AYUGRAM) == 0) return "OK";
    return "ERR1337";
}

// ЭКСПОРТ 2: collsend (теперь Python передает ТОЛЬКО URL и версию плагина)
__attribute__((visibility("default"))) void collsend(const char* url, const char* plugin_version) {
    TelemetryData* data = (TelemetryData*)malloc(sizeof(TelemetryData));
    if (!data) return;

    snprintf(data->url, sizeof(data->url), "%s", url ? url : "");
    snprintf(data->plugin_version, sizeof(data->plugin_version), "%s", plugin_version ? plugin_version : "unknown");
    get_real_package_name(data->package_name, sizeof(data->package_name));
    strcpy(data->user_id, "unknown");
    strcpy(data->client_version, "unknown");

    // Получаем JNI Env с текущего потока Питон-плагина
    JavaVM* vm = NULL;
    jsize count = 0;
    JNI_GetCreatedJavaVMs(&vm, 1, &count);
    if (vm) {
        JNIEnv* env = NULL;
        if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) == JNI_OK) {
            
            // Получаем версию клиента
            jclass buildVarsClass = find_app_class(env, "org.telegram.messenger.BuildVars");
            if (buildVarsClass) {
                jfieldID versionField = (*env)->GetStaticFieldID(env, buildVarsClass, "BUILD_VERSION_STRING", "Ljava/lang/String;");
                if (versionField) {
                    jstring jVersion = (jstring)(*env)->GetStaticObjectField(env, buildVarsClass, versionField);
                    if (jVersion) {
                        const char* v = (*env)->GetStringUTFChars(env, jVersion, NULL);
                        if (v) {
                            snprintf(data->client_version, sizeof(data->client_version), "%s", v);
                            (*env)->ReleaseStringUTFChars(env, jVersion, v);
                        }
                    }
                }
            }
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);

            // Получаем ID пользователя
            jclass userConfigClass = find_app_class(env, "org.telegram.messenger.UserConfig");
            if (userConfigClass) {
                jmethodID getInstanceMethod = (*env)->GetStaticMethodID(env, userConfigClass, "getInstance", "(I)Lorg/telegram/messenger/UserConfig;");
                if (getInstanceMethod) {
                    jobject userConfigObj = (*env)->CallStaticObjectMethod(env, userConfigClass, getInstanceMethod, 0);
                    if (userConfigObj) {
                        jmethodID getClientUserIdMethod = (*env)->GetMethodID(env, userConfigClass, "getClientUserId", "()J");
                        if (getClientUserIdMethod) {
                            jlong jUserId = (*env)->CallLongMethod(env, userConfigObj, getClientUserIdMethod);
                            snprintf(data->user_id, sizeof(data->user_id), "%lld", (long long)jUserId);
                        }
                    }
                }
            }
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        }
    }

    // Запускаем фоновую отправку
    pthread_t thread_id;
    pthread_create(&thread_id, NULL, http_post_worker, data);
    pthread_detach(thread_id);
}
