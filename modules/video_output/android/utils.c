/*****************************************************************************
 * utils.c: shared code between Android vout modules.
 *****************************************************************************
 * Copyright (C) 2014-2015 VLC authors and VideoLAN
 *
 * Authors: Felix Abecassis <felix.abecassis@gmail.com>
 *          Thomas Guillem <thomas@gllm.fr>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#include "utils.h"
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <assert.h>

typedef ANativeWindow* (*ptr_ANativeWindow_fromSurface)(JNIEnv*, jobject);
typedef ANativeWindow* (*ptr_ANativeWindow_fromSurfaceTexture)(JNIEnv*, jobject);
typedef void (*ptr_ANativeWindow_release)(ANativeWindow*);

typedef int (*ptr_SurfaceTexture_attachToGLContext)
                                    (AWindowHandler *p_awh, uint32_t tex_name);
typedef int (*ptr_SurfaceTexture_updateTexImage)(AWindowHandler *p_awh,
                                    const float **pp_transform_mtx);
typedef void (*ptr_SurfaceTexture_detachFromGLContext)
                                    (AWindowHandler *p_awh, JNIEnv *p_env);

struct SurfaceTextureHandler
{
    ptr_SurfaceTexture_attachToGLContext pf_attachToGL;
    ptr_SurfaceTexture_updateTexImage pf_updateTexImage;
    ptr_SurfaceTexture_detachFromGLContext pf_detachFromGL;
};

typedef void (*ptr_ASurfaceTexture_getTransformMatrix)
                                        (ASurfaceTexture *st, float mtx[16]);
typedef ASurfaceTexture* (*ptr_ASurfaceTexture_fromSurfaceTexture)
                                        (JNIEnv* env, jobject surfacetexture);
typedef ANativeWindow* (*ptr_ASurfaceTexture_acquireANativeWindow)
                                        (ASurfaceTexture *st);
typedef jobject (*ptr_ANativeWindow_toSurface)
                                        (JNIEnv *env, ANativeWindow *window);
typedef int (*ptr_ASurfaceTexture_attachToGLContext)
                                        (ASurfaceTexture *st, uint32_t texName);
typedef int (*ptr_ASurfaceTexture_updateTexImage)(ASurfaceTexture* st);
typedef int (*ptr_ASurfaceTexture_detachFromGLContext)(ASurfaceTexture *st);
typedef void (*ptr_ASurfaceTexture_release)(ASurfaceTexture *st);

struct ASurfaceTextureAPI
{
    float   transMat[16];

    jobject surfacetexture;
    ASurfaceTexture *p_ast;

    ptr_ASurfaceTexture_updateTexImage pf_updateTexImage;
    ptr_ASurfaceTexture_fromSurfaceTexture pf_astFromst;
    ptr_ASurfaceTexture_attachToGLContext pf_attachToGL;
    ptr_ASurfaceTexture_detachFromGLContext pf_detachFromGL;
    ptr_ASurfaceTexture_getTransformMatrix pf_getTransMatrix;
    ptr_ASurfaceTexture_release pf_releaseAst;
    ptr_ASurfaceTexture_acquireANativeWindow pf_acquireAnw;
    ptr_ANativeWindow_toSurface pf_anwToSurface;
};

struct AWindowHandler
{
    JavaVM *p_jvm;
    jobject jobj;
    vout_window_t *wnd;

    struct {
        jobject jsurface;
        ANativeWindow *p_anw;
    } views[AWindow_Max];

    void *p_anw_dl;
    ptr_ANativeWindow_fromSurface pf_winFromSurface;
    ptr_ANativeWindow_release pf_winRelease;
    native_window_api_t anw_api;

    struct SurfaceTextureHandler st;
    struct ASurfaceTextureAPI ndk_ast_api;
    bool b_has_ndk_ast_api;

    struct {
        awh_events_t cb;
    } event;
    bool b_has_video_layout_listener;

    struct {
        jfloatArray jtransform_mtx_array;
        jfloat *jtransform_mtx;
    } stex;
};

static struct
{
    struct {
        jclass clazz;
        jmethodID getVideoSurface;
        jmethodID getSubtitlesSurface;
        jmethodID registerNative;
        jmethodID unregisterNative;
        jmethodID setVideoLayout;
        jmethodID attachToGLContext;
        jmethodID detachFromGLContext;
        jmethodID waitAndUpdateTexImage;
        jmethodID getSurface;
    } AWindow;
    struct {
          jclass clazz;
          jmethodID constructor;
    } SurfaceTexture;
} jfields;

/*
 * Android Surface (pre android 2.3)
 */

