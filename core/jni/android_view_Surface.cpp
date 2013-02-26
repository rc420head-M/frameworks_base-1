/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "Surface"

#include <stdio.h>

#include "jni.h"
#include "JNIHelp.h"
#include "android_os_Parcel.h"
#include "android/graphics/GraphicsJNI.h"

#include <android_runtime/AndroidRuntime.h>
#include <android_runtime/android_view_Surface.h>
#include <android_runtime/android_graphics_SurfaceTexture.h>

#include <gui/Surface.h>
#include <gui/SurfaceControl.h>
#include <gui/GLConsumer.h>

#include <ui/Rect.h>
#include <ui/Region.h>

#include <SkCanvas.h>
#include <SkBitmap.h>
#include <SkRegion.h>

#include <utils/misc.h>
#include <utils/Log.h>

#include <ScopedUtfChars.h>

// ----------------------------------------------------------------------------

namespace android {

static const char* const OutOfResourcesException =
    "android/view/Surface$OutOfResourcesException";

static struct {
    jclass clazz;
    jfieldID mNativeObject;
    jfieldID mGenerationId;
    jfieldID mCanvas;
    jmethodID ctor;
} gSurfaceClassInfo;

static struct {
    jfieldID left;
    jfieldID top;
    jfieldID right;
    jfieldID bottom;
} gRectClassInfo;

static struct {
    jfieldID mFinalizer;
    jfieldID mNativeCanvas;
    jfieldID mSurfaceFormat;
} gCanvasClassInfo;

static struct {
    jfieldID mNativeCanvas;
} gCanvasFinalizerClassInfo;

// ----------------------------------------------------------------------------

bool android_view_Surface_isInstanceOf(JNIEnv* env, jobject obj) {
    return env->IsInstanceOf(obj, gSurfaceClassInfo.clazz);
}

sp<ANativeWindow> android_view_Surface_getNativeWindow(JNIEnv* env, jobject surfaceObj) {
    return android_view_Surface_getSurface(env, surfaceObj);
}

sp<Surface> android_view_Surface_getSurface(JNIEnv* env, jobject surfaceObj) {
    return reinterpret_cast<Surface *>(
            env->GetIntField(surfaceObj, gSurfaceClassInfo.mNativeObject));
}

jobject android_view_Surface_createFromIGraphicBufferProducer(JNIEnv* env,
        const sp<IGraphicBufferProducer>& bufferProducer) {
    if (bufferProducer == NULL) {
        return NULL;
    }

    sp<Surface> surface(new Surface(bufferProducer));
    if (surface == NULL) {
        return NULL;
    }

    jobject surfaceObj = env->NewObject(gSurfaceClassInfo.clazz, gSurfaceClassInfo.ctor, surface.get());
    if (surfaceObj == NULL) {
        if (env->ExceptionCheck()) {
            ALOGE("Could not create instance of Surface from IGraphicBufferProducer.");
            LOGE_EX(env);
            env->ExceptionClear();
        }
        return NULL;
    }
    surface->incStrong(surfaceObj);
    return surfaceObj;
}

// ----------------------------------------------------------------------------

static inline bool isSurfaceValid(const sp<Surface>& sur) {
    return Surface::isValid(sur);
}

// ----------------------------------------------------------------------------

static jint nativeCreateFromSurfaceTexture(JNIEnv* env, jclass clazz,
        jobject surfaceTextureObj) {
    sp<GLConsumer> st(SurfaceTexture_getSurfaceTexture(env, surfaceTextureObj));
    if (st == NULL) {
        jniThrowException(env, "java/lang/IllegalArgumentException",
                "SurfaceTexture has already been released");
        return 0;
    }

    sp<IGraphicBufferProducer> bq = st->getBufferQueue();
    sp<Surface> surface(new Surface(bq));
    if (surface == NULL) {
        jniThrowException(env, OutOfResourcesException, NULL);
        return 0;
    }

    surface->incStrong(clazz);
    return int(surface.get());
}

static void nativeRelease(JNIEnv* env, jclass clazz, jint nativeObject) {
    sp<Surface> sur(reinterpret_cast<Surface *>(nativeObject));
    sur->decStrong(clazz);
}

static void nativeDestroy(JNIEnv* env, jclass clazz, jint nativeObject) {
    sp<Surface> sur(reinterpret_cast<Surface *>(nativeObject));
    sur->decStrong(clazz);
}

static jboolean nativeIsValid(JNIEnv* env, jclass clazz, jint nativeObject) {
    sp<Surface> sur(reinterpret_cast<Surface *>(nativeObject));
    return isSurfaceValid(sur) ? JNI_TRUE : JNI_FALSE;
}

static jboolean nativeIsConsumerRunningBehind(JNIEnv* env, jclass clazz, jint nativeObject) {
    sp<Surface> sur(reinterpret_cast<Surface *>(nativeObject));
    if (!isSurfaceValid(sur)) {
        doThrowIAE(env);
        return JNI_FALSE;
    }
    int value = 0;
    ANativeWindow* anw = static_cast<ANativeWindow*>(sur.get());
    anw->query(anw, NATIVE_WINDOW_CONSUMER_RUNNING_BEHIND, &value);
    return value;
}

static inline SkBitmap::Config convertPixelFormat(PixelFormat format) {
    /* note: if PIXEL_FORMAT_RGBX_8888 means that all alpha bytes are 0xFF, then
        we can map to SkBitmap::kARGB_8888_Config, and optionally call
        bitmap.setIsOpaque(true) on the resulting SkBitmap (as an accelerator)
    */
    switch (format) {
    case PIXEL_FORMAT_RGBX_8888:    return SkBitmap::kARGB_8888_Config;
    case PIXEL_FORMAT_RGBA_8888:    return SkBitmap::kARGB_8888_Config;
    case PIXEL_FORMAT_RGBA_4444:    return SkBitmap::kARGB_4444_Config;
    case PIXEL_FORMAT_RGB_565:      return SkBitmap::kRGB_565_Config;
    case PIXEL_FORMAT_A_8:          return SkBitmap::kA8_Config;
    default:                        return SkBitmap::kNo_Config;
    }
}

static inline void swapCanvasPtr(JNIEnv* env, jobject canvasObj, SkCanvas* newCanvas) {
  jobject canvasFinalizerObj = env->GetObjectField(canvasObj, gCanvasClassInfo.mFinalizer);
  SkCanvas* previousCanvas = reinterpret_cast<SkCanvas*>(
          env->GetIntField(canvasObj, gCanvasClassInfo.mNativeCanvas));
  env->SetIntField(canvasObj, gCanvasClassInfo.mNativeCanvas, (int)newCanvas);
  env->SetIntField(canvasFinalizerObj, gCanvasFinalizerClassInfo.mNativeCanvas, (int)newCanvas);
  SkSafeUnref(previousCanvas);
}

static jobject nativeLockCanvas(JNIEnv* env, jobject surfaceObj, jint nativeObject, jobject dirtyRectObj) {
    sp<Surface> surface(reinterpret_cast<Surface *>(nativeObject));

    if (!isSurfaceValid(surface)) {
        doThrowIAE(env);
        return NULL;
    }

    // get dirty region
    Region dirtyRegion;
    if (dirtyRectObj) {
        Rect dirty;
        dirty.left = env->GetIntField(dirtyRectObj, gRectClassInfo.left);
        dirty.top = env->GetIntField(dirtyRectObj, gRectClassInfo.top);
        dirty.right = env->GetIntField(dirtyRectObj, gRectClassInfo.right);
        dirty.bottom = env->GetIntField(dirtyRectObj, gRectClassInfo.bottom);
        if (!dirty.isEmpty()) {
            dirtyRegion.set(dirty);
        }
    } else {
        dirtyRegion.set(Rect(0x3FFF, 0x3FFF));
    }

    ANativeWindow_Buffer outBuffer;
    Rect dirtyBounds(dirtyRegion.getBounds());
    status_t err = surface->lock(&outBuffer, &dirtyBounds);
    dirtyRegion.set(dirtyBounds);
    if (err < 0) {
        const char* const exception = (err == NO_MEMORY) ?
                OutOfResourcesException :
                "java/lang/IllegalArgumentException";
        jniThrowException(env, exception, NULL);
        return NULL;
    }

    // Associate a SkCanvas object to this surface
    jobject canvasObj = env->GetObjectField(surfaceObj, gSurfaceClassInfo.mCanvas);
    env->SetIntField(canvasObj, gCanvasClassInfo.mSurfaceFormat, outBuffer.format);

    SkBitmap bitmap;
    ssize_t bpr = outBuffer.stride * bytesPerPixel(outBuffer.format);
    bitmap.setConfig(convertPixelFormat(outBuffer.format), outBuffer.width, outBuffer.height, bpr);
    if (outBuffer.format == PIXEL_FORMAT_RGBX_8888) {
        bitmap.setIsOpaque(true);
    }
    if (outBuffer.width > 0 && outBuffer.height > 0) {
        bitmap.setPixels(outBuffer.bits);
    } else {
        // be safe with an empty bitmap.
        bitmap.setPixels(NULL);
    }

    SkCanvas* nativeCanvas = SkNEW_ARGS(SkCanvas, (bitmap));
    swapCanvasPtr(env, canvasObj, nativeCanvas);

    SkRegion clipReg;
    if (dirtyRegion.isRect()) { // very common case
        const Rect b(dirtyRegion.getBounds());
        clipReg.setRect(b.left, b.top, b.right, b.bottom);
    } else {
        size_t count;
        Rect const* r = dirtyRegion.getArray(&count);
        while (count) {
            clipReg.op(r->left, r->top, r->right, r->bottom, SkRegion::kUnion_Op);
            r++, count--;
        }
    }

    nativeCanvas->clipRegion(clipReg);

    if (dirtyRectObj) {
        const Rect& bounds(dirtyRegion.getBounds());
        env->SetIntField(dirtyRectObj, gRectClassInfo.left, bounds.left);
        env->SetIntField(dirtyRectObj, gRectClassInfo.top, bounds.top);
        env->SetIntField(dirtyRectObj, gRectClassInfo.right, bounds.right);
        env->SetIntField(dirtyRectObj, gRectClassInfo.bottom, bounds.bottom);
    }

    return canvasObj;
}

static void nativeUnlockCanvasAndPost(JNIEnv* env, jobject surfaceObj, jint nativeObject, jobject canvasObj) {
    jobject ownCanvasObj = env->GetObjectField(surfaceObj, gSurfaceClassInfo.mCanvas);
    if (!env->IsSameObject(ownCanvasObj, canvasObj)) {
        doThrowIAE(env);
        return;
    }

    sp<Surface> surface(reinterpret_cast<Surface *>(nativeObject));
    if (!isSurfaceValid(surface)) {
        return;
    }

    // detach the canvas from the surface
    SkCanvas* nativeCanvas = SkNEW(SkCanvas);
    swapCanvasPtr(env, canvasObj, nativeCanvas);

    // unlock surface
    status_t err = surface->unlockAndPost();
    if (err < 0) {
        doThrowIAE(env);
    }
}

// ----------------------------------------------------------------------------

static jint nativeCopyFrom(JNIEnv* env, jclass clazz,
        jint nativeObject, jint surfaceControlNativeObj) {
    /*
     * This is used by the WindowManagerService just after constructing
     * a Surface and is necessary for returning the Surface reference to
     * the caller. At this point, we should only have a SurfaceControl.
     */

    sp<SurfaceControl> ctrl(reinterpret_cast<SurfaceControl *>(surfaceControlNativeObj));
    sp<Surface> other(ctrl->getSurface());
    if (other != NULL) {
        other->incStrong(clazz);
    }

    sp<Surface> sur(reinterpret_cast<Surface *>(nativeObject));
    if (sur != NULL) {
        sur->decStrong(clazz);
    }

    return int(other.get());
}

static jint nativeReadFromParcel(JNIEnv* env, jclass clazz,
        jint nativeObject, jobject parcelObj) {
    Parcel* parcel = parcelForJavaObject(env, parcelObj);
    if (parcel == NULL) {
        doThrowNPE(env);
        return 0;
    }
    sp<Surface> self(reinterpret_cast<Surface *>(nativeObject));
    if (self != NULL) {
        self->decStrong(clazz);
    }
    sp<Surface> sur(Surface::readFromParcel(*parcel));
    if (sur != NULL) {
        sur->incStrong(clazz);
    }
    return int(sur.get());
}

static void nativeWriteToParcel(JNIEnv* env, jclass clazz,
        jint nativeObject, jobject parcelObj) {
    Parcel* parcel = parcelForJavaObject(env, parcelObj);
    if (parcel == NULL) {
        doThrowNPE(env);
        return;
    }
    sp<Surface> self(reinterpret_cast<Surface *>(nativeObject));
    Surface::writeToParcel(self, parcel);
}

// ----------------------------------------------------------------------------

static JNINativeMethod gSurfaceMethods[] = {
    {"nativeCreateFromSurfaceTexture", "(Landroid/graphics/SurfaceTexture;)I",
            (void*)nativeCreateFromSurfaceTexture },
    {"nativeRelease", "(I)V",
            (void*)nativeRelease },
    {"nativeDestroy", "(I)V",
            (void*)nativeDestroy },
    {"nativeIsValid", "(I)Z",
            (void*)nativeIsValid },
    {"nativeIsConsumerRunningBehind", "(I)Z",
            (void*)nativeIsConsumerRunningBehind },
    {"nativeLockCanvas", "(ILandroid/graphics/Rect;)Landroid/graphics/Canvas;",
            (void*)nativeLockCanvas },
    {"nativeUnlockCanvasAndPost", "(ILandroid/graphics/Canvas;)V",
            (void*)nativeUnlockCanvasAndPost },
    {"nativeCopyFrom", "(II)I",
            (void*)nativeCopyFrom },
    {"nativeReadFromParcel", "(ILandroid/os/Parcel;)I",
            (void*)nativeReadFromParcel },
    {"nativeWriteToParcel", "(ILandroid/os/Parcel;)V",
            (void*)nativeWriteToParcel },
};

int register_android_view_Surface(JNIEnv* env)
{
    int err = AndroidRuntime::registerNativeMethods(env, "android/view/Surface",
            gSurfaceMethods, NELEM(gSurfaceMethods));

    jclass clazz = env->FindClass("android/view/Surface");
    gSurfaceClassInfo.clazz = jclass(env->NewGlobalRef(clazz));
    gSurfaceClassInfo.mNativeObject =
            env->GetFieldID(gSurfaceClassInfo.clazz, "mNativeObject", "I");
    gSurfaceClassInfo.mGenerationId =
            env->GetFieldID(gSurfaceClassInfo.clazz, "mGenerationId", "I");
    gSurfaceClassInfo.mCanvas =
            env->GetFieldID(gSurfaceClassInfo.clazz, "mCanvas", "Landroid/graphics/Canvas;");
    gSurfaceClassInfo.ctor = env->GetMethodID(gSurfaceClassInfo.clazz, "<init>", "(I)V");

    clazz = env->FindClass("android/graphics/Canvas");
    gCanvasClassInfo.mFinalizer = env->GetFieldID(clazz, "mFinalizer", "Landroid/graphics/Canvas$CanvasFinalizer;");
    gCanvasClassInfo.mNativeCanvas = env->GetFieldID(clazz, "mNativeCanvas", "I");
    gCanvasClassInfo.mSurfaceFormat = env->GetFieldID(clazz, "mSurfaceFormat", "I");

    clazz = env->FindClass("android/graphics/Canvas$CanvasFinalizer");
    gCanvasFinalizerClassInfo.mNativeCanvas = env->GetFieldID(clazz, "mNativeCanvas", "I");

    clazz = env->FindClass("android/graphics/Rect");
    gRectClassInfo.left = env->GetFieldID(clazz, "left", "I");
    gRectClassInfo.top = env->GetFieldID(clazz, "top", "I");
    gRectClassInfo.right = env->GetFieldID(clazz, "right", "I");
    gRectClassInfo.bottom = env->GetFieldID(clazz, "bottom", "I");

    return err;
}

};
