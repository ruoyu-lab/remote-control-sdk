// The system clipboard (SystemClipboard) on Android: ClipboardManager through
// the Java VM the host handed the SDK (rcs_android_initialize). Text and HTML
// travel (a clip's first item, ClipData.newHtmlText); images on Android are
// content URIs of the copying application's provider, which the module
// cannot serve. A copy is concealed where it carries
// ClipDescription.EXTRA_IS_SENSITIVE (Android 13 on: the keyboard does not
// show it), and placed with it.
//
// Android 10 on lets an application read the clipboard only while it has the
// input focus: in the background no change shows (the description is null)
// and read() finds nothing. The change count is the time the clip was copied
// (ClipDescription.getTimestamp()).
//
// Files (FILES): the clip's items that name a content URI (a picture copied
// in a gallery, a document) are another application's, readable while the
// clip is this application's to read: they are copied into the files
// directory as the copy is read, by their display names, counted against
// max_bytes. A paste here of the other end's files would need a content
// provider of the host's, which the module has not: RECEIVE is not done.
#include "pastes.hpp"

#include <rcsdk/android.h>

#include <jni.h>
#include <pthread.h>

#include <cstdio>
#include <fstream>

namespace rcsdk::clipboard {
namespace {

constexpr char sensitive[] = "android.content.extra.IS_SENSITIVE";

pthread_key_t attachedKey;
const bool keyMade = [] {
    return pthread_key_create(&attachedKey, [](void* vm) { static_cast<JavaVM*>(vm)->DetachCurrentThread(); }) == 0;
}();

// The calling thread's JNIEnv and the application context.
struct Java {
    JNIEnv* env = nullptr;
    jobject context = nullptr;
};

Java java() {
    void* vm = nullptr;
    void* context = nullptr;
    if (rcs_android_get_java(&vm, &context) != RCS_OK) {
        fail(RCS_ERROR_UNSUPPORTED, "The host has not handed over its Java VM (rcs_android_initialize): the "
                                    "clipboard is Java's");
    }
    auto* machine = static_cast<JavaVM*>(vm);
    JNIEnv* env = nullptr;
    if (machine->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (machine->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            fail(RCS_ERROR_DEVICE, "Could not attach the clipboard's thread to the Java VM");
        }
        if (keyMade) {
            pthread_setspecific(attachedKey, machine);
        }
    }
    return {env, static_cast<jobject>(context)};
}

// Throws Error(DEVICE) when Java threw.
void check(JNIEnv* env, const char* what) {
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        fail(RCS_ERROR_DEVICE, std::string(what) + " failed in Java");
    }
}

// Local references made in a scope, deleted at its end.
class Frame {
public:
    explicit Frame(JNIEnv* env) : env_(env) {
        if (env_->PushLocalFrame(32) != 0) {
            env_->ExceptionClear();
            fail(RCS_ERROR_DEVICE, "Java has no room for local references");
        }
    }
    ~Frame() { env_->PopLocalFrame(nullptr); }

private:
    JNIEnv* env_;
};

Bytes utf8(JNIEnv* env, jobject charSequence) {
    if (!charSequence) {
        return {};
    }
    jclass object = env->FindClass("java/lang/Object");
    auto text = static_cast<jstring>(
        env->CallObjectMethod(charSequence, env->GetMethodID(object, "toString", "()Ljava/lang/String;")));
    check(env, "Reading the clip's text");
    // Java's own modified UTF-8 differs for supplementary characters: take the String's bytes as UTF-8.
    jclass string = env->FindClass("java/lang/String");
    auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(
        text, env->GetMethodID(string, "getBytes", "(Ljava/lang/String;)[B"), env->NewStringUTF("UTF-8")));
    check(env, "Reading the clip's text");
    Bytes result(size_t(env->GetArrayLength(bytes)));
    env->GetByteArrayRegion(bytes, 0, jsize(result.size()), reinterpret_cast<jbyte*>(result.data()));
    return result;
}

jstring javaText(JNIEnv* env, const Bytes& bytes) {
    jbyteArray array = env->NewByteArray(jsize(bytes.size()));
    env->SetByteArrayRegion(array, 0, jsize(bytes.size()), reinterpret_cast<const jbyte*>(bytes.data()));
    jclass string = env->FindClass("java/lang/String");
    auto text = static_cast<jstring>(env->NewObject(
        string, env->GetMethodID(string, "<init>", "([BLjava/lang/String;)V"), array, env->NewStringUTF("UTF-8")));
    check(env, "Making the clip's text");
    return text;
}

