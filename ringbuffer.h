/*
    Millennium Sound System
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

class RingBuffer
{
private:
    struct BufferSnapshot {
        int bufSize;
        int freeSpace;
        int saveFreeSpace;
        int saveReadPos;
        int readPos;
        int writePos;
        
        inline bool InSaveMode() const { return saveFreeSpace != -1; }
        inline int EffectiveFreeSpace() const { return InSaveMode() ? saveFreeSpace : freeSpace; }
        inline int UsedSpace() const { return bufSize - EffectiveFreeSpace(); }
    };
    
    std::atomic<int>            mFreeSpace{0};
    std::atomic<int>            mReadPos{0};
    std::atomic<int>            mWritePos{0};
    std::atomic<int>            mSaveFreeSpace{-1};
    std::atomic<int>            mSaveReadPos{-1};
    std::atomic<int>            mBufSize{0};
   
    mutable int                 mSaveReadCallCount{0};
    mutable int                 mRestoreReadCallCount{0};
    
    std::unique_ptr<uint8_t[]>  mBuffer;
    inline BufferSnapshot GetAtomicSnapshot() const {
        std::atomic_thread_fence(std::memory_order_acquire);
        
        BufferSnapshot snapshot;
        snapshot.bufSize = mBufSize.load(std::memory_order_acquire);
        snapshot.freeSpace = mFreeSpace.load(std::memory_order_acquire);
        snapshot.readPos = mReadPos.load(std::memory_order_acquire);
        snapshot.writePos = mWritePos.load(std::memory_order_acquire);
        snapshot.saveFreeSpace = mSaveFreeSpace.load(std::memory_order_acquire);
        snapshot.saveReadPos = mSaveReadPos.load(std::memory_order_acquire);
    
        return snapshot;
    }
    
    inline void UpdateAtomicState(const BufferSnapshot &snapshot) {
        std::atomic_thread_fence(std::memory_order_acq_rel);
        
        // Store with detailed logging
        mFreeSpace.store(snapshot.freeSpace, std::memory_order_release);
        mReadPos.store(snapshot.readPos, std::memory_order_release);
        mWritePos.store(snapshot.writePos, std::memory_order_release);
        mSaveFreeSpace.store(snapshot.saveFreeSpace, std::memory_order_release);
        mSaveReadPos.store(snapshot.saveReadPos, std::memory_order_release);
        
        std::atomic_thread_fence(std::memory_order_release);
        
        // Read back immediately to confirm
        int verifyFreeSpace = mFreeSpace.load(std::memory_order_acquire);
        int verifyReadPos = mReadPos.load(std::memory_order_acquire);
        int verifyWritePos = mWritePos.load(std::memory_order_acquire);
    }
    
public:
    
    explicit RingBuffer(int size = 1024)
    {
        if (Init(size) < 0) {
            RING_LOG("Failed to initialize buffer");
            throw std::runtime_error("Buffer initialization failed");
        }
    }
    
    inline int Init(int inSize) {
        try {
            mBuffer = std::unique_ptr<uint8_t[]>(new uint8_t[inSize]);
            mBufSize.store(inSize, std::memory_order_release);
            Empty();
            return 0;
        } catch (const std::bad_alloc&) {
            RING_LOG("Memory allocation failed");
            return -1;
        }
    }
    
    inline int FreeSpace(bool inAfterMarker = true) const {
        const BufferSnapshot snapshot = GetAtomicSnapshot();
        return snapshot.freeSpace;
    }
    
    inline int UsedSpace(bool inAfterMarker = true) const {
        const BufferSnapshot snapshot = GetAtomicSnapshot();
        int result = snapshot.bufSize - snapshot.freeSpace;
        return result;
    }
       
    
    inline int ReadData(void *_Nullable data, int bytes) {
        BufferSnapshot snapshot = GetAtomicSnapshot();
        
        int usedSpace = snapshot.UsedSpace();
        if ((bytes = std::min(usedSpace, bytes)) == 0)
            return 0;
    
        int endRead = (snapshot.readPos + bytes) % snapshot.bufSize;
        
        if (data) {
            if (endRead > snapshot.readPos) {
                std::memcpy(data, &mBuffer[snapshot.readPos], bytes);
            } else {
                int firstPartSize = snapshot.bufSize - snapshot.readPos;
                if (firstPartSize > 0 && firstPartSize <= bytes) {
                    std::memcpy(data, &mBuffer[snapshot.readPos], firstPartSize);
                    if (endRead > 0) {
                        std::memcpy(static_cast<uint8_t*>(data) + firstPartSize, &mBuffer[0], endRead);
                    }
                } else {
                    int safeSize = std::min(bytes, firstPartSize);
                    std::memcpy(data, &mBuffer[snapshot.readPos], safeSize);
                }
            }
        }
        
        snapshot.readPos = endRead;
        snapshot.freeSpace += bytes;
        
        UpdateAtomicState(snapshot);
        
        return bytes;
    }
    
    inline int WriteData(const void*_Nullable data, int bytes) {
        BufferSnapshot snapshot = GetAtomicSnapshot();
       
        int freeSpace = snapshot.EffectiveFreeSpace();
        if ((bytes = std::min(freeSpace, bytes)) == 0) {
            RING_LOG("WriteData: EXIT - no space available (freeSpace=%d)", freeSpace);
            return 0;
        }
        
        int endWrite = (snapshot.writePos + bytes) % snapshot.bufSize;
        if (data) {
            if (endWrite > snapshot.writePos) {
                std::memcpy(&mBuffer[snapshot.writePos], data, bytes);
            } else {
                int firstPartSize = snapshot.bufSize - snapshot.writePos;
                std::memcpy(&mBuffer[snapshot.writePos], data, firstPartSize);
                std::memcpy(&mBuffer[0], static_cast<const uint8_t*>(data) + firstPartSize, endWrite);
            }
        }
        
        snapshot.writePos = endWrite;
        snapshot.freeSpace -= bytes;
        
        UpdateAtomicState(snapshot);
        
        return bytes;
    }
    
    inline int BufSize() const {
        return mBufSize.load(std::memory_order_acquire);
    }
    
    inline bool IsReadMode() const {
        return mSaveFreeSpace.load(std::memory_order_acquire) == -1;
    }
    
    inline void Empty() {
        BufferSnapshot snapshot = GetAtomicSnapshot();
        
        snapshot.readPos = 0;
        snapshot.writePos = 0;
        snapshot.freeSpace = snapshot.bufSize;
        snapshot.saveFreeSpace = -1;
        snapshot.saveReadPos = -1;
        
        UpdateAtomicState(snapshot);
    }
    
    inline void SaveRead() {
        mSaveReadCallCount++;
        
        BufferSnapshot snapshot = GetAtomicSnapshot();
        snapshot.saveReadPos = snapshot.readPos;
        snapshot.saveFreeSpace = snapshot.freeSpace;
        UpdateAtomicState(snapshot);
    }
    
    inline void RestoreRead() {
        BufferSnapshot snapshot = GetAtomicSnapshot();
        
        if (snapshot.saveFreeSpace == -1) {
            RING_LOG("RestoreRead called but no save state exists!");
            return;
        }
        
        mRestoreReadCallCount++;
      
        int savedWritePos = snapshot.writePos; // Current writePos
        int bytesWrittenSinceSave = (savedWritePos - snapshot.saveReadPos + snapshot.bufSize) % snapshot.bufSize;
        
        if (bytesWrittenSinceSave > 0) {
            // Adjust the saved freeSpace to account for data written since save
            int adjustedSaveFreeSpace = snapshot.saveFreeSpace - bytesWrittenSinceSave;
            if (adjustedSaveFreeSpace < 0) adjustedSaveFreeSpace = 0;
            
            snapshot.readPos = snapshot.saveReadPos;
            snapshot.freeSpace = adjustedSaveFreeSpace;
        } else {
            // No data written since save - normal restore
            snapshot.readPos = snapshot.saveReadPos;
            snapshot.freeSpace = snapshot.saveFreeSpace;
        }
        
        // Clear save state
        snapshot.saveFreeSpace = -1;
        snapshot.saveReadPos = -1;
        
        UpdateAtomicState(snapshot);
        
        // Verify the restore didn't create inconsistent state
        BufferSnapshot verifySnapshot = GetAtomicSnapshot();
        int usedSpace = verifySnapshot.bufSize - verifySnapshot.freeSpace;
        int expectedUsed = (verifySnapshot.writePos - verifySnapshot.readPos + verifySnapshot.bufSize) % verifySnapshot.bufSize;
        
        if (std::abs(usedSpace - expectedUsed) > 1) {  // Allow 1 byte tolerance
            RING_LOG("RestoreRead created inconsistent state!");
            RING_LOG("   Calculated used: %d, Expected used: %d", usedSpace, expectedUsed);
            RING_LOG("   readPos: %d, writePos: %d, freeSpace: %d",
                     verifySnapshot.readPos, verifySnapshot.writePos, verifySnapshot.freeSpace);
        }
    }
    
    inline void ClearSaveState() {
        BufferSnapshot snapshot = GetAtomicSnapshot();
        
        if (snapshot.saveFreeSpace != -1) {
            snapshot.saveFreeSpace = -1;
            snapshot.saveReadPos = -1;
            UpdateAtomicState(snapshot);
        }
    }
    inline int PeekData(void* dst, int bytes) const {
        const BufferSnapshot snapshot = GetAtomicSnapshot();
        
        int usedSpace = snapshot.UsedSpace();
        if (usedSpace < bytes) {
            return -1;
        }
        
        if (snapshot.readPos < 0 || snapshot.readPos >= snapshot.bufSize)
            return -1;
        
        int contiguous = std::min(bytes, snapshot.bufSize - snapshot.readPos);
        std::memcpy(static_cast<uint8_t*>(dst), &mBuffer[snapshot.readPos], contiguous);
        
        if (bytes > contiguous) {
            std::memcpy(&static_cast<uint8_t*>(dst)[contiguous], &mBuffer[0], bytes - contiguous);
        }
        
        return bytes;
    }
    
    inline int Rewind(int bytes) {
        BufferSnapshot snapshot = GetAtomicSnapshot();
        
        int maxRewind;
        if (snapshot.saveReadPos != -1) {
            maxRewind = (snapshot.readPos - snapshot.saveReadPos + snapshot.bufSize) % snapshot.bufSize;
        } else {
            int usedSpace = (snapshot.writePos - snapshot.readPos + snapshot.bufSize) % snapshot.bufSize;
            int totalBufferUsed = snapshot.bufSize - snapshot.freeSpace;
            maxRewind = totalBufferUsed - usedSpace;
            if (maxRewind < 0) maxRewind = 0;
        }
        
        if (bytes > maxRewind) {
            return -maxRewind;
        }
        
        int newReadPos = (snapshot.readPos - bytes + snapshot.bufSize) % snapshot.bufSize;
        if (newReadPos < 0 || newReadPos >= snapshot.bufSize) {
            return -maxRewind;
        }
        
        // Update snapshot and write back
        snapshot.readPos = newReadPos;
        snapshot.freeSpace += bytes;
        
        UpdateAtomicState(snapshot);
        
        return bytes;
    }
    
    inline int SkipData(int bytes) {
        BufferSnapshot snapshot = GetAtomicSnapshot();
        
        int usedSpace = snapshot.UsedSpace();
        if (usedSpace < bytes) {
            return -1;  // Not enough data to skip
        }
        
        // Calculate new read position (same as ReadData)
        int newReadPos = (snapshot.readPos + bytes) % snapshot.bufSize;
        
        // Update snapshot (same as ReadData but no memcpy)
        snapshot.readPos = newReadPos;
        snapshot.freeSpace += bytes;
        
        UpdateAtomicState(snapshot);
        
        return bytes;
    }
    
    inline int Offset(int delta) {
        BufferSnapshot snapshot = GetAtomicSnapshot();
        
        int usedSpace = snapshot.UsedSpace();
        
        if (delta >= 0 && delta <= usedSpace) {
            int newReadPos = (snapshot.readPos + delta) % snapshot.bufSize;
            if (newReadPos < 0 || newReadPos >= snapshot.bufSize) {
                return -1;
            }
            
            // Update snapshot
            snapshot.readPos = newReadPos;
            snapshot.freeSpace += delta;
            
            UpdateAtomicState(snapshot);
            return 0;
        }
        return -2;
    }
    
    inline bool CanOffset(int offset) const {
        const BufferSnapshot snapshot = GetAtomicSnapshot();
        
        // Check if we can offset by this amount without going past write position
        int newReadPos = (snapshot.readPos + offset + snapshot.bufSize) % snapshot.bufSize;
        int distanceToWrite = (snapshot.writePos - newReadPos + snapshot.bufSize) % snapshot.bufSize;
        
        return (offset <= snapshot.UsedSpace()) && (distanceToWrite >= 0);
    }
    
    inline void LogSaveRestoreBalance() const {
        RING_LOG("Save/Restore balance: SaveRead=%d, RestoreRead=%d, InSaveMode=%s",
                 mSaveReadCallCount, mRestoreReadCallCount, (mSaveFreeSpace.load() != -1) ? "YES" : "NO");
    }
    
    inline void LogBufferState(const char*_Nonnull context = "") const {
        const BufferSnapshot snapshot = GetAtomicSnapshot();
        
        RING_LOG("Buffer[%s]: size=%d, free=%d, used=%d, read=%d, write=%d, saveMode=%s",
                 context,
                 snapshot.bufSize,
                 snapshot.EffectiveFreeSpace(),
                 snapshot.UsedSpace(),
                 snapshot.readPos,
                 snapshot.writePos,
                 snapshot.InSaveMode() ? "YES" : "NO");
    }
    
    
    inline void CheckSaveRestoreUsage() {
        RING_LOG("Save/Restore Usage Analysis:");
        RING_LOG("   SaveRead calls: %d", mSaveReadCallCount);
        RING_LOG("   RestoreRead calls: %d", mRestoreReadCallCount);
        RING_LOG("   Currently in save mode: %s", (mSaveFreeSpace.load() != -1) ? "YES" : "NO");
        
        if (mSaveReadCallCount > mRestoreReadCallCount + 1) {
            RING_LOG("SaveRead/RestoreRead imbalance detected!");
            RING_LOG("   This suggests a logic error in peek/read mode switching");
        }
    }
    
    inline void DumpBufferState(const char* context) const {
        const BufferSnapshot snapshot = GetAtomicSnapshot();
        
        RING_LOG("BUFFER DUMP [%s]:", context);
        RING_LOG("  bufSize: %d", snapshot.bufSize);
        RING_LOG("  freeSpace: %d", snapshot.freeSpace);
        RING_LOG("  saveFreeSpace: %d", snapshot.saveFreeSpace);
        RING_LOG("  readPos: %d", snapshot.readPos);
        RING_LOG("  writePos: %d", snapshot.writePos);
        RING_LOG("  saveReadPos: %d", snapshot.saveReadPos);
        RING_LOG("  InSaveMode: %s", snapshot.InSaveMode() ? "YES" : "NO");
        RING_LOG("  EffectiveFreeSpace: %d", snapshot.EffectiveFreeSpace());
        RING_LOG("  UsedSpace: %d", snapshot.UsedSpace());
        RING_LOG("  Buffer health: %s", ValidateSnapshot(snapshot, context) ? "OK" : "BROKEN");
    }
    
protected:
    
    // Disable copy operations
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&& other) noexcept = delete;
    RingBuffer& operator=(RingBuffer&& other) noexcept = delete;
};
