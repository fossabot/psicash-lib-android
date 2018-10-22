#include <jni.h>
#include <string>
#include <stdio.h>
#include "error.h"
#include "psicashlib/psicash.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

#define HTTP_REQUEST_FN_NAME    "makeHTTPRequest"
#define HTTP_REQUEST_FN_SIG     "(Ljava/lang/String;)Ljava/lang/String;"

static constexpr const char* kPsiCashUserAgent = "Psiphon-PsiCash-iOS"; // TODO: UPDATE FOR ANDROID

using namespace std;
using namespace psicash;

static jclass g_jClass;
static jmethodID g_makeHTTPRequestMID;
static PsiCash g_psiCash;

#define ERROR_MSG(msg)  (ErrorMsg(msg, __FILE__, __PRETTY_FUNCTION__, __LINE__).c_str())
#define WRAP_ERROR_MSG(err, msg)  (ErrorMsg(err, msg, __FILE__, __PRETTY_FUNCTION__, __LINE__).c_str())


// CheckJNIException returns false if there was no outstanding JNI exception, or returns true if
// there was, in addition to clearing it (allowing for further JNI operations).
bool CheckJNIException(JNIEnv* env) {
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe(); // writes to logcat
        env->ExceptionClear();
        return true;
    }
    return false;
}

// Note that the function returned by this is only valid as long as these arguments are valid.
// So, generally, it should only be used for the duration of a single JNI call.
MakeHTTPRequestFn GetHTTPReqFn(JNIEnv* env, jobject& this_obj) {
    MakeHTTPRequestFn http_req_fn = [env, &this_obj = this_obj](const string& params) -> string {
        json stub_result = {{"status", -1}, {"error", nullptr}, {"body", nullptr}, {"date", nullptr}};

        auto jParams = env->NewStringUTF(params.c_str());
        if (!jParams) {
            CheckJNIException(env);
            stub_result["error"] = MakeError("NewStringUTF failed").ToString();
            return stub_result.dump();
        }

        auto jResult = (jstring) env->CallObjectMethod(this_obj, g_makeHTTPRequestMID, jParams);
        if (!jResult) {
            CheckJNIException(env);
            stub_result["error"] = MakeError("CallObjectMethod failed").ToString();
            return stub_result.dump();
        }

        auto resultCString = env->GetStringUTFChars(jResult, NULL);
        if (!resultCString) {
            CheckJNIException(env);
            stub_result["error"] = MakeError("GetStringUTFChars failed").ToString();
            return stub_result.dump();
        }

        auto result = string(resultCString);
        env->ReleaseStringUTFChars(jResult, resultCString);

        return result;
    };

    return http_req_fn;
}

extern "C" JNIEXPORT jboolean
JNICALL
Java_ca_psiphon_psicashlib_PsiCashLib_NativeStaticInit(JNIEnv* env, jclass type) {
    g_jClass = reinterpret_cast<jclass>(env->NewGlobalRef(type));

    g_makeHTTPRequestMID = env->GetMethodID(g_jClass, HTTP_REQUEST_FN_NAME, HTTP_REQUEST_FN_SIG);
    if (!g_makeHTTPRequestMID) {
        CheckJNIException(env);
        return false;
    }

    return true;
}

// Returns null on success or an error message on failure.
extern "C" JNIEXPORT jstring
JNICALL
Java_ca_psiphon_psicashlib_PsiCashLib_NativeObjectInit(
        JNIEnv* env,
        jobject /*this_obj*/,
        jstring file_store_root,
        jboolean test) {
    if (file_store_root == nullptr) {
        return env->NewStringUTF(ERROR_MSG("file_store_root is null"));
    }

    auto file_store_root_str = env->GetStringUTFChars(file_store_root, NULL);

    if (file_store_root_str == nullptr) {
        return env->NewStringUTF(ERROR_MSG("file_store_root_str is null"));
    }

    // We can't set the HTTP requester function yet, as we can't cache `this_obj`.
    auto err = g_psiCash.Init(kPsiCashUserAgent, file_store_root_str, nullptr, test);

    env->ReleaseStringUTFChars(file_store_root, file_store_root_str);

    if (err) {
        return env->NewStringUTF(WRAP_ERROR_MSG(err, "g_psiCash.Init failed"));
    }

    return nullptr;
}

/*
 * Response JSON structure is:
 * {
 *      status: Status value,
 *      error: "message if status==Status::Invalid",
 *      purchase: Purchase; invalid if not success
 * }
 */
extern "C" JNIEXPORT jstring
JNICALL
Java_ca_psiphon_psicashlib_PsiCashLib_NewExpiringPurchase(
        JNIEnv* env,
        jobject this_obj,
        jstring j_params_json) {
    auto output = json::object({{"status",   Status::Invalid},
                                {"error",    nullptr},
                                {"purchase", nullptr}});

    if (!j_params_json) {
        output["error"] = MakeError("j_params_json is null").ToString();
        return env->NewStringUTF(output.dump().c_str());
    }

    auto c_params_json = env->GetStringUTFChars(j_params_json, NULL);
    if (!c_params_json) {
        output["error"] = MakeError("GetStringUTFChars failed").ToString();
        return env->NewStringUTF(output.dump().c_str());
    }

    auto params_json = string(c_params_json);
    env->ReleaseStringUTFChars(j_params_json, c_params_json);

    string transaction_class, distinguisher;
    int64_t expected_price;
    try {
        auto j = json::parse(params_json);
        transaction_class = j["class"].get<string>();
        distinguisher = j["distinguisher"].get<string>();
        expected_price = j["expectedPrice"].get<int64_t>();
    }
    catch (json::exception& e) {
        output["error"] = MakeError(
                utils::Stringer("params json parse failed: ", e.what(), "; id:", e.id)).ToString();
        return env->NewStringUTF(output.dump().c_str());
    }

    g_psiCash.SetHTTPRequestFn(GetHTTPReqFn(env, this_obj));

    auto result = g_psiCash.NewExpiringPurchase(transaction_class, distinguisher, expected_price);

    if (!result) {
        output["error"] = WrapError(result.error(),
                                    "g_psiCash.NewExpiringPurchase failed").ToString();
        return env->NewStringUTF(output.dump().c_str());
    }

    output["status"] = result->status;
    if (result->purchase) {
        output["purchase"] = *result->purchase;
    }

    return env->NewStringUTF(output.dump().c_str());
}

