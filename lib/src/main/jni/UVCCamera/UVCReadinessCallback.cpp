/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 *
 * File name: UVCReadinessCallback.cpp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * All files in the folder are under this Apache License, Version 2.0.
 */

#include <stdlib.h>
#include <linux/time.h>
#include <unistd.h>
#include "utilbase.h"
#include "UVCReadinessCallback.h"

#define LOCAL_DEBUG 0

UVCReadinessCallback::UVCReadinessCallback()
:	mReadinessCallbackObj(NULL),
	mIsReady(false) {

	ENTER();
	pthread_mutex_init(&readiness_mutex, NULL);
	EXIT();
}

UVCReadinessCallback::~UVCReadinessCallback() {

	ENTER();
	pthread_mutex_lock(&readiness_mutex);
	{
		if (mReadinessCallbackObj) {
			// Note: Can't delete global ref here as we don't have JNIEnv
			// The caller should clear the callback before destroying
			mReadinessCallbackObj = NULL;
		}
	}
	pthread_mutex_unlock(&readiness_mutex);
	pthread_mutex_destroy(&readiness_mutex);
	EXIT();
}

int UVCReadinessCallback::setCallback(JNIEnv *env, jobject readiness_callback_obj) {

	ENTER();
	pthread_mutex_lock(&readiness_mutex);
	{
		if (!env->IsSameObject(mReadinessCallbackObj, readiness_callback_obj)) {
			ireadinesscallback_fields.onNativeReady = NULL;
			if (mReadinessCallbackObj) {
				env->DeleteGlobalRef(mReadinessCallbackObj);
			}
			mReadinessCallbackObj = readiness_callback_obj;
			if (readiness_callback_obj) {
				// get method IDs of Java object for callback
				jclass clazz = env->GetObjectClass(readiness_callback_obj);
				if (LIKELY(clazz)) {
					ireadinesscallback_fields.onNativeReady = env->GetMethodID(clazz,
						"onNativeReady", "()V");
				} else {
					LOGW("failed to get object class");
				}
				env->ExceptionClear();
				if (!ireadinesscallback_fields.onNativeReady) {
					LOGE("Can't find IReadinessCallback#onNativeReady");
					env->DeleteGlobalRef(readiness_callback_obj);
					mReadinessCallbackObj = readiness_callback_obj = NULL;
				}
			}
		}
	}
	pthread_mutex_unlock(&readiness_mutex);
	RETURN(0, int);
}

void UVCReadinessCallback::notifyReady() {
	ENTER();

	mIsReady.store(true, std::memory_order_release);

	JavaVM *vm = getVM();
	JNIEnv *env;
	// attach to JavaVM
	vm->AttachCurrentThread(&env, NULL);

	pthread_mutex_lock(&readiness_mutex);
	{
		if (mReadinessCallbackObj && ireadinesscallback_fields.onNativeReady) {
			env->CallVoidMethod(mReadinessCallbackObj, ireadinesscallback_fields.onNativeReady);
			env->ExceptionClear();
		}
	}
	pthread_mutex_unlock(&readiness_mutex);

	vm->DetachCurrentThread();

	LOGD("Native readiness signaled to Kotlin");
	EXIT();
}

bool UVCReadinessCallback::isReady() const {
	return mIsReady.load(std::memory_order_acquire);
}
