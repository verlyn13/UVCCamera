/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2024-2026 UVCCamera contributors
 *
 * File name: HandleManager.cpp
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
 */

#include "HandleManager.h"

//======================================================================
// Global HandleManager Singletons
//======================================================================
// Using function-local statics for lazy initialization (C++11 thread-safe)
// This avoids static initialization order fiasco and ensures the managers
// are initialized before first use.
//======================================================================

HandleManager& getCameraHandleManager() {
    static HandleManager instance;
    return instance;
}

HandleManager& getRingBufferHandleManager() {
    static HandleManager instance;
    return instance;
}
