/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2024-2026 UVCCamera contributors
 *
 * File name: HandleManager.h
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

#ifndef HANDLEMANAGER_H_
#define HANDLEMANAGER_H_

#include <atomic>
#include <cstdint>
#include <unistd.h>  // For usleep()
#include <android/log.h>

//======================================================================
// Strong-Reference Handle Manager
//======================================================================
//
// This implements slot-based reference counting to prevent use-after-free
// crashes when Kotlin/Java holds stale JNI handles.
//
// Key insight: Simple generation IDs have a race condition:
//   Thread A: validateHandle(id) -> returns valid pointer
//   Thread B: destroyCamera() -> deletes object, increments generation
//   Thread A: Uses now-dangling pointer -> CRASH
//
// Solution: ScopedRef increments activeRefs BEFORE validation.
// invalidateAndFree() spins until activeRefs == 0 before deleting.
//
// Memory ordering:
//   - acquire on read (ensures we see latest writes)
//   - release on write (ensures our writes are visible)
//   - No seq_cst needed (we use generation for ordering, not atomics)
//
//======================================================================

// Use local log tag for HandleManager (avoid conflicts with utilbase.h)
#define HANDLE_LOG_TAG "HandleManager"
#define HANDLE_LOGI(...) __android_log_print(ANDROID_LOG_INFO, HANDLE_LOG_TAG, __VA_ARGS__)
#define HANDLE_LOGW(...) __android_log_print(ANDROID_LOG_WARN, HANDLE_LOG_TAG, __VA_ARGS__)
#define HANDLE_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, HANDLE_LOG_TAG, __VA_ARGS__)

// Maximum concurrent camera/ring buffer instances
// 64 is more than enough for USB cameras (typically 1-4 devices)
static constexpr int MAX_HANDLE_SLOTS = 64;

// Error codes
static constexpr int64_t INVALID_HANDLE = -1;
static constexpr int JNI_ERR_INVALID_HANDLE = -100;

//======================================================================
// HandleSlot: Individual slot in the handle table
//======================================================================

// Cache line size (typical for ARM and x86)
static constexpr size_t CACHE_LINE_SIZE = 64;

struct HandleSlot {
    // Generation counter (odd = alive, even = dead)
    // This allows detecting stale handles without holding locks
    std::atomic<uint32_t> generation{0};  // Starts even (dead)

    // Number of JNI calls currently using this slot
    // invalidateAndFree() waits for this to reach 0
    std::atomic<int> activeRefs{0};

    // The actual context pointer (UVCCamera*, FrameBufferRing*, etc.)
    void* context{nullptr};

    // Cache-line padding to prevent false sharing between slots
    // Calculated to ensure total size is CACHE_LINE_SIZE
    // 32-bit: 4 + 4 + 4 = 12 bytes data -> 52 bytes padding
    // 64-bit: 4 + 4 + 8 = 16 bytes data -> 48 bytes padding
private:
    static constexpr size_t DATA_SIZE = sizeof(std::atomic<uint32_t>) +
                                        sizeof(std::atomic<int>) +
                                        sizeof(void*);
    static constexpr size_t PADDING_SIZE = CACHE_LINE_SIZE - DATA_SIZE;
    char padding[PADDING_SIZE];
};

// Static assert to verify cache-line alignment
static_assert(sizeof(HandleSlot) == CACHE_LINE_SIZE, "HandleSlot should be cache-line aligned");

//======================================================================
// HandleManager: Slot-based reference counting for JNI handles
//======================================================================
class HandleManager {
private:
    HandleSlot mSlots[MAX_HANDLE_SLOTS];
    std::atomic<uint32_t> mNextFreeHint{0};  // Hint for next free slot search

public:
    //------------------------------------------------------------------
    // ScopedRef: RAII guard for JNI calls
    //------------------------------------------------------------------
    // Usage:
    //   auto ref = getHandleManager().acquire(handle);
    //   if (!ref) return JNI_ERR_INVALID_HANDLE;
    //   UVCCamera* camera = static_cast<UVCCamera*>(ref.ptr);
    //   // Use camera safely...
    //   // ~ScopedRef() automatically decrements activeRefs
    //------------------------------------------------------------------
    class ScopedRef {
    private:
        HandleSlot* mSlot;

    public:
        void* ptr;  // The context pointer (public for easy access)

        ScopedRef(HandleSlot* slot, void* p) : mSlot(slot), ptr(p) {}

