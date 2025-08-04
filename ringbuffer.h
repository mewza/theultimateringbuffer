/*
 *   The Ultimate Ring Buffer v1.1 - It only took 25 years to write it.
 *   �1999-2025 SUBBAND, Inc. & Dmitry Boldyrev
 *   
 *   Description:    Ring Buffer with std::atomic Synchronization
 *   Updated:        Aug 3, 2025
 */

#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>

// #define DEBUG_RING

#ifdef DEBUG_RING
#define RING_LOG(fmt, ...) do { \
    LOG(fmt, ##__VA_ARGS__); \
} while (0)
#else
#define RING_LOG(fmt, ...) do { \
} while (0)
#endif

/*
    The Ultimate Ring Buffer - It only took 25 years to write it.
    �1999-2025 SUBBAND, Inc. & Dmitry Boldyrev
    
    Description:    Ring Buffer with std::atomic Synchronization
    Updated:        Aug 3, 2025
*/

#pragma once

#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>

// #define DEBUG_RING

#ifdef DEBUG_RING
#define RING_LOG(fmt, ...) do { \
    LOG(fmt, ##__VA_ARGS__); \
} while (0)
#else
#define RING_LOG(fmt, ...) do { \
} while (0)
#endif

class RingBuffer {
private:
    std::atomic<int> mReadPos{0};
    std::atomic<int> mWritePos{0};
    int mBufSize{0};
    std::unique_ptr<uint8_t[]> mBuffer;
    
    // Save/restore state (used by reader thread only for peek operations)
    int mSaveReadPos{-1};
    
    // Debug counters
    mutable int mSaveReadCallCount{0};
    mutable int mRestoreReadCallCount{0};
    
public:
    explicit RingBuffer(int size = 1024) {
        if (Init(size) < 0) {
            throw std::runtime_error("Buffer initialization failed");
        }
    }
    
    inline int Init(int inSize) {
        try {
            mBuffer = std::unique_ptr<uint8_t[]>(new uint8_t[inSize]);
            mBufSize = inSize;
            Empty();
            return 0;
        } catch (const std::bad_alloc&) {
            RING_LOG("Memory allocation failed for size %d", inSize);
            return -1;
        }
    }
    
    inline int BufSize() const {
        return mBufSize;
    }
    
    inline void Empty() {
        mReadPos.store(0, std::memory_order_relaxed);
        mWritePos.store(0, std::memory_order_relaxed);
        mSaveReadPos = -1;
        
        // Memory fence to ensure visibility
        std::atomic_thread_fence(std::memory_order_release);
        
        RING_LOG("Buffer emptied");
    }
    
    // ===== SPACE CALCULATIONS =====
    
    inline int FreeSpace(bool inAfterMarker = true) const {
        int currentRead = mReadPos.load(std::memory_order_acquire);
        int currentWrite = mWritePos.load(std::memory_order_acquire);
        
        // Free space = total - used - 1 (keep one slot empty to distinguish full/empty)
        int used = (currentWrite - currentRead + mBufSize) % mBufSize;
        return mBufSize - used - 1;
    }
    
    inline int UsedSpace(bool inAfterMarker = true) const {
        int currentRead = mReadPos.load(std::memory_order_acquire);
        int currentWrite = mWritePos.load(std::memory_order_acquire);
        
        return (currentWrite - currentRead + mBufSize) % mBufSize;
    }
    
    // ===== CORE READ/WRITE OPERATIONS =====
    
    inline int WriteData(const void* _Nullable data, int bytes) {
        if (bytes <= 0) return 0;
        
        int currentRead = mReadPos.load(std::memory_order_acquire);
        int currentWrite = mWritePos.load(std::memory_order_relaxed);
        
        // Calculate available space (keep 1 slot empty)
        int available = (currentRead - currentWrite - 1 + mBufSize) % mBufSize;
        bytes = std::min(bytes, available);
        
        if (bytes > 0 && data) {
            int endWrite = (currentWrite + bytes) % mBufSize;
            
            if (endWrite > currentWrite) {
                // No wraparound
                std::memcpy(&mBuffer[currentWrite], data, bytes);
            } else {
                // Handle wraparound
                int firstPart = mBufSize - currentWrite;
                std::memcpy(&mBuffer[currentWrite], data, firstPart);
                std::memcpy(&mBuffer[0], static_cast<const uint8_t*>(data) + firstPart, bytes - firstPart);
            }
            
            // Update write position with release semantics
            mWritePos.store(endWrite, std::memory_order_release);
            
            RING_LOG("WriteData: wrote %d bytes, writePos %d->%d", bytes, currentWrite, endWrite);
        }
        
        return bytes;
    }
    