extern void *jni_AndroidJavaSurfaceToNativeSurface(jobject surf);
#ifndef ANDROID_SYM_S_LOCK
# define ANDROID_SYM_S_LOCK "_ZN7android7Surface4lockEPNS0_11SurfaceInfoEb"
#endif
#ifndef ANDROID_SYM_S_LOCK2
# define ANDROID_SYM_S_LOCK2 "_ZN7android7Surface4lockEPNS0_11SurfaceInfoEPNS_6RegionE"
#endif
#ifndef ANDROID_SYM_S_UNLOCK
# define ANDROID_SYM_S_UNLOCK "_ZN7android7Surface13unlockAndPostEv"
#endif
typedef void (*AndroidSurface_lock)(void *, void *, int);
typedef void (*AndroidSurface_lock2)(void *, void *, void *);
typedef void (*AndroidSurface_unlockAndPost)(void *);

typedef struct {
    void *p_dl_handle;
    void *p_surface_handle;
    AndroidSurface_lock pf_lock;
    AndroidSurface_lock2 pf_lock2;
    AndroidSurface_unlockAndPost pf_unlockAndPost;
} NativeSurface;

static inline void *
NativeSurface_Load(const char *psz_lib, NativeSurface *p_ns)
{
    void *p_lib = dlopen(psz_lib, RTLD_NOW);
    if (!p_lib)
        return NULL;

    p_ns->pf_lock = (AndroidSurface_lock)(dlsym(p_lib, ANDROID_SYM_S_LOCK));
    p_ns->pf_lock2 = (AndroidSurface_lock2)(dlsym(p_lib, ANDROID_SYM_S_LOCK2));
    p_ns->pf_unlockAndPost =
        (AndroidSurface_unlockAndPost)(dlsym(p_lib, ANDROID_SYM_S_UNLOCK));

    if ((p_ns->pf_lock || p_ns->pf_lock2) && p_ns->pf_unlockAndPost)
        return p_lib;

    dlclose(p_lib);
    return NULL;
}

static void *
NativeSurface_getHandle(JNIEnv *p_env, jobject jsurf)
{
    jclass clz;
    jfieldID fid;
    intptr_t p_surface_handle = 0;

    clz = (*p_env)->GetObjectClass(p_env, jsurf);
    if ((*p_env)->ExceptionCheck(p_env))
    {
        (*p_env)->ExceptionClear(p_env);
        return NULL;
    }
    fid = (*p_env)->GetFieldID(p_env, clz, "mSurface", "I");
    if (fid == NULL)
    {
        if ((*p_env)->ExceptionCheck(p_env))
            (*p_env)->ExceptionClear(p_env);
        fid = (*p_env)->GetFieldID(p_env, clz, "mNativeSurface", "I");
        if (fid == NULL)
        {
            if ((*p_env)->ExceptionCheck(p_env))
                (*p_env)->ExceptionClear(p_env);
        }
    }
    if (fid != NULL)
        p_surface_handle = (intptr_t)(*p_env)->GetIntField(p_env, jsurf, fid);
    (*p_env)->DeleteLocalRef(p_env, clz);

    return (void *)p_surface_handle;
}


static ANativeWindow*
NativeSurface_fromSurface(JNIEnv *p_env, jobject jsurf)
{
    void *p_surface_handle;
    NativeSurface *p_ns;

    static const char *libs[] = {
        "libsurfaceflinger_client.so",
        "libgui.so",
        "libui.so"
    };
    p_surface_handle = NativeSurface_getHandle(p_env, jsurf);
    if (!p_surface_handle)
        return NULL;
    p_ns = malloc(sizeof(NativeSurface));
    if (!p_ns)
        return NULL;
    p_ns->p_surface_handle = p_surface_handle;

    for (size_t i = 0; i < ARRAY_SIZE(libs); i++)
    {
        void *p_dl_handle = NativeSurface_Load(libs[i], p_ns);
        if (p_dl_handle)
        {
            p_ns->p_dl_handle = p_dl_handle;
            return (ANativeWindow*)p_ns;
        }
    }
    free(p_ns);
    return NULL;
}

static void
NativeSurface_release(ANativeWindow* p_anw)
{
    NativeSurface *p_ns = (NativeSurface *)p_anw;

    dlclose(p_ns->p_dl_handle);
    free(p_ns);
}

static int32_t
NativeSurface_lock(ANativeWindow *p_anw, ANativeWindow_Buffer *p_anb,
                   ARect *p_rect)
{
    (void) p_rect;
    NativeSurface *p_ns = (NativeSurface *)p_anw;
    struct {
        uint32_t    w;
        uint32_t    h;
        uint32_t    s;
        uint32_t    usage;
        uint32_t    format;
        uint32_t*   bits;
        uint32_t    reserved[2];
    } info = { 0 };

    if (p_ns->pf_lock)
        p_ns->pf_lock(p_ns->p_surface_handle, &info, 1);
    else
        p_ns->pf_lock2(p_ns->p_surface_handle, &info, NULL);

    if (!info.w || !info.h) {
        p_ns->pf_unlockAndPost(p_ns->p_surface_handle);
        return -1;
    }

    if (p_anb) {
        p_anb->bits = info.bits;
        p_anb->width = info.w;
        p_anb->height = info.h;
        p_anb->stride = info.s;
        p_anb->format = info.format;
    }
    return 0;
}

