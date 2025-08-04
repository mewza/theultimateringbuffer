/*
 *   The Ultimate Ring Buffer v1.2 - It only took 25 years to write it.
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
    fprintf(stderr, fmt, ##__VA_ARGS__); \
} while (0)
#else
#define RING_LOG(fmt, ...) do { \
} while (0)
#endif

class RingBuffer {
private:
    std::atomic<int> mReadPos{0};
    std::atomic<int> mWritePos{0};
    int mSaveFreeSpace{-1};
    int mSaveReadPos{-1};
    int mBufSize{0};
    std::unique_ptr<uint8_t[]> mBuffer;
public:
    explicit RingBuffer(int size = 1024) {
        if (Init(size) < 0) {
            throw std::runtime_error("Buffer initialization failed");
        }
    }
    
    inline int Init(int inSize) {
        try {
            mBuffer = std::unique_ptr<uint8_t[]>(new uint8_t[inSize + 1]);
            mBufSize = inSize + 1;
            Empty();
            return 0;
        } catch (const std::bad_alloc&) {
            RING_LOG("Memory allocation failed for size %d", inSize);
            return -1;
        }
    }
    
    inline int BufSize() const {
        return mBufSize - 1;
    }
    
    inline void Empty() {
        mReadPos.store(0, std::memory_order_relaxed);
        mWritePos.store(0, std::memory_order_relaxed);
        mSaveReadPos = -1;
        
        std::atomic_thread_fence(std::memory_order_release);
    }
    
    // ===== SPACE CALCULATIONS =====
    
    inline int FreeSpace(bool inAfterMarker = true) const {
        return BufSize() - UsedSpace(inAfterMarker);
    }
        
    inline int UsedSpace(bool inAfterMarker = true) const {
        int currentRead = mReadPos.load(std::memory_order_acquire);
        int currentWrite = mWritePos.load(std::memory_order_acquire);
        
        if (!inAfterMarker && mSaveReadPos != -1) {
            currentRead = mSaveReadPos;
        }
        return (currentWrite - currentRead + mBufSize) % mBufSize;
    }
    
    // ===== CORE READ/WRITE OPERATIONS =====
   
    inline int WriteData(const void* _Nullable data, int bytes) {
        if (bytes <= 0) return 0;
        
        int currentRead = mReadPos.load(std::memory_order_acquire);
        int currentWrite = mWritePos.load(std::memory_order_relaxed);
        
        // Calculate available space (keep 1 slot empty)
        int available = (currentRead - currentWrite - 1 + mBufSize) % mBufSize;
        if ((bytes = std::min(bytes, available)) == 0)
            return 0;
        
        if (data) {
            int endWrite = (currentWrite + bytes) % mBufSize;
            
            if (endWrite > currentWrite) {
                std::memcpy(&mBuffer[currentWrite], data, bytes);
            } else {
                int firstPart = mBufSize - currentWrite;
                std::memcpy(&mBuffer[currentWrite], data, firstPart);
                std::memcpy(&mBuffer[0], static_cast<const uint8_t*>(data) + firstPart, bytes - firstPart);
            }
            
            mWritePos.store(endWrite, std::memory_order_release);
            
            if (mSaveFreeSpace != -1) {
                mSaveFreeSpace = std::max(mSaveFreeSpace - bytes, 0);
            }
            
            RING_LOG("WriteData: wrote %d bytes, writePos %d→%d, savedFree=%d",
                     bytes, currentWrite, endWrite, mSaveFreeSpace);
        }
        return bytes;
    }
    
    inline int ReadData(void* _Nullable data, int bytes) {
        if (bytes <= 0) return 0;
        
        int currentWrite = mWritePos.load(std::memory_order_acquire);
        int currentRead = mReadPos.load(std::memory_order_relaxed);
        
        int available = (currentWrite - currentRead + mBufSize) % mBufSize;
        if ((bytes = std::min(bytes, available)) == 0)
            return 0;
        
        if (bytes > 0) {
            int endRead = (currentRead + bytes) % mBufSize;
            
            if (data) {
                if (endRead > currentRead) {
                    std::memcpy(data, &mBuffer[currentRead], bytes);
                } else {
                    int firstPart = mBufSize - currentRead;
                    std::memcpy(data, &mBuffer[currentRead], firstPart);
                    std::memcpy(static_cast<uint8_t*>(data) + firstPart, &mBuffer[0], bytes - firstPart);
                }
            }
            
            mReadPos.store(endRead, std::memory_order_release);
            
            if (mSaveFreeSpace != -1) {
                mSaveFreeSpace = std::max(mSaveFreeSpace - bytes, 0);
            }
            
            RING_LOG("ReadData: read %d bytes, readPos %d→%d, savedFree now %d", bytes, currentRead, endRead, mSaveFreeSpace);
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
            std::memcpy(dst, &mBuffer[currentRead], bytes);
        } else {
            int firstPart = mBufSize - currentRead;
            std::memcpy(dst, &mBuffer[currentRead], firstPart);
            std::memcpy(static_cast<uint8_t*>(dst) + firstPart, &mBuffer[0], bytes - firstPart);
        }
        return bytes;
    }
    
    // ===== SAVE/RESTORE FOR PEEK MODE =====
    
    inline void SaveRead() {
        if (mSaveReadPos != -1) return;  // Already saved
        
        mSaveReadPos = mReadPos.load(std::memory_order_acquire);
        mSaveFreeSpace = FreeSpace(true);  // Save current free space
        
        RING_LOG("SaveRead: saved readPos=%d, freeSpace=%d", mSaveReadPos, mSaveFreeSpace);
    }
    
    inline int RestoreRead() {
        if (mSaveReadPos == -1) {
            RING_LOG("RestoreRead: no save state exists");
            return -1;
        }
        
        int oldPos = mReadPos.load(std::memory_order_relaxed);
        mReadPos.store(mSaveReadPos, std::memory_order_release);
        
        RING_LOG("RestoreRead: restored readPos %d→%d, freeSpace=%d",
                 oldPos, mSaveReadPos, mSaveFreeSpace);
        
        // Clear save state
        mSaveReadPos = -1;
        mSaveFreeSpace = -1;
        return 0;
    }
    
    inline void ClearSaveState() {
        if (mSaveReadPos != -1) {
            RING_LOG("ClearSaveState: clearing saved readPos=%d, freeSpace=%d", mSaveReadPos, mSaveFreeSpace);
            mSaveReadPos = -1;
            mSaveFreeSpace = -1;
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
        if ((bytes = std::min(bytes, available)) == 0)
            return 0;
        
        int endRead = (currentRead + bytes) % mBufSize;
        mReadPos.store(endRead, std::memory_order_release);
        
        RING_LOG("SkipData: skipped %d bytes, readPos %d->%d", bytes, currentRead, endRead);
        
        if (mSaveFreeSpace != -1) {
            mSaveFreeSpace = std::max(mSaveFreeSpace - bytes, 0);
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
        
        // Calculate how far we can safely rewind (back toward saved position)
        int maxRewind = (currentRead - mSaveReadPos + mBufSize) % mBufSize;
        
        if (bytes > maxRewind) {
            RING_LOG("Rewind: requested %d > max %d", bytes, maxRewind);
            return -1;
        }
        
        int newRead = (currentRead - bytes + mBufSize) % mBufSize;
        mReadPos.store(newRead, std::memory_order_release);
        
        // ✅ UPDATE SAVED FREE SPACE WHEN REWINDING
        if (mSaveFreeSpace != -1) {
            mSaveFreeSpace += bytes;  // Rewinding increases available data from saved position
        }
        
        RING_LOG("Rewind: rewound %d bytes, readPos %d→%d, savedFree now %d",
                 bytes, currentRead, newRead, mSaveFreeSpace);
        return bytes;
    }
    
    
    inline int Offset(int delta) {
        if (delta == 0) return 0;
        
        int currentRead = mReadPos.load(std::memory_order_relaxed);
        int currentWrite = mWritePos.load(std::memory_order_acquire);
        
        int newRead = (currentRead + delta + mBufSize) % mBufSize;
        
        if (delta > 0) {
            // Forward offset - check available data
            int available = (currentWrite - currentRead + mBufSize) % mBufSize;
            if (delta > available) {
                RING_LOG("Offset: forward offset %d > available %d", delta, available);
                return -1;
            }
        } else {
            // Backward offset - check if we have save state
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
        
       if (mSaveFreeSpace != -1) {
           mSaveFreeSpace = std::max(mSaveFreeSpace - delta, 0);
        }
        
        RING_LOG("Offset: moved %d, readPos %d→%d, savedFree now %d", \
                 delta, currentRead, newRead, mSaveFreeSpace);
        return 0;
    }
    
    // ===== DEBUGGING & VALIDATION =====
    
    inline void LogSaveRestoreBalance() const {
        RING_LOG("Save/Restore balance: SaveRead=%d, RestoreRead=%d, InSaveMode=%s", \
                 mSaveReadCallCount, mRestoreReadCallCount,
                 (mSaveReadPos != -1) ? "YES" : "NO");
    }
    
    inline void LogBufferState(const char* _Nonnull context = "") const {
        int currentRead = mReadPos.load(std::memory_order_acquire);
        int currentWrite = mWritePos.load(std::memory_order_acquire);
        int used = UsedSpace();
        int free = FreeSpace();
        
        RING_LOG("Buffer[%s]: size=%d, free=%d, used=%d, read=%d, write=%d, saveMode=%s", \
                 context, mBufSize, free, used, currentRead, currentWrite, \
                 (mSaveReadPos != -1) ? "YES" : "NO");
    }
   
    
    inline void DumpBufferState(const char* context = "") const {
        RING_LOG("PEEK STATE [%s]:", context);
        RING_LOG("  readPos: %d (saved: %d)", mReadPos.load(), mSaveReadPos);
        RING_LOG("  writePos: %d", mWritePos.load());
        RING_LOG("  freeSpace: %d (saved: %d)", FreeSpace(true), mSaveFreeSpace);
        RING_LOG("  usedSpace: %d (saved: %d)", UsedSpace(true), UsedSpace(false));
        RING_LOG("  inPeekMode: %s", (mSaveReadPos != -1) ? "YES" : "NO");
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
    
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&& other) noexcept = delete;
    RingBuffer& operator=(RingBuffer&& other) noexcept = delete;
};