// A Java object's String as UTF-8 (empty for null).
std::string stringOf(JNIEnv* env, jobject text) {
    const Bytes bytes = utf8(env, text);
    return std::string(bytes.begin(), bytes.end());
}

// The files the clip's items name by URI, copied into a directory of the
// files directory while their bytes fit `left`; nullopt without any.
// `tooMany`: some did not fit, or could not be read.
std::optional<Files> copiedFiles(JNIEnv* env, jobject context, jobject clip, jclass clipType, Pastes& pastes,
                                 size_t& left, bool& tooMany) {
    const jint count = env->CallIntMethod(clip, env->GetMethodID(clipType, "getItemCount", "()I"));
    const jmethodID itemAt = env->GetMethodID(clipType, "getItemAt", "(I)Landroid/content/ClipData$Item;");
    jclass itemType = env->FindClass("android/content/ClipData$Item");
    const jmethodID uriOf = env->GetMethodID(itemType, "getUri", "()Landroid/net/Uri;");
    jclass contextType = env->FindClass("android/content/Context");
    jobject resolver = env->CallObjectMethod(
        context, env->GetMethodID(contextType, "getContentResolver", "()Landroid/content/ContentResolver;"));
    jclass resolverType = env->FindClass("android/content/ContentResolver");
    const jmethodID query = env->GetMethodID(
        resolverType, "query",
        "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)"
        "Landroid/database/Cursor;");
    const jmethodID open =
        env->GetMethodID(resolverType, "openInputStream", "(Landroid/net/Uri;)Ljava/io/InputStream;");
    jclass cursorType = env->FindClass("android/database/Cursor");
    jclass streamType = env->FindClass("java/io/InputStream");
    const jmethodID readInto = env->GetMethodID(streamType, "read", "([B)I");
    check(env, "Reading the clip's files");
    Files files;
    std::string directory;
    for (jint i = 0; i < count; ++i) {
        Frame frame(env);
        jobject uri = env->CallObjectMethod(env->CallObjectMethod(clip, itemAt, i), uriOf);
        check(env, "ClipData.Item.getUri");
        if (!uri) {
            continue;
        }
        // Its name and size (OpenableColumns), as its provider says.
        jclass stringType = env->FindClass("java/lang/String");
        jobjectArray columns = env->NewObjectArray(2, stringType, nullptr);
        env->SetObjectArrayElement(columns, 0, env->NewStringUTF("_display_name"));
        env->SetObjectArrayElement(columns, 1, env->NewStringUTF("_size"));
        jobject cursor = env->CallObjectMethod(resolver, query, uri, columns, nullptr, nullptr, nullptr);
        if (env->ExceptionCheck() || !cursor) {
            env->ExceptionClear();
            tooMany = true; // Not this application's to read.
            continue;
        }
        std::string name;
        if (env->CallBooleanMethod(cursor, env->GetMethodID(cursorType, "moveToFirst", "()Z"))) {
            name = stringOf(env, env->CallObjectMethod(cursor, env->GetMethodID(cursorType, "getString",
                                                                                 "(I)Ljava/lang/String;"),
                                                       jint(0)));
        }
        env->CallVoidMethod(cursor, env->GetMethodID(cursorType, "close", "()V"));
        check(env, "Reading a file's name");
        if (!nameProblem(name).empty()) {
            tooMany = true;
            continue;
        }
        // Copied while it fits.
        if (directory.empty()) {
            Publication local{};
            uint8_t random[32];
            rcs_generate_connection_key(random, sizeof(random));
            std::copy(random, random + local.size(), local.begin());
            directory = pastes.directoryFor(local);
        }
        const std::string path = directory + "/" + name;
        jobject in = env->CallObjectMethod(resolver, open, uri);
        if (env->ExceptionCheck() || !in) {
            env->ExceptionClear();
            tooMany = true;
            continue;
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        jbyteArray buffer = env->NewByteArray(65536);
        std::vector<char> bytes(65536);
        uint64_t total = 0;
        bool whole = bool(out);
        for (jint got = 0; whole && (got = env->CallIntMethod(in, readInto, buffer)) >= 0;) {
            if (env->ExceptionCheck() || total + uint64_t(got) > left) {
                env->ExceptionClear();
                whole = false;
                break;
            }
            env->GetByteArrayRegion(buffer, 0, got, reinterpret_cast<jbyte*>(bytes.data()));
            out.write(bytes.data(), got);
            total += uint64_t(got);
        }
        env->ExceptionClear();
        env->CallVoidMethod(in, env->GetMethodID(streamType, "close", "()V"));
        env->ExceptionClear();
        out.close();
        if (!whole || !out) {
            std::remove(path.c_str());
            tooMany = true;
            continue;
        }
        left -= size_t(total);
        files.entries.push_back({name, RCS_FILE_REGULAR, total, 0, path});
    }
    return files.entries.empty() ? std::nullopt : std::optional(std::move(files));
}

class AndroidClipboard final : public SystemClipboard {
public:
    uint64_t changeCount() override {
        const auto [env, context] = java();
        Frame frame(env);
        jobject description = env->CallObjectMethod(
            manager(env, context), method(env, "getPrimaryClipDescription", "()Landroid/content/ClipDescription;"));
        check(env, "ClipboardManager.getPrimaryClipDescription");
        if (description) { // None in the background: the last change seen stays.
            jclass type = env->FindClass("android/content/ClipDescription");
            last_ = uint64_t(env->CallLongMethod(description, env->GetMethodID(type, "getTimestamp", "()J")));
            check(env, "ClipDescription.getTimestamp");
        }
        return last_;
    }

    Content read(uint32_t formats, size_t maxBytes) override {
        const auto [env, context] = java();
        Frame frame(env);
        Content content;
        jobject clip =
            env->CallObjectMethod(manager(env, context), method(env, "getPrimaryClip", "()Landroid/content/ClipData;"));
        check(env, "ClipboardManager.getPrimaryClip");
        jclass clipType = env->FindClass("android/content/ClipData");
        if (!clip || env->CallIntMethod(clip, env->GetMethodID(clipType, "getItemCount", "()I")) < 1) {
            return content;
        }
        content.concealed = isSensitive(env, clip, clipType);
        jobject item = env->CallObjectMethod(
            clip, env->GetMethodID(clipType, "getItemAt", "(I)Landroid/content/ClipData$Item;"), jint(0));
        jclass itemType = env->FindClass("android/content/ClipData$Item");
        Bytes text =
            utf8(env, env->CallObjectMethod(item, env->GetMethodID(itemType, "getText", "()Ljava/lang/CharSequence;")));
        Bytes html =
            utf8(env, env->CallObjectMethod(item, env->GetMethodID(itemType, "getHtmlText", "()Ljava/lang/String;")));
        check(env, "Reading the clip");
        size_t left = maxBytes;
        const auto add = [&](rcs_clipboard_format format, Bytes bytes) {
            if (bytes.empty()) {
                return;
            }
            if (!(formats & (1u << format)) || bytes.size() > left) {
                content.leftOut |= 1u << format;
                return;
            }
            left -= bytes.size();
            content.items.push_back({format, std::move(bytes)});
        };
        add(RCS_CLIPBOARD_TEXT, std::move(text));
        add(RCS_CLIPBOARD_HTML, std::move(html));
        if (formats & filesFormat) {
            bool tooMany = false;
            auto files = copiedFiles(env, context, clip, clipType, *pastes_, left, tooMany);
            if (files && filesBytes(*files) <= left) {
                content.files = std::move(files);
            }
            if (tooMany || (files && !content.files)) {
                content.leftOut |= filesFormat;
            }
        }
        return content;
    }

    uint64_t write(const Content& content) override {
        const auto [env, context] = java();
        Frame frame(env);
        const Bytes* text = nullptr;
        const Bytes* html = nullptr;
        for (const auto& item : content.items) {
            if (item.format == RCS_CLIPBOARD_TEXT) {
                text = &item.bytes;
            } else if (item.format == RCS_CLIPBOARD_HTML) {
                html = &item.bytes;
            } else {
                fail(RCS_ERROR_UNSUPPORTED, "Android's clipboard takes text and HTML from the module");
            }
        }
        jclass clipType = env->FindClass("android/content/ClipData");
        jstring label = env->NewStringUTF("Remote copy");
        jobject clip = nullptr;
        if (html) {
            clip = env->CallStaticObjectMethod(
                clipType,
                env->GetStaticMethodID(clipType, "newHtmlText",
                                       "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;Ljava/lang/String;)"
                                       "Landroid/content/ClipData;"),
                label, javaText(env, text ? *text : *html), javaText(env, *html));
        } else {
            clip = env->CallStaticObjectMethod(
                clipType,
                env->GetStaticMethodID(clipType, "newPlainText",
                                       "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;"),
                label, javaText(env, text ? *text : Bytes{}));
        }
        check(env, "Making the clip");
        if (content.concealed) {
            markSensitive(env, clip, clipType);
        }
        jobject clipboard = manager(env, context);
        if (content.items.empty()) {
            env->CallVoidMethod(clipboard, method(env, "clearPrimaryClip", "()V"));
        } else {
            env->CallVoidMethod(clipboard, method(env, "setPrimaryClip", "(Landroid/content/ClipData;)V"), clip);
        }
        check(env, "ClipboardManager.setPrimaryClip");
        return changeCount();
    }

private:
    static jobject manager(JNIEnv* env, jobject context) {
        jclass contextType = env->FindClass("android/content/Context");
        jobject service = env->CallObjectMethod(
            context, env->GetMethodID(contextType, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;"),
            env->NewStringUTF("clipboard"));
        check(env, "Context.getSystemService(\"clipboard\")");
        if (!service) {
            fail(RCS_ERROR_UNSUPPORTED, "This device has no clipboard service");
        }
        return service;
    }

    static jmethodID method(JNIEnv* env, const char* name, const char* signature) {
        jclass type = env->FindClass("android/content/ClipboardManager");
        const jmethodID id = env->GetMethodID(type, name, signature);
        check(env, name);
        return id;
    }

    static jobject extras(JNIEnv* env, jobject clip, jclass clipType) {
        jobject description = env->CallObjectMethod(
            clip, env->GetMethodID(clipType, "getDescription", "()Landroid/content/ClipDescription;"));
        jclass type = env->FindClass("android/content/ClipDescription");
        return env->CallObjectMethod(description,
                                     env->GetMethodID(type, "getExtras", "()Landroid/os/PersistableBundle;"));
    }

    static bool isSensitive(JNIEnv* env, jobject clip, jclass clipType) {
        jobject bundle = extras(env, clip, clipType);
        check(env, "ClipDescription.getExtras");
        if (!bundle) {
            return false;
        }
        jclass type = env->FindClass("android/os/BaseBundle");
        const bool marked = env->CallBooleanMethod(
            bundle, env->GetMethodID(type, "getBoolean", "(Ljava/lang/String;)Z"), env->NewStringUTF(sensitive));
        check(env, "Reading EXTRA_IS_SENSITIVE");
        return marked;
    }

    static void markSensitive(JNIEnv* env, jobject clip, jclass clipType) {
        jclass bundleType = env->FindClass("android/os/PersistableBundle");
        jobject bundle = env->NewObject(bundleType, env->GetMethodID(bundleType, "<init>", "()V"));
        jclass baseType = env->FindClass("android/os/BaseBundle");
        env->CallVoidMethod(bundle, env->GetMethodID(baseType, "putBoolean", "(Ljava/lang/String;Z)V"),
                            env->NewStringUTF(sensitive), jboolean(true));
        jobject description = env->CallObjectMethod(
            clip, env->GetMethodID(clipType, "getDescription", "()Landroid/content/ClipDescription;"));
        jclass type = env->FindClass("android/content/ClipDescription");
        env->CallVoidMethod(description, env->GetMethodID(type, "setExtras", "(Landroid/os/PersistableBundle;)V"),
                            bundle);
        check(env, "Marking the clip sensitive");
    }

    uint64_t last_ = 0;
};

} // namespace

std::unique_ptr<SystemClipboard> createPlatformClipboard() {
    java(); // UNSUPPORTED before rcs_android_initialize.
    return std::make_unique<AndroidClipboard>();
}

uint32_t platformFormats() noexcept {
    return (1u << RCS_CLIPBOARD_TEXT) | (1u << RCS_CLIPBOARD_HTML);
}

bool platformCarriesFiles() noexcept {
    return true;
}

uint32_t platformFiles(std::string& why) {
    why = "A paste here of the other end's files would need a content provider of the host's: not done";
    return RCS_CLIPBOARD_SEND;
}

} // namespace rcsdk::clipboard
