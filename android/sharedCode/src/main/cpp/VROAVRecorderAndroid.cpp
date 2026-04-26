//
//  VROAVRecorderAndroid.h
//  ViroRenderer
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

#include <VRORenderTarget.h>
#include <VROPlatformUtil.h>
#include "VROAVRecorderAndroid.h"
#include "VRODriverOpenGL.h"
#include "VROImageShaderProgram.h"
#include "VROImagePostProcess.h"
#include "VRORecorderEglSurfaceDisplay.h"
#include "VRORenderToTextureDelegateAndroid.h"
#include "VROTexture.h"
#include "jni/MediaRecorder_JNI.h"

VROAVRecorderAndroid::VROAVRecorderAndroid(std::shared_ptr<MediaRecorder_JNI> jRecorder) {
    _recorderDisplay = nullptr;
    _w_mediaRecorderJNI = jRecorder;
    _isRecording = false;
    _scheduledScreenShot = false;
}

VROAVRecorderAndroid::~VROAVRecorderAndroid() {
}

void VROAVRecorderAndroid::init(std::shared_ptr<VRODriver> driver) {
    std::vector<std::string> blitSamplers = { "source_texture" };
    std::vector<std::string> blitCode = {
            "uniform sampler2D source_texture;",
            "frag_color = texture(source_texture, v_texcoord);"
    };

    std::shared_ptr<VROShaderProgram> blitShader
            = VROImageShaderProgram::create(blitSamplers, blitCode, driver);
    _recordingPostProcess = driver->newImagePostProcess(blitShader);

    // Watermark shader: samples a texture with premultiplied-ish alpha. The
    // caller enables GL_BLEND around the blit so alpha controls the composite.
    // Android bitmaps are loaded top-down (row 0 = top of image) while GL
    // texture coordinates are bottom-up (y=0 = bottom). Flip the y-coord when
    // sampling so the watermark renders upright.
    std::vector<std::string> watermarkSamplers = { "watermark_texture" };
    std::vector<std::string> watermarkCode = {
            "uniform sampler2D watermark_texture;",
            "frag_color = texture(watermark_texture, vec2(v_texcoord.x, 1.0 - v_texcoord.y));"
    };
    std::shared_ptr<VROShaderProgram> watermarkShader
            = VROImageShaderProgram::create(watermarkSamplers, watermarkCode, driver);
    _watermarkPostProcess = driver->newImagePostProcess(watermarkShader);
}

void VROAVRecorderAndroid::setWatermark(std::shared_ptr<VROTexture> texture, VROVector4f normalizedFrame) {
    if (!texture) {
        clearWatermark();
        return;
    }
    _watermarkTexture = texture;
    _watermarkFrame = normalizedFrame;
    _addWatermark = true;
}

void VROAVRecorderAndroid::clearWatermark() {
    _addWatermark = false;
    _watermarkTexture.reset();
    _watermarkFrame = VROVector4f();
}

std::shared_ptr<VRORenderToTextureDelegateAndroid> VROAVRecorderAndroid::getRenderToTextureDelegate() {
    if (_renderToTextureDelegate == nullptr) {
        _renderToTextureDelegate = std::make_shared<VRORenderToTextureDelegateAndroid>(shared_from_this());
    }
    return _renderToTextureDelegate;
}

void VROAVRecorderAndroid::setEnableVideoFrameRecording(bool isRecording) {
    std::shared_ptr<MediaRecorder_JNI> jRecorder = _w_mediaRecorderJNI.lock();
    if (!jRecorder) {
        return;
    }

    _isRecording = isRecording;
    jRecorder->onEnableFrameRecording(isRecording);
}

void VROAVRecorderAndroid::scheduleScreenCapture() {
    _scheduledScreenShot = true;
}

