#pragma once
#include <cstdint>
namespace hk3ds
{
// Save container: two fixed slots written in place, alternately. A slot is a
// header followed by the event record; the CRC-32C covers magic, sequence,
// payloadBytes and the payload. The newest valid slot wins on load, so a torn
// write can only damage the slot that did not hold the current save.
// Decision and measurements: docs/architecture.md "Save slots".
struct SaveSlotHeader {char magic[4];std::uint32_t sequence,payloadBytes,crc;};
inline constexpr unsigned saveSlotStride=4096,saveSlotCopies=2;
inline constexpr unsigned saveContainerBytes=saveSlotStride*saveSlotCopies;
inline constexpr unsigned saveSlotPayloadLimit=saveSlotStride-sizeof(SaveSlotHeader);

// Storage adapter, replaceable in host tests to inject torn and failed writes.
struct SaveStorage
{
    virtual ~SaveStorage()=default;
    // Reads at most capacity bytes. Returns the file size (capacity+1 when the
    // file is longer), or -1 when the file is missing or unreadable.
    virtual long read(const char* path,unsigned char* out,unsigned capacity)=0;
    // One write into an existing file; never changes its size.
    virtual bool writeAt(const char* path,unsigned offset,const unsigned char* data,unsigned bytes)=0;
    // Whole-file replacement that keeps the previous file (or its .bak) until
    // the new one is installed. Slow on SD: creation and migration only.
    virtual bool replace(const char* path,const unsigned char* data,unsigned bytes)=0;
};
SaveStorage& stdioSaveStorage();

// Where the next save goes. Unknown cursors are rebuilt by reading the file; a
// failed write forgets the cursor so the next attempt re-reads what survived.
struct SaveCursor
{
    bool known=false,inPlace=false;
    std::uint8_t target=0;
    std::uint32_t sequence=0; // sequence the next write will carry
};

// Optional per-operation timing of the stdio adapter (console diagnostics).
struct SaveOperationTicks {std::uint64_t open=0,write=0,close=0;bool replaced=false;};
void probeSaveStorage(std::uint64_t (*clock)(),SaveOperationTicks* result);

// A save never starts while a gate transition is pending: the room load that
// follows must not share the SD card with it. The revision stays uncommitted,
// so the save starts on the first frame after the load.
constexpr bool saveMayStart(bool transitionPending) {return !transitionPending;}
}