static void
NativeSurface_unlockAndPost(ANativeWindow *p_anw)
{
    NativeSurface *p_ns = (NativeSurface *)p_anw;

    p_ns->pf_unlockAndPost(p_ns->p_surface_handle);
}

static void
LoadNativeSurfaceAPI(AWindowHandler *p_awh)
{
    p_awh->pf_winFromSurface = NativeSurface_fromSurface;
    p_awh->pf_winRelease = NativeSurface_release;
    p_awh->anw_api.winLock = NativeSurface_lock;
    p_awh->anw_api.unlockAndPost = NativeSurface_unlockAndPost;
    p_awh->anw_api.setBuffersGeometry = NULL;
}

/*
 * Android ASurfaceTexture Android NDK
 */

static int
NDKSurfaceTexture_attachToGLContext(AWindowHandler *p_awh, uint32_t texName);
static int
NDKSurfaceTexture_updateTexImage(AWindowHandler *p_awh,
                                            const float **pp_transform_mtx);
static void
NDKSurfaceTexture_detachFromGLContext(AWindowHandler *p_awh, JNIEnv *p_env);

static int
JNISurfaceTexture_waitAndUpdateTexImage(AWindowHandler *p_awh,
                                            const float **pp_transform_mtx);
static void
JNISurfaceTexture_detachFromGLContext(AWindowHandler *p_awh, JNIEnv *p_env);
static int
JNISurfaceTexture_attachToGLContext(AWindowHandler *p_awh, uint32_t tex_name);