    inline int ReadData(void* _Nullable data, int bytes) {
        if (bytes <= 0) return 0;
        
        int currentWrite = mWritePos.load(std::memory_order_acquire);
        int currentRead = mReadPos.load(std::memory_order_relaxed);
        
        // Calculate available data
        int available = (currentWrite - currentRead + mBufSize) % mBufSize;
        bytes = std::min(bytes, available);
        
        if (bytes > 0) {
            int endRead = (currentRead + bytes) % mBufSize;
            
            if (data) {
                if (endRead > currentRead) {
                    // No wraparound
                    std::memcpy(data, &mBuffer[currentRead], bytes);
                } else {
                    // Handle wraparound
                    int firstPart = mBufSize - currentRead;
                    std::memcpy(data, &mBuffer[currentRead], firstPart);
                    std::memcpy(static_cast<uint8_t*>(data) + firstPart, &mBuffer[0], bytes - firstPart);
                }
            }
            
            // Update read position with release semantics
            mReadPos.store(endRead, std::memory_order_release);
            
            RING_LOG("ReadData: read %d bytes, readPos %d->%d", bytes, currentRead, endRead);
        }
        
        return bytes;
    }
    
    // ===== PEEK OPERATIONS =====
    
    inline int PeekData(void* dst, int bytes) const {
        if (!dst || bytes <= 0) return -1;
        
        int currentWrite = mWritePos.load(std::memory_order_acquire);
        int currentRead = mReadPos.load(std::memory_order_acquire);
        
        int available = (currentWrite - currentRead + mBufSize) % mBufSize;
        if (available < bytes) {
            return -1; // Not enough data
        }
        
        if (currentRead < 0 || currentRead >= mBufSize) {
            return -1; // Invalid read position
        }
        
        int endRead = (currentRead + bytes) % mBufSize;
        
        if (endRead > currentRead) {
            // No wraparound
            std::memcpy(dst, &mBuffer[currentRead], bytes);
        } else {
            // Handle wraparound
            int firstPart = mBufSize - currentRead;
            std::memcpy(dst, &mBuffer[currentRead], firstPart);
            std::memcpy(static_cast<uint8_t*>(dst) + firstPart, &mBuffer[0], bytes - firstPart);
        }
        
        return bytes;
    }
    
    // ===== SAVE/RESTORE FOR PEEK MODE =====
    
    inline void SaveRead() {
        mSaveReadCallCount++;
        mSaveReadPos = mReadPos.load(std::memory_order_acquire);
        
        RING_LOG("SaveRead: saved position %d (call #%d)", mSaveReadPos, mSaveReadCallCount);
    }
    
    inline int RestoreRead() {
        mRestoreReadCallCount++;
        
        if (mSaveReadPos == -1) {
            RING_LOG("RestoreRead: no save state exists (call #%d)", mRestoreReadCallCount);
            return -1;
        }
        
        int oldPos = mReadPos.load(std::memory_order_relaxed);
        mReadPos.store(mSaveReadPos, std::memory_order_release);
        
        RING_LOG("RestoreRead: restored %d->%d (call #%d)", oldPos, mSaveReadPos, mRestoreReadCallCount);
        
        // Clear save state
        mSaveReadPos = -1;
        return 0;
    }
    
    inline void ClearSaveState() {
        if (mSaveReadPos != -1) {
            RING_LOG("ClearSaveState: clearing saved position %d", mSaveReadPos);
            mSaveReadPos = -1;
        }
    }
    
    inline bool IsReadMode() const {
        return mSaveReadPos == -1;
    }
    
    // ===== POSITIONING OPERATIONS =====
    
    inline int SkipData(int bytes) {
        if (bytes <= 0) return 0;
        
        // Same as ReadData but without copying
        int currentWrite = mWritePos.load(std::memory_order_acquire);
        int currentRead = mReadPos.load(std::memory_order_relaxed);
        
        int available = (currentWrite - currentRead + mBufSize) % mBufSize;
        bytes = std::min(bytes, available);
        
        if (bytes > 0) {
            int endRead = (currentRead + bytes) % mBufSize;
            mReadPos.store(endRead, std::memory_order_release);
            
            RING_LOG("SkipData: skipped %d bytes, readPos %d->%d", bytes, currentRead, endRead);
        }
        
        return bytes;
    }
    
    inline int Rewind(int bytes) {
        if (bytes <= 0) return 0;
        
        // Only allow rewind in save mode for safety
        if (mSaveReadPos == -1) {
            RING_LOG("Rewind: no save state - cannot rewind");
            return -1;
        }
        
        int currentRead = mReadPos.load(std::memory_order_relaxed);
        
        // Calculate how far we can safely rewind
        int maxRewind = (currentRead - mSaveReadPos + mBufSize) % mBufSize;
        
        if (bytes > maxRewind) {
            RING_LOG("Rewind: requested %d > max %d", bytes, maxRewind);
            return -1;
        }
        
        int newRead = (currentRead - bytes + mBufSize) % mBufSize;
        mReadPos.store(newRead, std::memory_order_release);
        
        RING_LOG("Rewind: rewound %d bytes, readPos %d->%d", bytes, currentRead, newRead);
        return bytes;
    }
    
