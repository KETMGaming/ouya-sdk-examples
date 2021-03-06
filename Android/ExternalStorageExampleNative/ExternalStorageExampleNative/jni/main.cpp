/*
 * Copyright (C) 2010 The Android Open Source Project
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
 *
 */

//BEGIN_INCLUDE(all)
#include <jni.h>
#include <errno.h>
#include <string>

#include <EGL/egl.h>
#include <GLES/gl.h>

#include <android/sensor.h>
#include <android/log.h>
#include "../native_app_glue/android_native_app_glue.h"

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "native-activity", __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, "native-activity", __VA_ARGS__))

// If we cause an exception in JNI, we print the exception info to
// the log, we clear the exception to avoid a pending-exception
// crash, and we force the function to return.
#define EXCEPTION_RETURN(env) \
        if (env->ExceptionOccurred()) { \
                env->ExceptionDescribe(); \
                env->ExceptionClear(); \
                return; \
        }

/**
 * Our saved state data.
 */
struct saved_state {
    float angle;
    int32_t x;
    int32_t y;
};

/**
 * Shared state for our app.
 */
struct engine {
    struct android_app* app;

    ASensorManager* sensorManager;
    const ASensor* accelerometerSensor;
    ASensorEventQueue* sensorEventQueue;

    int animating;
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    int32_t width;
    int32_t height;
    struct saved_state state;
};

/**
 * Initialize an EGL context for the current display.
 */
static int engine_init_display(struct engine* engine) {
    // initialize OpenGL ES and EGL

    /*
     * Here specify the attributes of the desired configuration.
     * Below, we select an EGLConfig with at least 8 bits per color
     * component compatible with on-screen windows
     */
    const EGLint attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_BLUE_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_RED_SIZE, 8,
            EGL_NONE
    };
    EGLint w, h, dummy, format;
    EGLint numConfigs;
    EGLConfig config;
    EGLSurface surface;
    EGLContext context;

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

    eglInitialize(display, 0, 0);

    /* Here, the application chooses the configuration it desires. In this
     * sample, we have a very simplified selection process, where we pick
     * the first EGLConfig that matches our criteria */
    eglChooseConfig(display, attribs, &config, 1, &numConfigs);

    /* EGL_NATIVE_VISUAL_ID is an attribute of the EGLConfig that is
     * guaranteed to be accepted by ANativeWindow_setBuffersGeometry().
     * As soon as we picked a EGLConfig, we can safely reconfigure the
     * ANativeWindow buffers to match, using EGL_NATIVE_VISUAL_ID. */
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &format);

    ANativeWindow_setBuffersGeometry(engine->app->window, 0, 0, format);

    surface = eglCreateWindowSurface(display, config, engine->app->window, NULL);
    context = eglCreateContext(display, config, NULL, NULL);

    if (eglMakeCurrent(display, surface, surface, context) == EGL_FALSE) {
        LOGW("Unable to eglMakeCurrent");
        return -1;
    }

    eglQuerySurface(display, surface, EGL_WIDTH, &w);
    eglQuerySurface(display, surface, EGL_HEIGHT, &h);

    engine->display = display;
    engine->context = context;
    engine->surface = surface;
    engine->width = w;
    engine->height = h;
    engine->state.angle = 0;

    // Initialize GL state.
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
    glEnable(GL_CULL_FACE);
    glShadeModel(GL_SMOOTH);
    glDisable(GL_DEPTH_TEST);

    return 0;
}

/**
 * Just the current frame in the display.
 */