static int
LoadNDKSurfaceTextureAPI(AWindowHandler *p_awh, void *p_library, JNIEnv *p_env)
{
    p_awh->ndk_ast_api.pf_astFromst = (ptr_ASurfaceTexture_fromSurfaceTexture)
        dlsym(p_library, "ASurfaceTexture_fromSurfaceTexture");
    if (p_awh->ndk_ast_api.pf_astFromst == NULL) return VLC_EGENERIC;

    p_awh->ndk_ast_api.pf_updateTexImage = (ptr_ASurfaceTexture_updateTexImage)
        dlsym(p_library, "ASurfaceTexture_updateTexImage");
    if (p_awh->ndk_ast_api.pf_updateTexImage == NULL) return VLC_EGENERIC;

    p_awh->ndk_ast_api.pf_attachToGL = (ptr_ASurfaceTexture_attachToGLContext)
        dlsym(p_library, "ASurfaceTexture_attachToGLContext");
    if (p_awh->ndk_ast_api.pf_attachToGL == NULL) return VLC_EGENERIC;

    p_awh->ndk_ast_api.pf_detachFromGL = (ptr_ASurfaceTexture_detachFromGLContext)
        dlsym(p_library, "ASurfaceTexture_detachFromGLContext");
    if (p_awh->ndk_ast_api.pf_detachFromGL == NULL) return VLC_EGENERIC;

    p_awh->ndk_ast_api.pf_getTransMatrix = (ptr_ASurfaceTexture_getTransformMatrix)
        dlsym(p_library, "ASurfaceTexture_getTransformMatrix");
    if (p_awh->ndk_ast_api.pf_getTransMatrix == NULL) return VLC_EGENERIC;

    p_awh->ndk_ast_api.pf_releaseAst = (ptr_ASurfaceTexture_release)
        dlsym(p_library, "ASurfaceTexture_release");
    if (p_awh->ndk_ast_api.pf_releaseAst == NULL) return VLC_EGENERIC;

    p_awh->ndk_ast_api.pf_acquireAnw = (ptr_ASurfaceTexture_acquireANativeWindow)
        dlsym(p_library, "ASurfaceTexture_acquireANativeWindow");
    if (p_awh->ndk_ast_api.pf_acquireAnw == NULL) return VLC_EGENERIC;

    p_awh->ndk_ast_api.pf_anwToSurface = (ptr_ANativeWindow_toSurface)
        dlsym(p_library, "ANativeWindow_toSurface");
    if (p_awh->ndk_ast_api.pf_anwToSurface == NULL) return VLC_EGENERIC;

    jclass surfacetexture_class = (*p_env)->FindClass(p_env,
                                            "android/graphics/SurfaceTexture");
    if (!surfacetexture_class)
        return VLC_EGENERIC;

    jfields.SurfaceTexture.clazz = (*p_env)->NewGlobalRef(p_env,
                                                         surfacetexture_class);
    (*p_env)->DeleteLocalRef(p_env, surfacetexture_class);
    if (!jfields.SurfaceTexture.clazz)
        return VLC_EGENERIC;

    jfields.SurfaceTexture.constructor = (*p_env)->GetMethodID(p_env,
                               jfields.SurfaceTexture.clazz, "<init>", "(Z)V");
    if (!jfields.SurfaceTexture.constructor)
    {
        (*p_env)->DeleteGlobalRef(p_env, jfields.SurfaceTexture.clazz);
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

/*
 * Android NativeWindow (post android 2.3)
 */

static void
LoadNativeWindowAPI(AWindowHandler *p_awh, JNIEnv *p_env)
{
    void *p_library = dlopen("libandroid.so", RTLD_NOW);
    if (!p_library)
    {
        LoadNativeSurfaceAPI(p_awh);
        return;
    }

    p_awh->pf_winFromSurface = dlsym(p_library, "ANativeWindow_fromSurface");
    p_awh->pf_winRelease = dlsym(p_library, "ANativeWindow_release");
    p_awh->anw_api.winLock = dlsym(p_library, "ANativeWindow_lock");
    p_awh->anw_api.unlockAndPost = dlsym(p_library, "ANativeWindow_unlockAndPost");
    p_awh->anw_api.setBuffersGeometry = dlsym(p_library, "ANativeWindow_setBuffersGeometry");

    if (p_awh->pf_winFromSurface && p_awh->pf_winRelease
     && p_awh->anw_api.winLock && p_awh->anw_api.unlockAndPost
     && p_awh->anw_api.setBuffersGeometry)
    {
        p_awh->b_has_ndk_ast_api = !LoadNDKSurfaceTextureAPI(p_awh, p_library, p_env);
        if (p_awh->b_has_ndk_ast_api)
        {
            p_awh->st.pf_attachToGL = NDKSurfaceTexture_attachToGLContext;
            p_awh->st.pf_updateTexImage = NDKSurfaceTexture_updateTexImage;
            p_awh->st.pf_detachFromGL = NDKSurfaceTexture_detachFromGLContext;
        }
        else
        {
            p_awh->st.pf_attachToGL = JNISurfaceTexture_attachToGLContext;
            p_awh->st.pf_updateTexImage = JNISurfaceTexture_waitAndUpdateTexImage;
            p_awh->st.pf_detachFromGL = JNISurfaceTexture_detachFromGLContext;
        }

        p_awh->p_anw_dl = p_library;
    }
    else
    {
        dlclose(p_library);
        LoadNativeSurfaceAPI(p_awh);
    }
}

/*
 * Andoid JNIEnv helper
 */

static pthread_key_t jni_env_key;
static pthread_once_t jni_env_key_once = PTHREAD_ONCE_INIT;

/* This function is called when a thread attached to the Java VM is canceled or
 * exited */
static void
jni_detach_thread(void *data)
{
    JNIEnv *env = data;
    JavaVM *jvm;

    (*env)->GetJavaVM(env, &jvm);
    assert(jvm);
    (*jvm)->DetachCurrentThread(jvm);
}

static void jni_env_key_create()
{
    /* Create a TSD area and setup a destroy callback when a thread that
     * previously set the jni_env_key is canceled or exited */
    pthread_key_create(&jni_env_key, jni_detach_thread);
}

static JNIEnv *
android_getEnvCommon(vlc_object_t *p_obj, JavaVM *jvm, const char *psz_name)
{
    assert((p_obj && !jvm) || (!p_obj && jvm));

    JNIEnv *env;

    pthread_once(&jni_env_key_once, jni_env_key_create);
    env = pthread_getspecific(jni_env_key);
    if (env == NULL)
    {
        if (!jvm)
            jvm = var_InheritAddress(p_obj, "android-jvm");

        if (!jvm)
            return NULL;

        /* if GetEnv returns JNI_OK, the thread is already attached to the
         * JavaVM, so we are already in a java thread, and we don't have to
         * setup any destroy callbacks */
        if ((*jvm)->GetEnv(jvm, (void **)&env, JNI_VERSION_1_2) != JNI_OK)
        {
            /* attach the thread to the Java VM */
            JavaVMAttachArgs args;

            args.version = JNI_VERSION_1_2;
            args.name = psz_name;
            args.group = NULL;

            if ((*jvm)->AttachCurrentThread(jvm, &env, &args) != JNI_OK)
                return NULL;

            /* Set the attached env to the thread-specific data area (TSD) */
            if (pthread_setspecific(jni_env_key, env) != 0)
            {
                (*jvm)->DetachCurrentThread(jvm);
                return NULL;
            }
        }
    }

    return env;
}

JNIEnv *
android_getEnv(vlc_object_t *p_obj, const char *psz_name)
{
    return android_getEnvCommon(p_obj, NULL, psz_name);
}

static void
AndroidNativeWindow_onMouseEvent(JNIEnv*, jobject, jlong, jint, jint, jint, jint);
static void
AndroidNativeWindow_onWindowSize(JNIEnv*, jobject, jlong, jint, jint );

const JNINativeMethod jni_callbacks[] = {
    { "nativeOnMouseEvent", "(JIIII)V",
        (void *)AndroidNativeWindow_onMouseEvent },
    { "nativeOnWindowSize", "(JII)V",
        (void *)AndroidNativeWindow_onWindowSize },
};

static int
InitJNIFields(JNIEnv *env, vlc_object_t *p_obj, jobject *jobj)
{
    static vlc_mutex_t lock = VLC_STATIC_MUTEX;
    static int i_init_state = -1;
    int ret;
    jclass clazz;

    vlc_mutex_lock(&lock);

    if (i_init_state != -1)
        goto end;

#define CHECK_EXCEPTION(what, critical) do { \
    if( (*env)->ExceptionCheck(env) ) \
    { \
        msg_Err(p_obj, "%s failed", what); \
        (*env)->ExceptionClear(env); \
        if (critical) { \
            i_init_state = 0; \
            goto end; \
        } \
    } \
} while( 0 )
#define GET_METHOD(id, str, args, critical) do { \
    jfields.id = (*env)->GetMethodID(env, clazz, (str), (args)); \
    CHECK_EXCEPTION("GetMethodID("str")", critical); \
} while( 0 )

    clazz = (*env)->GetObjectClass(env, jobj);
    CHECK_EXCEPTION("AndroidNativeWindow clazz", true);
    GET_METHOD(AWindow.getVideoSurface,
               "getVideoSurface", "()Landroid/view/Surface;", true);
    GET_METHOD(AWindow.getSubtitlesSurface,
               "getSubtitlesSurface", "()Landroid/view/Surface;", true);
    GET_METHOD(AWindow.registerNative,
               "registerNative", "(J)I", true);
    GET_METHOD(AWindow.unregisterNative,
               "unregisterNative", "()V", true);
    GET_METHOD(AWindow.setVideoLayout,
               "setVideoLayout", "(IIIIII)V", true);

    GET_METHOD(AWindow.attachToGLContext,
               "SurfaceTexture_attachToGLContext", "(I)Z", true);
    GET_METHOD(AWindow.detachFromGLContext,
               "SurfaceTexture_detachFromGLContext", "()V", true);
    GET_METHOD(AWindow.waitAndUpdateTexImage,
               "SurfaceTexture_waitAndUpdateTexImage", "([F)Z",
               true);
    GET_METHOD(AWindow.getSurface,
               "SurfaceTexture_getSurface", "()Landroid/view/Surface;", true);

    if ((*env)->RegisterNatives(env, clazz, jni_callbacks, 2) < 0)
    {
        msg_Err(p_obj, "RegisterNatives failed");
        i_init_state = 0;
        goto end;
    }
    jfields.AWindow.clazz = (*env)->NewGlobalRef(env, clazz);
    (*env)->DeleteLocalRef(env, clazz);

#undef GET_METHOD
#undef CHECK_EXCEPTION

    i_init_state = 1;
end:
    ret = i_init_state == 1 ? VLC_SUCCESS : VLC_EGENERIC;
    if (ret)
        msg_Err(p_obj, "AndroidNativeWindow jni init failed" );
    vlc_mutex_unlock(&lock);
    return ret;
}