    inline int Offset(int delta) {
        if (delta == 0) return 0;
        
        int currentRead = mReadPos.load(std::memory_order_relaxed);
        int currentWrite = mWritePos.load(std::memory_order_acquire);
        
        int newRead = (currentRead + delta + mBufSize) % mBufSize;
        
        // Validate new position doesn't go past write position
        int available = (currentWrite - currentRead + mBufSize) % mBufSize;
        
        if (delta > 0 && delta > available) {
            RING_LOG("Offset: forward offset %d > available %d", delta, available);
            return -1;
        }
        
        if (delta < 0) {
            // For backward offset, check if we have save state
            if (mSaveReadPos == -1) {
                RING_LOG("Offset: backward offset requires save state");
                return -1;
            }
            
            int maxBackward = (currentRead - mSaveReadPos + mBufSize) % mBufSize;
            if (-delta > maxBackward) {
                RING_LOG("Offset: backward offset %d > max %d", -delta, maxBackward);
                return -1;
            }
        }
        
        mReadPos.store(newRead, std::memory_order_release);
        RING_LOG("Offset: moved %d, readPos %d->%d", delta, currentRead, newRead);
        return 0;
    }
    
    inline bool CanOffset(int offset) const {
        int currentRead = mReadPos.load(std::memory_order_acquire);
        int currentWrite = mWritePos.load(std::memory_order_acquire);
        
        if (offset > 0) {
            int available = (currentWrite - currentRead + mBufSize) % mBufSize;
            return offset <= available;
        } else if (offset < 0) {
            if (mSaveReadPos == -1) return false;
            int maxBackward = (currentRead - mSaveReadPos + mBufSize) % mBufSize;
            return (-offset) <= maxBackward;
        }
        
        return true; // offset == 0
    }
    
    // ===== DEBUGGING & VALIDATION =====
    
    inline void LogSaveRestoreBalance() const {
        RING_LOG("Save/Restore balance: SaveRead=%d, RestoreRead=%d, InSaveMode=%s",
                 mSaveReadCallCount, mRestoreReadCallCount,
                 (mSaveReadPos != -1) ? "YES" : "NO");
    }
    
    inline void LogBufferState(const char* _Nonnull context = "") const {
        int currentRead = mReadPos.load(std::memory_order_acquire);
        int currentWrite = mWritePos.load(std::memory_order_acquire);
        int used = UsedSpace();
        int free = FreeSpace();
        
        RING_LOG("Buffer[%s]: size=%d, free=%d, used=%d, read=%d, write=%d, saveMode=%s",
                 context, mBufSize, free, used, currentRead, currentWrite,
                 (mSaveReadPos != -1) ? "YES" : "NO");
    }
    
    inline void CheckSaveRestoreUsage() const {
        RING_LOG("Save/Restore Usage Analysis:");
        RING_LOG("   SaveRead calls: %d", mSaveReadCallCount);
        RING_LOG("   RestoreRead calls: %d", mRestoreReadCallCount);
        RING_LOG("   Currently in save mode: %s", (mSaveReadPos != -1) ? "YES" : "NO");
        
        if (mSaveReadCallCount > mRestoreReadCallCount + 1) {
            RING_LOG("⚠️ SaveRead/RestoreRead imbalance detected!");
        }
    }
    
    inline void DumpBufferState(const char* context) const {
        int currentRead = mReadPos.load(std::memory_order_acquire);
        int currentWrite = mWritePos.load(std::memory_order_acquire);
        
        RING_LOG("BUFFER DUMP [%s]:", context);
        RING_LOG("  bufSize: %d", mBufSize);
        RING_LOG("  readPos: %d", currentRead);
        RING_LOG("  writePos: %d", currentWrite);
        RING_LOG("  saveReadPos: %d", mSaveReadPos);
        RING_LOG("  freeSpace: %d", FreeSpace());
        RING_LOG("  usedSpace: %d", UsedSpace());
        RING_LOG("  inSaveMode: %s", (mSaveReadPos != -1) ? "YES" : "NO");
        RING_LOG("  isValid: %s", ValidateBuffer() ? "YES" : "NO");
    }
    
    inline bool ValidateBuffer() const {
        int currentRead = mReadPos.load(std::memory_order_acquire);
        int currentWrite = mWritePos.load(std::memory_order_acquire);
        
        // Check positions are within bounds
        if (currentRead < 0 || currentRead >= mBufSize) {
            RING_LOG("❌ Invalid readPos: %d", currentRead);
            return false;
        }
        
        if (currentWrite < 0 || currentWrite >= mBufSize) {
            RING_LOG("❌ Invalid writePos: %d", currentWrite);
            return false;
        }
        
        if (mSaveReadPos != -1 && (mSaveReadPos < 0 || mSaveReadPos >= mBufSize)) {
            RING_LOG("❌ Invalid saveReadPos: %d", mSaveReadPos);
            return false;
        }
        
        // Check space calculations are consistent
        int used = UsedSpace();
        int free = FreeSpace();
        
        if (used + free + 1 != mBufSize) { // +1 for the reserved slot
            RING_LOG("❌ Space calculation error: used=%d + free=%d + 1 != size=%d", used, free, mBufSize);
            return false;
        }
        
        return true;
    }
    
    // Disable copy operations
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&& other) noexcept = delete;
    RingBuffer& operator=(RingBuffer&& other) noexcept = delete;
};