static void engine_draw_frame(struct engine* engine) {
    if (engine->display == NULL) {
        // No display.
        return;
    }

    // Just fill the screen with a color.
    glClearColor(((float)engine->state.x)/engine->width, engine->state.angle,
            ((float)engine->state.y)/engine->height, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    eglSwapBuffers(engine->display, engine->surface);
}

/**
 * Tear down the EGL context currently associated with the display.
 */
static void engine_term_display(struct engine* engine) {
    if (engine->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(engine->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (engine->context != EGL_NO_CONTEXT) {
            eglDestroyContext(engine->display, engine->context);
        }
        if (engine->surface != EGL_NO_SURFACE) {
            eglDestroySurface(engine->display, engine->surface);
        }
        eglTerminate(engine->display);
    }
    engine->animating = 0;
    engine->display = EGL_NO_DISPLAY;
    engine->context = EGL_NO_CONTEXT;
    engine->surface = EGL_NO_SURFACE;
}

/**
 * Process the next input event.
 */
static int32_t engine_handle_input(struct android_app* app, AInputEvent* event) {
    struct engine* engine = (struct engine*)app->userData;
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
        engine->animating = 1;
        engine->state.x = AMotionEvent_getX(event, 0);
        engine->state.y = AMotionEvent_getY(event, 0);
        return 1;
    }
    return 0;
}

/**
 * Process the next main command.
 */
static void engine_handle_cmd(struct android_app* app, int32_t cmd) {
    struct engine* engine = (struct engine*)app->userData;
    switch (cmd) {
        case APP_CMD_SAVE_STATE:
            // The system has asked us to save our current state.  Do so.
            engine->app->savedState = malloc(sizeof(struct saved_state));
            *((struct saved_state*)engine->app->savedState) = engine->state;
            engine->app->savedStateSize = sizeof(struct saved_state);
            break;
        case APP_CMD_INIT_WINDOW:
            // The window is being shown, get it ready.
            if (engine->app->window != NULL) {
                engine_init_display(engine);
                engine_draw_frame(engine);
            }
            break;
        case APP_CMD_TERM_WINDOW:
            // The window is being hidden or closed, clean it up.
            engine_term_display(engine);
            break;
        case APP_CMD_GAINED_FOCUS:
            // When our app gains focus, we start monitoring the accelerometer.
            if (engine->accelerometerSensor != NULL) {
                ASensorEventQueue_enableSensor(engine->sensorEventQueue,
                        engine->accelerometerSensor);
                // We'd like to get 60 events per second (in us).
                ASensorEventQueue_setEventRate(engine->sensorEventQueue,
                        engine->accelerometerSensor, (1000L/60)*1000);
            }
            break;
        case APP_CMD_LOST_FOCUS:
            // When our app loses focus, we stop monitoring the accelerometer.
            // This is to avoid consuming battery while not being used.
            if (engine->accelerometerSensor != NULL) {
                ASensorEventQueue_disableSensor(engine->sensorEventQueue,
                        engine->accelerometerSensor);
            }
            // Also stop animating.
            engine->animating = 0;
            engine_draw_frame(engine);
            break;
    }
}

jclass jc_context = NULL;
jclass jc_environment = NULL;
jclass jc_file = NULL;
jclass jc_nativeActivity = NULL;

/**
 * This is the main entry point of a native application that is using
 * android_native_app_glue.  It runs in its own thread, with its own
 * event loop for receiving input events and doing other things.
 */
void android_main(struct android_app* state) {

    struct engine engine;

    // Make sure glue isn't stripped.
    app_dummy();

    memset(&engine, 0, sizeof(engine));
    state->userData = &engine;
    state->onAppCmd = engine_handle_cmd;
    state->onInputEvent = engine_handle_input;
    engine.app = state;

	// in the new thread:
	JNIEnv* myNewEnv;
	JavaVMAttachArgs args;
	args.version = JNI_VERSION_1_6; // choose your JNI version
	args.name = NULL; // you might want to give the java thread a name
	args.group = NULL; // you might want to assign the java thread to a ThreadGroup
	state->activity->vm->AttachCurrentThread(&myNewEnv, &args);

	std::string msg;

	LOGI("Finding environment class...");
	jc_environment = myNewEnv->FindClass("android/os/Environment");
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Found environment class");

	LOGI("Find environment.getExternalStorageDirectory method...");
	jmethodID invokeGetExternalStorageDirectory = myNewEnv->GetStaticMethodID(jc_environment, "getExternalStorageDirectory", "()Ljava/io/File;");
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Found environment.getExternalStorageDirectory method");

	LOGI("Find environment.getExternalStoragePublicDirectory method...");
	jmethodID getExternalStoragePublicDirectory = myNewEnv->GetStaticMethodID(jc_environment, "getExternalStoragePublicDirectory", "(Ljava/lang/String;)Ljava/io/File;");
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Found environment.getExternalStoragePublicDirectory method");

	LOGI("Invoke environment.getExternalStorageDirectory method...");
	jobject fileObj = myNewEnv->CallStaticObjectMethod(jc_environment, invokeGetExternalStorageDirectory);
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Invoked environment.getExternalStorageDirectory method");

	LOGI("Finding file class...");
	jc_file = myNewEnv->FindClass("java/io/File");
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Found File class");

	LOGI("Find File.separatorChar field id...");
	jfieldID f_separatorChar = myNewEnv->GetStaticFieldID(jc_file, "separatorChar", "C");
	LOGI("Found File.separatorChar fieldid ");

	LOGI("Find File.separatorChar field...");
	jchar c_separatorChar = myNewEnv->GetStaticCharField(jc_file, f_separatorChar);
	LOGI("Found File.separatorChar field");

	char fileSeparatorChar = c_separatorChar;
	EXCEPTION_RETURN(myNewEnv);

	msg = "File.separatorChar: ";
	msg.append(&fileSeparatorChar);
	LOGI(msg.c_str());

	/*
	//convert utf16 to char
	msg = fileSeparatorChar;
	fileSeparatorChar = msg.c_str()[0];

	msg = "File.separatorChar: ";
	msg.append(&fileSeparatorChar);
	LOGI(msg.c_str());
	*/

	LOGI("Find file.File(String) constructor...");
	jmethodID constructFile = myNewEnv->GetMethodID(jc_file, "<init>", "(Ljava/lang/String;)V");
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Found file.File(String) constructor");

	LOGI("Find file.delete method...");
	jmethodID invokeDelete = myNewEnv->GetMethodID(jc_file, "delete", "()Z");
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Found fine.delete method");

	LOGI("Find file.mkdirs method...");
	jmethodID invokeMkdirs = myNewEnv->GetMethodID(jc_file, "mkdirs", "()Z");
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Found fine.mkdirs method");

	LOGI("Find file.getAbsolutePath method...");
	jmethodID invokeGetAbsolutePath = myNewEnv->GetMethodID(jc_file, "getAbsolutePath", "()Ljava/lang/String;");
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Found fine.getAbsolutePath method");

	LOGI("Invoke file.getAbsolutePath method...");
	jstring pathObj = (jstring)myNewEnv->CallObjectMethod(fileObj, invokeGetAbsolutePath);
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Invoked fine.getAbsolutePath method");

	std::string strPath = myNewEnv->GetStringUTFChars(pathObj, NULL);
	EXCEPTION_RETURN(myNewEnv);
	
	msg = "Environment.getExternalStorageDirectory result: ";
	msg.append(strPath.c_str());
	LOGI(msg.c_str());

	LOGI("Finding NativeActivity class...");
	jc_nativeActivity = myNewEnv->FindClass("android/app/NativeActivity");
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Found NativeActivity class");

	LOGI("Find NativeActivity.getApplicationContext method...");
	jmethodID invokeGetApplicationContext = myNewEnv->GetMethodID(jc_nativeActivity, "getApplicationContext", "()Landroid/content/Context;");
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Found NativeActivity.getApplicationContext method");

	LOGI("Invoke NativeActivity.getApplicationContext method...");
	jobject contextObj = myNewEnv->CallObjectMethod(state->activity->clazz, invokeGetApplicationContext);
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Invoked NativeActivity.getApplicationContext method");

	LOGI("Finding Context class...");
	jc_context = myNewEnv->FindClass("android/content/Context");
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Found Context class");

	LOGI("Find Context.getPackageName method...");
	jmethodID invokeGetPackageName = myNewEnv->GetMethodID(jc_context, "getPackageName", "()Ljava/lang/String;");
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Found Context.getPackageName method");

	LOGI("Invoke Context.getPackageName method...");
	jstring packageObj = (jstring)myNewEnv->CallObjectMethod(contextObj, invokeGetPackageName);
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Invoked Context.getPackageName method");

	std::string strPackage = myNewEnv->GetStringUTFChars(packageObj, NULL);
	EXCEPTION_RETURN(myNewEnv);

	msg = "Context.getPackageName result: ";
	msg.append(strPackage.c_str());
	LOGI(msg.c_str());

	std::string relativeExternalStoragePath = "Android";
	relativeExternalStoragePath.append(&fileSeparatorChar);
	relativeExternalStoragePath.append("data");
	relativeExternalStoragePath.append(&fileSeparatorChar);
	relativeExternalStoragePath.append(strPackage);

	std::string absoluteParentPath = strPath;
	absoluteParentPath.append(&fileSeparatorChar);
	absoluteParentPath.append(relativeExternalStoragePath);
	absoluteParentPath.append(&fileSeparatorChar);

	LOGI("Allocating parent path string...");
	jstring parentPathString = myNewEnv->NewStringUTF(absoluteParentPath.c_str());
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Allocated parent path string");
	
	LOGI("Allocating parent File object...");
	jobject objFileParent = myNewEnv->AllocObject(jc_file);
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Allocated parent File object");

	LOGI("Invoke File(string) constructor...");
	myNewEnv->CallObjectMethod(objFileParent, constructFile, parentPathString);
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Invoked File(string) constructor");

	LOGI("Invoke File.mkdirs method...");
	myNewEnv->CallObjectMethod(objFileParent, invokeMkdirs);
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Invoked File.mkdirs method");

	std::string absolutePath = strPath;
	absolutePath.append(&fileSeparatorChar);
	absolutePath.append(relativeExternalStoragePath);
	absolutePath.append(&fileSeparatorChar);
	absolutePath.append("ExternalNativeStorageExample.txt");

	msg = "External Storage IO base folder: ";
	msg.append(absolutePath.c_str());
	LOGI(msg.c_str());

	//std::mkdir(absolutionParentPath.c_str());

	LOGI("Writing to external storage...");
	FILE* fileWrite = fopen(absolutePath.c_str(), "w");
	if (fileWrite)
	{
		std::string content = "Hello External Storage!";
		fprintf(fileWrite, content.c_str());
		LOGI(content.c_str());
		fclose(fileWrite);
		LOGI("Wrote to external storage");
	}
	else
	{
		msg = "Failed to open file for write, does the path exist? ";
		msg.append(absolutePath.c_str());
		LOGI(msg.c_str());
	}

	LOGI("Reading from external storage...");
	FILE* fileRead = fopen(absolutePath.c_str(), "r");
	if (fileRead)
	{
		char buffer[256];
		std::string content;
		while (fgets(buffer, 256, fileRead) != NULL)
		{
			content.append(&buffer[0]);
		}
		LOGI(content.c_str());
		fclose(fileRead);
		LOGI("Read from external storage");
	}
	else
	{
		msg = "Failed to open file for read, does the path exist? ";
		msg.append(absolutePath.c_str());
		LOGI(msg.c_str());
	}

	LOGI("Allocating path string...");
	jstring pathString = myNewEnv->NewStringUTF(absolutePath.c_str());
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Allocated path string");
	
	LOGI("Allocating File object...");
	jobject objFile = myNewEnv->AllocObject(jc_file);
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Allocated File object");

	LOGI("Invoke File(string) constructor...");
	myNewEnv->CallObjectMethod(objFile, constructFile, pathString);
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Invoked File(string) constructor");

	LOGI("Deleting from external storage...");
	LOGI("Invoke File.delete method...");
	myNewEnv->CallObjectMethod(objFile, invokeDelete);
	EXCEPTION_RETURN(myNewEnv);
	LOGI("Invoked File.delete method");
	LOGI("Deleted from external storage...");

    // Prepare to monitor accelerometer
    engine.sensorManager = ASensorManager_getInstance();
    engine.accelerometerSensor = ASensorManager_getDefaultSensor(engine.sensorManager,
            ASENSOR_TYPE_ACCELEROMETER);
    engine.sensorEventQueue = ASensorManager_createEventQueue(engine.sensorManager,
            state->looper, LOOPER_ID_USER, NULL, NULL);

    if (state->savedState != NULL) {
        // We are starting with a previous saved state; restore from it.
        engine.state = *(struct saved_state*)state->savedState;
    }

    // loop waiting for stuff to do.

    while (1) {
        // Read all pending events.
        int ident;
        int events;
        struct android_poll_source* source;

        // If not animating, we will block forever waiting for events.
        // If animating, we loop until all events are read, then continue
        // to draw the next frame of animation.
        while ((ident=ALooper_pollAll(engine.animating ? 0 : -1, NULL, &events,
                (void**)&source)) >= 0) {

            // Process this event.
            if (source != NULL) {
                source->process(state, source);
            }

            // If a sensor has data, process it now.
            if (ident == LOOPER_ID_USER) {
                if (engine.accelerometerSensor != NULL) {
                    ASensorEvent event;
                    while (ASensorEventQueue_getEvents(engine.sensorEventQueue,
                            &event, 1) > 0) {
                        LOGI("accelerometer: x=%f y=%f z=%f",
                                event.acceleration.x, event.acceleration.y,
                                event.acceleration.z);
                    }
                }
            }

            // Check if we are exiting.
            if (state->destroyRequested != 0) {
                engine_term_display(&engine);
                return;
            }
        }

        if (engine.animating) {
            // Done with events; draw next animation frame.
            engine.state.angle += .01f;
            if (engine.state.angle > 1) {
                engine.state.angle = 0;
            }

            // Drawing is throttled to the screen update rate, so there
            // is no need to do timing here.
            engine_draw_frame(&engine);
        }
    }
}
//END_INCLUDE(all)