#define JNI_CALL(what, obj, method, ...) \
    (*p_env)->what(p_env, obj, jfields.method, ##__VA_ARGS__)
#define JNI_ANWCALL(what, method, ...) \
    (*p_env)->what(p_env, p_awh->jobj, jfields.AWindow.method, ##__VA_ARGS__)
#define JNI_STEXCALL(what, method, ...) \
    (*p_env)->what(p_env, p_awh->jobj, jfields.AWindow.method, ##__VA_ARGS__)

static JNIEnv*
AWindowHandler_getEnv(AWindowHandler *p_awh)
{
    return android_getEnvCommon(NULL, p_awh->p_jvm, "AWindowHandler");
}

AWindowHandler *
AWindowHandler_new(vout_window_t *wnd, awh_events_t *p_events)
{
#define AWINDOW_REGISTER_FLAGS_SUCCESS 0x1
#define AWINDOW_REGISTER_FLAGS_HAS_VIDEO_LAYOUT_LISTENER 0x2

    AWindowHandler *p_awh;
    JNIEnv *p_env;
    JavaVM *p_jvm = var_InheritAddress(wnd, "android-jvm");
    jobject jobj = var_InheritAddress(wnd, "drawable-androidwindow");

    if (!p_jvm || !jobj)
    {
        msg_Err(wnd, "libvlc_media_player options not set");
        return NULL;
    }

    p_env = android_getEnvCommon(NULL, p_jvm, "AWindowHandler");
    if (!p_env)
    {
        msg_Err(wnd, "can't get JNIEnv");
        return NULL;
    }

    if (InitJNIFields(p_env, VLC_OBJECT(wnd), jobj) != VLC_SUCCESS)
    {
        msg_Err(wnd, "InitJNIFields failed");
        return NULL;
    }
    msg_Dbg(wnd, "InitJNIFields success");

    p_awh = calloc(1, sizeof(AWindowHandler));
    if (!p_awh)
        return NULL;

    p_awh->p_jvm = p_jvm;
    p_awh->jobj = (*p_env)->NewGlobalRef(p_env, jobj);

    p_awh->wnd = wnd;
    p_awh->event.cb = *p_events;

    jfloatArray jarray = (*p_env)->NewFloatArray(p_env, 16);
    if ((*p_env)->ExceptionCheck(p_env))
    {
        (*p_env)->ExceptionClear(p_env);
        free(p_awh);
        return NULL;
    }
    p_awh->stex.jtransform_mtx_array = (*p_env)->NewGlobalRef(p_env, jarray);
    (*p_env)->DeleteLocalRef(p_env, jarray);
    p_awh->stex.jtransform_mtx = NULL;

    const jint flags = JNI_ANWCALL(CallIntMethod, registerNative,
                                   (jlong)(intptr_t)p_awh);
    if ((flags & AWINDOW_REGISTER_FLAGS_SUCCESS) == 0)
    {
        msg_Err(wnd, "AWindow already registered");
        (*p_env)->DeleteGlobalRef(p_env, p_awh->jobj);
        (*p_env)->DeleteGlobalRef(p_env, p_awh->stex.jtransform_mtx_array);
        free(p_awh);
        return NULL;
    }
    LoadNativeWindowAPI(p_awh, p_env);

    p_awh->b_has_video_layout_listener =
        flags & AWINDOW_REGISTER_FLAGS_HAS_VIDEO_LAYOUT_LISTENER;

    if (p_awh->b_has_video_layout_listener)
    {
        /* XXX: HACK: force mediacodec to setup an OpenGL surface when the vout
         * is forced to gles2. Indeed, setting b_has_video_layout_listener to
         * false will result in mediacodec using the AWindow_SurfaceTexture
         * surface.
         */
        char *vout_modules = var_InheritString(wnd, "vout");
        if (vout_modules
         && (strncmp(vout_modules, "gles2", sizeof("gles2") - 1) == 0
          || strncmp(vout_modules, "opengles2", sizeof("opengles2") - 1) == 0))
            p_awh->b_has_video_layout_listener = false;
        free(vout_modules);
    }

    return p_awh;
}

static void
AWindowHandler_releaseANativeWindowEnv(AWindowHandler *p_awh, JNIEnv *p_env,
                                       enum AWindow_ID id)
{
    assert(id < AWindow_Max);

    if (p_awh->views[id].p_anw)
    {
        p_awh->pf_winRelease(p_awh->views[id].p_anw);
        p_awh->views[id].p_anw = NULL;
    }

    if (p_awh->views[id].jsurface)
    {
        (*p_env)->DeleteGlobalRef(p_env, p_awh->views[id].jsurface);
        p_awh->views[id].jsurface = NULL;
    }
}

void
AWindowHandler_destroy(AWindowHandler *p_awh)
{
    JNIEnv *p_env = AWindowHandler_getEnv(p_awh);

    if (p_env)
    {
        if (p_awh->b_has_ndk_ast_api)
        {
            p_awh->ndk_ast_api.pf_releaseAst(p_awh->ndk_ast_api.p_ast);
            (*p_env)->DeleteGlobalRef(p_env, p_awh->ndk_ast_api.surfacetexture);
            (*p_env)->DeleteGlobalRef(p_env, jfields.SurfaceTexture.clazz);
        }

        JNI_ANWCALL(CallVoidMethod, unregisterNative);
        AWindowHandler_releaseANativeWindowEnv(p_awh, p_env, AWindow_Video);
        AWindowHandler_releaseANativeWindowEnv(p_awh, p_env, AWindow_Subtitles);
        (*p_env)->DeleteGlobalRef(p_env, p_awh->jobj);
    }

    if (p_awh->p_anw_dl)
        dlclose(p_awh->p_anw_dl);

    (*p_env)->DeleteGlobalRef(p_env, p_awh->stex.jtransform_mtx_array);
    free(p_awh);
}

native_window_api_t *
AWindowHandler_getANativeWindowAPI(AWindowHandler *p_awh)
{
    return &p_awh->anw_api;
}

static jobject InitNDKSurfaceTexture(AWindowHandler *p_awh, JNIEnv *p_env,
        enum AWindow_ID id)
{
    jobject surfacetexture = (*p_env)->NewObject(p_env,
      jfields.SurfaceTexture.clazz, jfields.SurfaceTexture.constructor, false);

    if (surfacetexture == NULL)
        goto error;

    p_awh->ndk_ast_api.surfacetexture = (*p_env)->NewGlobalRef(p_env,
                                                             surfacetexture);

    p_awh->ndk_ast_api.p_ast = p_awh->ndk_ast_api.pf_astFromst(p_env,
            p_awh->ndk_ast_api.surfacetexture);
    if (p_awh->ndk_ast_api.p_ast == NULL)
        goto error;

    p_awh->views[id].p_anw = p_awh->ndk_ast_api.pf_acquireAnw(p_awh->ndk_ast_api.p_ast);
    if (p_awh->views[id].p_anw == NULL)
        goto error;

    jobject jsurface = p_awh->ndk_ast_api.pf_anwToSurface(p_env, p_awh->views[id].p_anw);
    if (jsurface == NULL)
        goto error;
    (*p_env)->DeleteLocalRef(p_env, surfacetexture);
    return jsurface;

error:
    if (surfacetexture == NULL)
        return NULL;
    (*p_env)->DeleteLocalRef(p_env, surfacetexture);
    (*p_env)->DeleteGlobalRef(p_env, p_awh->ndk_ast_api.surfacetexture);
    if (p_awh->ndk_ast_api.p_ast == NULL)
        return NULL;
    p_awh->ndk_ast_api.pf_releaseAst(p_awh->ndk_ast_api.p_ast);
    if (p_awh->views[id].p_anw == NULL)
        return NULL;
    AWindowHandler_releaseANativeWindow(p_awh, id);
    return NULL;
}

static int
WindowHandler_NewSurfaceEnv(AWindowHandler *p_awh, JNIEnv *p_env,
                            enum AWindow_ID id)
{
    jobject jsurface;

    switch (id)
    {
        case AWindow_Video:
            jsurface = JNI_ANWCALL(CallObjectMethod, getVideoSurface);
            break;
        case AWindow_Subtitles:
            jsurface = JNI_ANWCALL(CallObjectMethod, getSubtitlesSurface);
            break;
        case AWindow_SurfaceTexture:
        {
            if (p_awh->b_has_ndk_ast_api)
                jsurface = InitNDKSurfaceTexture(p_awh, p_env, id);
            else
                jsurface = JNI_STEXCALL(CallObjectMethod, getSurface);
            break;
        }
        default:
            vlc_assert_unreachable();
    }
    if (!jsurface)
        return VLC_EGENERIC;

    p_awh->views[id].jsurface = (*p_env)->NewGlobalRef(p_env, jsurface);
    (*p_env)->DeleteLocalRef(p_env, jsurface);
    return VLC_SUCCESS;
}

ANativeWindow *
AWindowHandler_getANativeWindow(AWindowHandler *p_awh, enum AWindow_ID id)
{
    assert(id < AWindow_Max);

    JNIEnv *p_env;

    if (p_awh->views[id].p_anw)
        return p_awh->views[id].p_anw;

    p_env = AWindowHandler_getEnv(p_awh);
    if (!p_env)
        return NULL;

    if (WindowHandler_NewSurfaceEnv(p_awh, p_env, id) != VLC_SUCCESS)
        return NULL;
    assert(p_awh->views[id].jsurface != NULL);

    if (!p_awh->views[id].p_anw)
        p_awh->views[id].p_anw = p_awh->pf_winFromSurface(p_env,
                                                    p_awh->views[id].jsurface);
    else
        assert(p_awh->b_has_ndk_ast_api && id == AWindow_SurfaceTexture);

    return p_awh->views[id].p_anw;
}

jobject
AWindowHandler_getSurface(AWindowHandler *p_awh, enum AWindow_ID id)
{
    assert(id < AWindow_Max);

    if (p_awh->views[id].jsurface)
        return p_awh->views[id].jsurface;

    AWindowHandler_getANativeWindow(p_awh, id);
    return p_awh->views[id].jsurface;
}


void AWindowHandler_releaseANativeWindow(AWindowHandler *p_awh,
                                         enum AWindow_ID id)
{
    JNIEnv *p_env = AWindowHandler_getEnv(p_awh);
    if (p_env)
        AWindowHandler_releaseANativeWindowEnv(p_awh, p_env, id);
}

static inline AWindowHandler *jlong_AWindowHandler(jlong handle)
{
    return (AWindowHandler *)(intptr_t) handle;
}

static void
AndroidNativeWindow_onMouseEvent(JNIEnv* env, jobject clazz, jlong handle,
                                 jint action, jint button, jint x, jint y)
{
    (void) env; (void) clazz;
    AWindowHandler *p_awh = jlong_AWindowHandler(handle);

    p_awh->event.cb.on_new_mouse_coords(p_awh->wnd,
        & (struct awh_mouse_coords) { action, button, x, y });
}

static void
AndroidNativeWindow_onWindowSize(JNIEnv* env, jobject clazz, jlong handle,
                                 jint width, jint height)
{
    (void) env; (void) clazz;
    AWindowHandler *p_awh = jlong_AWindowHandler(handle);

    if (width >= 0 && height >= 0)
        p_awh->event.cb.on_new_window_size(p_awh->wnd, width, height);
}

bool
AWindowHandler_canSetVideoLayout(AWindowHandler *p_awh)
{
    return p_awh->b_has_video_layout_listener;
}

int
AWindowHandler_setVideoLayout(AWindowHandler *p_awh,
                              int i_width, int i_height,
                              int i_visible_width, int i_visible_height,
                              int i_sar_num, int i_sar_den)
{
    assert(p_awh->b_has_video_layout_listener);
    JNIEnv *p_env = AWindowHandler_getEnv(p_awh);
    if (!p_env)
        return VLC_EGENERIC;

    JNI_ANWCALL(CallVoidMethod, setVideoLayout, i_width, i_height,
                i_visible_width,i_visible_height, i_sar_num, i_sar_den);
    return VLC_SUCCESS;
}

static int
NDKSurfaceTexture_attachToGLContext(AWindowHandler *p_awh, uint32_t texName)
{
    return p_awh->ndk_ast_api.pf_attachToGL(p_awh->ndk_ast_api.p_ast, texName);
}

static int
JNISurfaceTexture_attachToGLContext(AWindowHandler *p_awh, uint32_t tex_name)
{
    JNIEnv *p_env = android_getEnvCommon(NULL, p_awh->p_jvm, "SurfaceTexture");
    if (!p_env)
        return VLC_EGENERIC;

    return JNI_STEXCALL(CallBooleanMethod, attachToGLContext, tex_name) ?
           VLC_SUCCESS : VLC_EGENERIC;
}

int
SurfaceTexture_attachToGLContext(AWindowHandler *p_awh, uint32_t tex_name)
{
    return p_awh->st.pf_attachToGL(p_awh, tex_name);
}

static void
NDKSurfaceTexture_detachFromGLContext(AWindowHandler *p_awh, JNIEnv *p_env)
{
    (void)p_env;
    p_awh->ndk_ast_api.pf_detachFromGL(p_awh->ndk_ast_api.p_ast);
}

static void
JNISurfaceTexture_detachFromGLContext(AWindowHandler *p_awh, JNIEnv *p_env)
{
    JNI_STEXCALL(CallVoidMethod, detachFromGLContext);

    if (p_awh->stex.jtransform_mtx != NULL)
    {
        (*p_env)->ReleaseFloatArrayElements(p_env, p_awh->stex.jtransform_mtx_array,
                                            p_awh->stex.jtransform_mtx,
                                            JNI_ABORT);
        p_awh->stex.jtransform_mtx = NULL;
    }
}

void
SurfaceTexture_detachFromGLContext(AWindowHandler *p_awh)
{
    JNIEnv *p_env = android_getEnvCommon(NULL, p_awh->p_jvm, "SurfaceTexture");

    if (!p_env)
        return;

    p_awh->st.pf_detachFromGL(p_awh, p_env);
    AWindowHandler_releaseANativeWindowEnv(p_awh, p_env, AWindow_SurfaceTexture);
}

static int
NDKSurfaceTexture_updateTexImage(AWindowHandler *p_awh, const float **pp_transform_mtx)
{
    if (p_awh->ndk_ast_api.pf_updateTexImage(p_awh->ndk_ast_api.p_ast))
        return VLC_EGENERIC;

    p_awh->ndk_ast_api.pf_getTransMatrix(p_awh->ndk_ast_api.p_ast,
                                                 p_awh->ndk_ast_api.transMat);
    *pp_transform_mtx = p_awh->ndk_ast_api.transMat;
    return VLC_SUCCESS;
}

static int
JNISurfaceTexture_waitAndUpdateTexImage(AWindowHandler *p_awh,
                                            const float **pp_transform_mtx)
{
    JNIEnv *p_env = android_getEnvCommon(NULL, p_awh->p_jvm, "SurfaceTexture");
    if (!p_env)
        return VLC_EGENERIC;

    if (p_awh->stex.jtransform_mtx != NULL)
        (*p_env)->ReleaseFloatArrayElements(p_env, p_awh->stex.jtransform_mtx_array,
                                            p_awh->stex.jtransform_mtx,
                                            JNI_ABORT);

    bool ret = JNI_STEXCALL(CallBooleanMethod, waitAndUpdateTexImage,
                            p_awh->stex.jtransform_mtx_array);
    if (ret)
    {
        p_awh->stex.jtransform_mtx = (*p_env)->GetFloatArrayElements(p_env,
                                            p_awh->stex.jtransform_mtx_array, NULL);
        *pp_transform_mtx = p_awh->stex.jtransform_mtx;
        return VLC_SUCCESS;
    }
    else
    {
        p_awh->stex.jtransform_mtx = NULL;
        return VLC_EGENERIC;
    }
}

int
SurfaceTexture_updateTexImage(AWindowHandler *p_awh, const float **pp_transform_mtx)
{
    return p_awh->st.pf_updateTexImage(p_awh, pp_transform_mtx);
}
