//
//  Copyright © 2017 Viro Media. All rights reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining
//  a copy of this software and associated documentation files (the
//  "Software"), to deal in the Software without restriction, including
//  without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to
//  permit persons to whom the Software is furnished to do so, subject to
//  the following conditions:
//
//  The above copyright notice and this permission notice shall be included
//  in all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
//  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <memory>
#include <VROPlatformUtil.h>
#include <VRORenderToTextureDelegateAndroid.h>
#include "MediaRecorder_JNI.h"
#include "VRORenderer_JNI.h"
#include "VROChoreographer.h"
#include "VROImageAndroid.h"
#include "VROTexture.h"
#include "VROVector4f.h"

#if VRO_PLATFORM_ANDROID
#define VRO_METHOD(return_type, method_name) \
  JNIEXPORT return_type JNICALL              \
      Java_com_viro_core_ViroMediaRecorder_##method_name
#endif

extern "C" {
VRO_METHOD(jlong, nativeCreateNativeRecorder)(VRO_ARGS
                                              jlong renderer_j) {
    std::shared_ptr<MediaRecorder_JNI> recorder = std::make_shared<MediaRecorder_JNI>(obj, env);
    std::weak_ptr<VROSceneRenderer> renderer_w = Renderer::native(renderer_j);

    VROPlatformDispatchAsyncRenderer([renderer_w, recorder] {
        std::shared_ptr<VROSceneRenderer> renderer = renderer_w.lock();
        if (renderer) {
            recorder->nativeCreateRecorder(renderer);
        }
    });
    return MediaRecorder::jptr(recorder);
}

VRO_METHOD(void, nativeDeleteNativeRecorder)(VRO_ARGS
                                             jlong recorder_j) {
    delete reinterpret_cast<PersistentRef<MediaRecorder_JNI> *>(recorder_j);
}

VRO_METHOD(void, nativeEnableFrameRecording)(VRO_ARGS
                                             jlong recorder_j,
                                             jboolean isRecording_j) {
    std::shared_ptr<MediaRecorder_JNI> recorder = MediaRecorder::native(recorder_j);

    VROPlatformDispatchAsyncRenderer([recorder, isRecording_j] {
        recorder->nativeEnableFrameRecording(isRecording_j);
    });
}

VRO_METHOD(void, nativeScheduleScreenCapture)(VRO_ARGS
                                              jlong jRecorderRef) {
    std::shared_ptr<MediaRecorder_JNI> recorder = MediaRecorder::native(jRecorderRef);
    VROPlatformDispatchAsyncRenderer([recorder] {
        recorder->nativeScheduleScreenCapture();
    });
}

VRO_METHOD(void, nativeSetWatermark)(VRO_ARGS
                                     jlong jRecorderRef,
                                     jobject jBitmap,
                                     jfloat x, jfloat y, jfloat w, jfloat h) {
    std::shared_ptr<MediaRecorder_JNI> recorder = MediaRecorder::native(jRecorderRef);

    // Build the texture now on the calling thread so we can safely release the
    // local Bitmap reference. The VROImageAndroid holds its own global ref and
    // the texture will hydrate lazily when first bound during the watermark blit.
    std::shared_ptr<VROTexture> texture;
    if (jBitmap != nullptr) {
        std::shared_ptr<VROImage> image = std::make_shared<VROImageAndroid>(jBitmap);
        texture = std::make_shared<VROTexture>(/* sRGB */ true, VROMipmapMode::None, image);
    }

    VROVector4f frame(x, y, w, h);
    VROPlatformDispatchAsyncRenderer([recorder, texture, frame] {
        recorder->nativeSetWatermark(texture, frame);
    });
}

VRO_METHOD(void, nativeClearWatermark)(VRO_ARGS
                                       jlong jRecorderRef) {
    std::shared_ptr<MediaRecorder_JNI> recorder = MediaRecorder::native(jRecorderRef);
    VROPlatformDispatchAsyncRenderer([recorder] {
        recorder->nativeClearWatermark();
    });
}
} // extern "C"