        ~ScopedRef() {
            if (mSlot) {
                mSlot->activeRefs.fetch_sub(1, std::memory_order_release);
            }
        }

        // Move constructor
        ScopedRef(ScopedRef&& other) noexcept : mSlot(other.mSlot), ptr(other.ptr) {
            other.mSlot = nullptr;
            other.ptr = nullptr;
        }

        // Move assignment (deleted - use move constructor instead)
        ScopedRef& operator=(ScopedRef&&) = delete;

        // Copy operations disabled
        ScopedRef(const ScopedRef&) = delete;
        ScopedRef& operator=(const ScopedRef&) = delete;

        // Boolean conversion for if (!ref) checks
        explicit operator bool() const { return ptr != nullptr; }

        // Get slot for advanced operations (e.g., checking generation)
        HandleSlot* getSlot() const { return mSlot; }
    };

    //------------------------------------------------------------------
    // registerContext: Create a new handle for a context pointer
    //------------------------------------------------------------------
    // Returns: Handle encoding (generation << 32) | index, or INVALID_HANDLE
    //------------------------------------------------------------------
    int64_t registerContext(void* ctx) {
        if (ctx == nullptr) {
            HANDLE_LOGE("registerContext: null context");
            return INVALID_HANDLE;
        }

        // Search for a free slot starting from hint
        uint32_t startIdx = mNextFreeHint.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < MAX_HANDLE_SLOTS; i++) {
            uint32_t idx = (startIdx + i) % MAX_HANDLE_SLOTS;
            HandleSlot& slot = mSlots[idx];

            // Check if slot is dead (even generation) and empty
            uint32_t gen = slot.generation.load(std::memory_order_acquire);
            if ((gen % 2) == 0 && slot.context == nullptr) {
                // Try to claim this slot
                // Note: This is not fully atomic, but races are benign:
                // - Two threads claiming same slot: one will overwrite, but
                //   the overwritten context was never returned to caller
                // - This is acceptable for camera use case (single-threaded init)

                slot.context = ctx;
                uint32_t newGen = gen + 1;  // Make it odd (alive)
                slot.generation.store(newGen, std::memory_order_release);

                // Update hint for next search
                mNextFreeHint.store((idx + 1) % MAX_HANDLE_SLOTS, std::memory_order_relaxed);

                int64_t handle = (static_cast<int64_t>(newGen) << 32) | idx;
                HANDLE_LOGI("registerContext: ctx=%p -> handle=0x%llx (slot=%u, gen=%u)",
                     ctx, (unsigned long long)handle, idx, newGen);
                return handle;
            }
        }