bool VROAVRecorderAndroid::onRenderedFrameTexture(std::shared_ptr<VRORenderTarget> input,
                                                  std::shared_ptr<VRODriver> driver) {
    if (_isRecording) {
        if (_recorderDisplay == nullptr) {
            std::shared_ptr<VRODriverOpenGL> openGLDriver = std::static_pointer_cast<VRODriverOpenGL>(driver);
            _recorderDisplay = std::make_shared<VRORecorderEglSurfaceDisplay>(openGLDriver, shared_from_this());
        }
        _recorderDisplay->setViewport({0, 0, input->getWidth(), input->getHeight()});

        driver->bindRenderTarget(_recorderDisplay, VRORenderTargetUnbindOp::Invalidate);

        // In linear color mode (HDR enabled) the scene framebuffer holds linear RGB values;
        // gamma-encode them before the encoder reads the surface, otherwise recorded video
        // is noticeably darker than the live view. In non-linear mode the framebuffer is
        // already sRGB-encoded and can be blit straight through.
        if (driver->getColorRenderingMode() == VROColorRenderingMode::Linear) {
            getGammaPostProcess(driver)->blit({ input->getTexture(0) }, driver);
        } else {
            _recordingPostProcess->blit({ input->getTexture(0) }, driver);
        }

        // Composite the watermark on top of the scene, as an alpha-blended
        // quad at the normalized frame. The recorder display is already bound.
        if (_addWatermark && _watermarkTexture && _watermarkPostProcess) {
            int surfaceW = input->getWidth();
            int surfaceH = input->getHeight();
            int wx  = (int) (_watermarkFrame.x * surfaceW);
            int wy  = (int) (_watermarkFrame.y * surfaceH);  // top-left origin
            int ww  = (int) (_watermarkFrame.z * surfaceW);
            int wh  = (int) (_watermarkFrame.w * surfaceH);
            // GL viewport origin is bottom-left; flip the y-axis.
            int glY = surfaceH - wy - wh;
            if (ww > 0 && wh > 0) {
                GL( glEnable(GL_BLEND) );
                GL( glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA) );
                GL( glViewport(wx, glY, ww, wh) );
                GL( glScissor(wx, glY, ww, wh) );

                _watermarkPostProcess->blit({ _watermarkTexture }, driver);

                // Restore full-surface viewport/scissor for any subsequent work
                // on this frame, and turn blending off so we don't leak state.
                GL( glViewport(0, 0, surfaceW, surfaceH) );
                GL( glScissor(0, 0, surfaceW, surfaceH) );
                GL( glDisable(GL_BLEND) );
            }
        }
    }

    if (_scheduledScreenShot) {
        std::shared_ptr<MediaRecorder_JNI> jRecorder = _w_mediaRecorderJNI.lock();
        if (jRecorder) {
            passert (driver->getRenderTarget() == input);

            // The input target is LDR and has already been tone-mapped, but may need gamma correction.
            // We need gamma correction if we're in linear color space.
            if (driver->getColorRenderingMode() == VROColorRenderingMode::Linear) {
                std::shared_ptr<VRORenderTarget> ldrTarget = bindScreenshotLDRTarget(input->getWidth(), input->getHeight(), driver);
                getGammaPostProcess(driver)->blit({ input->getTexture(0) }, driver);
                ldrTarget->bindRead();
            }
            // Otherwise we can perform a direct read or blit
            else {
                input->bindRead();
            }

            // This will call glReadPixels up in Java, reading from the currently bound (READ) framebuffer
            jRecorder->onTakeScreenshot();
        }
        _scheduledScreenShot = false;
    }
    return true;
}

void VROAVRecorderAndroid::bindToEglSurface() {
    std::shared_ptr<MediaRecorder_JNI> jRecorder = _w_mediaRecorderJNI.lock();
    if (!jRecorder) {
        return;
    }

    jRecorder->onBindToEGLSurface();
}

void VROAVRecorderAndroid::unbindFromEGLSurface() {
    std::shared_ptr<MediaRecorder_JNI> jRecorder = _w_mediaRecorderJNI.lock();
    if (!jRecorder) {
        return;
    }

    jRecorder->onUnbindFromEGLSurface();
}

void VROAVRecorderAndroid::eglSwap() {
    std::shared_ptr<MediaRecorder_JNI> jRecorder = _w_mediaRecorderJNI.lock();
    if (!jRecorder) {
        return;
    }

    jRecorder->onEglSwap();
}

std::shared_ptr<VRORenderTarget> VROAVRecorderAndroid::bindScreenshotLDRTarget(int width, int height,
                                                                               std::shared_ptr<VRODriver> &driver) {
    if (!_screenshotLDRTarget) {
        pinfo("Creating screenshot LDR render target");
        _screenshotLDRTarget = driver->newRenderTarget( VRORenderTargetType::ColorTexture, 1, 1, false, false);
    }
    _screenshotLDRTarget->setViewport({0, 0, width, height});
    _screenshotLDRTarget->hydrate();

    driver->bindRenderTarget(_screenshotLDRTarget, VRORenderTargetUnbindOp::Invalidate);
    return _screenshotLDRTarget;
}

std::shared_ptr<VROImagePostProcess> VROAVRecorderAndroid::getGammaPostProcess(std::shared_ptr<VRODriver> driver) {
    if (!_gammaPostProcess) {
        // The sampler name in `samplers` must match the uniform name in the shader
        // code below — VROImagePostProcess uses `samplers` to locate and bind the
        // input texture(s) to the corresponding shader uniform(s). A name mismatch
        // leaves the sampler unbound and the shader reads from texture unit 0 with
        // no texture attached, producing a black frame.
        std::vector<std::string> samplers = { "hdr_texture" };
        std::vector<std::string> code = {
                "const highp float gamma = 2.2;",
                "uniform sampler2D hdr_texture;",
                "highp vec4 srgb_color = texture(hdr_texture, v_texcoord);",
                "highp vec3 gamma_color = pow(srgb_color.xyz, vec3(1.0 / gamma));",
                "frag_color = vec4(gamma_color, srgb_color.a);",
        };

        std::shared_ptr<VROShaderModifier> modifier = std::make_shared<VROShaderModifier>(VROShaderEntryPoint::Image, code);
        std::vector<std::shared_ptr<VROShaderModifier>> modifiers = { modifier };
        std::shared_ptr<VROImageShaderProgram> shader = std::make_shared<VROImageShaderProgram>(samplers, modifiers, driver);
        _gammaPostProcess = driver->newImagePostProcess(shader);
    }

    return _gammaPostProcess;
}