MediaRecorder_JNI::MediaRecorder_JNI(VRO_OBJECT recorderJavaObject, JNIEnv *env) {
    _javaMediaRecorder = reinterpret_cast<jclass>(VROPlatformGetJNIEnv()->NewWeakGlobalRef(recorderJavaObject));
}

MediaRecorder_JNI::~MediaRecorder_JNI() {
    JNIEnv *env = VROPlatformGetJNIEnv();
    env->DeleteWeakGlobalRef(_javaMediaRecorder);
}

/*
 * Calls from Java to Native
 */
void MediaRecorder_JNI::nativeCreateRecorder(std::shared_ptr<VROSceneRenderer> renderer) {
    // Create the VROAndroidRecorder representing this Media Jni Recorder through which all calls are routed to.
    _nativeMediaRecorder = std::make_shared<VROAVRecorderAndroid>(shared_from_this());
    _choreographer = renderer->getRenderer()->getChoreographer();
    _driver = renderer->getDriver();
}

void MediaRecorder_JNI::nativeEnableFrameRecording(bool isRecording) {
    std::shared_ptr<VROChoreographer> choreographer = _choreographer.lock();
    std::shared_ptr<VRODriver> driver = _driver.lock();
    if (!choreographer || !driver) {
        return;
    }

    if (isRecording) {
        // Needs to be re-initialized at the start of each recording
        _nativeMediaRecorder->init(driver);
        std::shared_ptr<VRORenderToTextureDelegateAndroid> delegate = _nativeMediaRecorder->getRenderToTextureDelegate();
        choreographer->setRenderToTextureDelegate(delegate);
    } else {
        choreographer->setRenderToTextureDelegate(nullptr);
    }
    _nativeMediaRecorder->setEnableVideoFrameRecording(isRecording);
}

void MediaRecorder_JNI::nativeScheduleScreenCapture() {
    std::shared_ptr<VROChoreographer> choreographer = _choreographer.lock();
    if (!choreographer) {
        return;
    }

    std::shared_ptr<VRORenderToTextureDelegateAndroid> delegate = _nativeMediaRecorder->getRenderToTextureDelegate();
    choreographer->setRenderToTextureDelegate(delegate);
    _nativeMediaRecorder->scheduleScreenCapture();
}

void MediaRecorder_JNI::nativeSetWatermark(std::shared_ptr<VROTexture> texture, VROVector4f frame) {
    if (_nativeMediaRecorder) {
        _nativeMediaRecorder->setWatermark(texture, frame);
    }
}

void MediaRecorder_JNI::nativeClearWatermark() {
    if (_nativeMediaRecorder) {
        _nativeMediaRecorder->clearWatermark();
    }
}

/*
 * Native to Java calls.
 */
void MediaRecorder_JNI::onBindToEGLSurface() {
    VROPlatformCallHostFunction(_javaMediaRecorder, "onNativeBindToEGLSurface","()V");
}

void MediaRecorder_JNI::onUnbindFromEGLSurface() {
    VROPlatformCallHostFunction(_javaMediaRecorder, "onNativeUnbindEGLSurface","()V");
}

void MediaRecorder_JNI::onEnableFrameRecording(bool enabled) {
    VROPlatformCallHostFunction(_javaMediaRecorder, "onNativeEnableFrameRecording","(Z)V", enabled);
}

void MediaRecorder_JNI::onEglSwap() {
    VROPlatformCallHostFunction(_javaMediaRecorder, "onNativeSwapEGLSurface","()V");
}

void MediaRecorder_JNI::onTakeScreenshot() {
    VROPlatformCallHostFunction(_javaMediaRecorder, "onNativeTakeScreenshot","()V");

    // If we're not recording video, then go ahead and remove the delegate. There's a
    // performance penalty for leaving the RTT delegate in place in the choreographer.
    std::shared_ptr<VROChoreographer> choreographer = _choreographer.lock();
    if (!choreographer) {
        return;
    }
    if (!_nativeMediaRecorder->isRecordingVideo()) {
        choreographer->setRenderToTextureDelegate(nullptr);
    }
}