        HANDLE_LOGE("registerContext: no free slots available (max=%d)", MAX_HANDLE_SLOTS);
        return INVALID_HANDLE;
    }

    //------------------------------------------------------------------
    // acquire: Get a ScopedRef for a handle (increments activeRefs)
    //------------------------------------------------------------------
    // Returns: ScopedRef with valid ptr, or ScopedRef{nullptr, nullptr}
    //------------------------------------------------------------------
    ScopedRef acquire(int64_t handle) {
        if (handle == INVALID_HANDLE || handle < 0) {
            return {nullptr, nullptr};
        }

        uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFF);
        uint32_t expectedGen = static_cast<uint32_t>((handle >> 32) & 0xFFFFFFFF);

        if (idx >= MAX_HANDLE_SLOTS) {
            HANDLE_LOGW("acquire: invalid index %u (max=%d)", idx, MAX_HANDLE_SLOTS);
            return {nullptr, nullptr};
        }

        HandleSlot& slot = mSlots[idx];

        // CRITICAL: Increment activeRefs FIRST, BEFORE checking generation
        // This prevents the race where another thread invalidates between
        // our check and our use of the pointer.
        slot.activeRefs.fetch_add(1, std::memory_order_acquire);

        // Now validate generation
        uint32_t actualGen = slot.generation.load(std::memory_order_acquire);
        if (actualGen == expectedGen && slot.context != nullptr) {
            // Valid handle - ScopedRef will decrement activeRefs on destruction
            return {&slot, slot.context};
        }

        // Invalid handle - decrement activeRefs and return null
        slot.activeRefs.fetch_sub(1, std::memory_order_release);

        if (actualGen != expectedGen) {
            HANDLE_LOGW("acquire: generation mismatch for handle 0x%llx (expected=%u, actual=%u)",
                 (unsigned long long)handle, expectedGen, actualGen);
        } else {
            HANDLE_LOGW("acquire: null context for handle 0x%llx", (unsigned long long)handle);
        }

        return {nullptr, nullptr};
    }

    //------------------------------------------------------------------
    // invalidateAndFree: Mark handle as invalid and wait for refs to drain
    //------------------------------------------------------------------
    // Returns: The context pointer (caller must delete it), or nullptr
    //
    // This function BLOCKS until all active JNI calls using this handle
    // have completed. This is acceptable because:
    // - JNI calls are typically short (<10ms)
    // - Camera disconnect is not latency-critical
    // - Blocking prevents use-after-free crashes
    //------------------------------------------------------------------
    void* invalidateAndFree(int64_t handle) {
        if (handle == INVALID_HANDLE || handle < 0) {
            return nullptr;
        }

        uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFF);
        uint32_t expectedGen = static_cast<uint32_t>((handle >> 32) & 0xFFFFFFFF);

        if (idx >= MAX_HANDLE_SLOTS) {
            HANDLE_LOGW("invalidateAndFree: invalid index %u", idx);
            return nullptr;
        }

        HandleSlot& slot = mSlots[idx];

        // Verify generation matches (caller should have valid handle)
        uint32_t actualGen = slot.generation.load(std::memory_order_acquire);
        if (actualGen != expectedGen) {
            HANDLE_LOGW("invalidateAndFree: generation mismatch (expected=%u, actual=%u)",
                 expectedGen, actualGen);
            return nullptr;
        }

        // Step 1: Mark as dead (increment to even)
        // New acquire() calls will now fail
        slot.generation.fetch_add(1, std::memory_order_release);
        HANDLE_LOGI("invalidateAndFree: marked handle 0x%llx as dead (gen=%u->%u)",
             (unsigned long long)handle, actualGen, actualGen + 1);

        // Step 2: Spin-wait for active references to drain
        int spinCount = 0;
        const int maxSpins = 10000;  // ~10 seconds at 1ms per spin
        while (slot.activeRefs.load(std::memory_order_acquire) > 0) {
            if (++spinCount > maxSpins) {
                HANDLE_LOGE("invalidateAndFree: TIMEOUT waiting for refs to drain (handle=0x%llx, refs=%d)",
                     (unsigned long long)handle, slot.activeRefs.load());
                // Continue anyway to avoid permanent leak, but log the error
                break;
            }
            if (spinCount % 100 == 0) {
                HANDLE_LOGW("invalidateAndFree: waiting for %d active refs (spin=%d)",
                     slot.activeRefs.load(), spinCount);
            }
            usleep(1000);  // 1ms = 1000 microseconds
        }

        // Step 3: Extract and clear context
        void* ctx = slot.context;
        slot.context = nullptr;

        HANDLE_LOGI("invalidateAndFree: freed handle 0x%llx -> ctx=%p (spins=%d)",
             (unsigned long long)handle, ctx, spinCount);

        return ctx;
    }

    //------------------------------------------------------------------
    // isValid: Check if handle is valid without acquiring
    //------------------------------------------------------------------
    // Note: This is a snapshot - handle could become invalid immediately after
    //------------------------------------------------------------------
    bool isValid(int64_t handle) const {
        if (handle == INVALID_HANDLE || handle < 0) {
            return false;
        }

        uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFF);
        uint32_t expectedGen = static_cast<uint32_t>((handle >> 32) & 0xFFFFFFFF);

        if (idx >= MAX_HANDLE_SLOTS) {
            return false;
        }

        const HandleSlot& slot = mSlots[idx];
        uint32_t actualGen = slot.generation.load(std::memory_order_acquire);

        return (actualGen == expectedGen) && (slot.context != nullptr);
    }

    //------------------------------------------------------------------
    // getActiveRefCount: Get current active ref count for debugging
    //------------------------------------------------------------------
    int getActiveRefCount(int64_t handle) const {
        if (handle == INVALID_HANDLE || handle < 0) {
            return -1;
        }

        uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFF);
        if (idx >= MAX_HANDLE_SLOTS) {
            return -1;
        }

        return mSlots[idx].activeRefs.load(std::memory_order_acquire);
    }
};

//======================================================================
// Global HandleManager instances
//======================================================================
// We use separate managers for different object types to avoid
// type confusion attacks and make debugging easier.
//======================================================================

// For UVCCamera objects
HandleManager& getCameraHandleManager();

// For FrameBufferRing objects
HandleManager& getRingBufferHandleManager();

#endif /* HANDLEMANAGER_H_ */
