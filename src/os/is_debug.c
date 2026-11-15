#include "libultra_internal.h"
#include "libc/stdarg.h"
#include <string.h>
#include "printf.h"
#include <PR/os.h>
#include "hardware.h"

// Note: This file was very quickly strewn together from oot.
// Not great but it works.

OSPiHandle* sISVHandle; // official name : is_Handle

typedef struct {
    /* 0x00 */ u32 magic; // IS64
    /* 0x04 */ u32 get;
    /* 0x08 */ u8 unk_08[0x14 - 0x08];
    /* 0x14 */ u32 put;
    /* 0x18 */ u8 unk_18[0x20 - 0x18];
    /* 0x20 */ u8 data[0x10000 - 0x20];
} ISVDbg;

#define gISVDbgPrnAdrs ((ISVDbg*) 0x03FF0000)
#define ASCII_TO_U32(a, b, c, d) ((u32) ((a << 24) | (b << 16) | (c << 8) | (d << 0)))

void* is_proutSyncPrintf(void* arg, const char* str, u32 count);
s32 __osEPiRawWriteIo(OSPiHandle* a0, u32 a1, u32 a2);
void __osPiRelAccess(void);

s32 __osEPiRawReadIo(OSPiHandle* arg0, u32 devAddr, u32* data) {
    register s32 stat;
    while (stat = HW_REG(PI_STATUS_REG, s32), stat & (PI_STATUS_BUSY | PI_STATUS_IOBUSY | PI_STATUS_ERROR)) {
        ;
    }
    *data = HW_REG(arg0->baseAddress | devAddr, s32);
    return 0;
}

s32 osEPiReadIo(OSPiHandle* handle, u32 devAddr, u32* data) {
    register s32 ret;

    __osPiGetAccess();
    ret = __osEPiRawReadIo(handle, devAddr, data);
    __osPiRelAccess();

    return ret;
}

s32 osEPiWriteIo(OSPiHandle* handle, u32 devAddr, u32 data) {
    register s32 ret;

    __osPiGetAccess();
    ret = __osEPiRawWriteIo(handle, devAddr, data);
    __osPiRelAccess();

    return ret;
}

void sc64Init(void);

void isPrintfInit(void) {
    sISVHandle = osCartRomInit();
    osEPiWriteIo(sISVHandle, (u32) &gISVDbgPrnAdrs->put, 0);
    osEPiWriteIo(sISVHandle, (u32) &gISVDbgPrnAdrs->get, 0);
    osEPiWriteIo(sISVHandle, (u32) &gISVDbgPrnAdrs->magic, ASCII_TO_U32('I', 'S', '6', '4'));
    sc64Init();
}

// SummerCart64: receive text lines sent to the stdin of `sc64deployer debug`.
// See docs/02_n64_commands.md of https://github.com/Polprzewodnikowy/SummerCart64
// Addresses are relative to the cart ROM handle (PI 0x10000000).
#define SC64_SCR 0x0FFF0000
#define SC64_DATA0 0x0FFF0004
#define SC64_DATA1 0x0FFF0008
#define SC64_IDENTIFIER 0x0FFF000C
#define SC64_KEY 0x0FFF0010
#define SC64_BUFFER 0x0FFE0000 // 8 KiB data buffer
#define SC64_BUFFER_PI 0x1FFE0000
#define SC64_BUFFER_SIZE 0x2000
#define SC64_SCR_CPU_BUSY (1 << 31)
#define SC64_SCR_CMD_ERROR (1 << 30)
#define SC64_USB_READ_BUSY (1 << 31)
#define SC64_DATATYPE_TEXT 0x01
#define SC64_MAX_POLLS 100000

static s32 sSc64Present; // .bss: mk64.ld has no .data for this file

static s32 sc64Command(u8 id, u32 arg0, u32 arg1, u32* rsp0, u32* rsp1) {
    u32 scr;
    s32 polls = SC64_MAX_POLLS;

    osEPiWriteIo(sISVHandle, SC64_DATA0, arg0);
    osEPiWriteIo(sISVHandle, SC64_DATA1, arg1);
    osEPiWriteIo(sISVHandle, SC64_SCR, id);
    do {
        osEPiReadIo(sISVHandle, SC64_SCR, &scr);
    } while ((scr & SC64_SCR_CPU_BUSY) && --polls > 0);
    if (polls == 0 || (scr & SC64_SCR_CMD_ERROR)) {
        return -1;
    }
    osEPiReadIo(sISVHandle, SC64_DATA0, rsp0);
    osEPiReadIo(sISVHandle, SC64_DATA1, rsp1);
    return 0;
}

void sc64Init(void) {
    u32 identifier;

    osEPiWriteIo(sISVHandle, SC64_KEY, 0x00000000);
    osEPiWriteIo(sISVHandle, SC64_KEY, ASCII_TO_U32('_', 'U', 'N', 'L'));
    osEPiWriteIo(sISVHandle, SC64_KEY, ASCII_TO_U32('O', 'C', 'K', '_'));
    osEPiReadIo(sISVHandle, SC64_IDENTIFIER, &identifier);
    // Not on a SummerCart64 (e.g. an emulator): never touch it again
    sSc64Present = identifier == ASCII_TO_U32('S', 'C', 'v', '2');
}

/**
 * Fetches a pending text line into buf (NUL terminated, at most size - 1 chars).
 * Returns its length, or 0 if there is none. Cheap enough to call every frame.
 * Packets not fetched within 1 s get dropped by the SummerCart64.
 */
s32 sc64ReadText(char* buf, s32 size) {
    u32 status;
    u32 length;
    u32 word;
    u32 type;
    s32 polls = SC64_MAX_POLLS;
    s32 i;

    if (!sSc64Present || sc64Command('u', 0, 0, &status, &length) != 0) {
        return 0;
    }
    type = status & 0xFF;
    if (type == 0 || length == 0 || length > SC64_BUFFER_SIZE) {
        return 0;
    }
    if (sc64Command('m', SC64_BUFFER_PI, length, &word, &word) != 0) {
        return 0;
    }
    do {
        if (sc64Command('u', 0, 0, &status, &word) != 0) {
            return 0;
        }
    } while ((status & SC64_USB_READ_BUSY) && --polls > 0);
    if (polls == 0 || type != SC64_DATATYPE_TEXT) {
        return 0;
    }
    if (length > (u32) (size - 1)) {
        length = size - 1;
    }
    for (i = 0; i < (s32) length; i++) {
        if ((i & 3) == 0) {
            osEPiReadIo(sISVHandle, SC64_BUFFER + i, &word);
        }
        buf[i] = (word >> ((3 - (i & 3)) * 8)) & 0xFF;
    }
    buf[length] = '\0';
    return length;
}

void osSyncPrintfUnused(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    _Printf((char* (*) (char*, const char*, size_t) ) is_proutSyncPrintf, NULL, fmt, args);

    va_end(args);
}

void osSyncPrintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    _Printf((char* (*) (char*, const char*, size_t) ) is_proutSyncPrintf, NULL, fmt, args);

    va_end(args);
}

void print(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    _Printf((char* (*) (char*, const char*, size_t) ) is_proutSyncPrintf, NULL, fmt, args);

    va_end(args);
}

// assumption
void rmonPrintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    _Printf((char* (*) (char*, const char*, size_t) ) is_proutSyncPrintf, NULL, fmt, args);

    va_end(args);
}

void* is_proutSyncPrintf(UNUSED void* arg, const char* str, u32 count) {
    u32 data;
    s32 pos;
    s32 start;
    s32 end;

    osEPiReadIo(sISVHandle, (u32) &gISVDbgPrnAdrs->magic, &data);
    if (data != ASCII_TO_U32('I', 'S', '6', '4')) {
        return (void*) 1;
    }
    osEPiReadIo(sISVHandle, (u32) &gISVDbgPrnAdrs->get, &data);
    pos = data;
    osEPiReadIo(sISVHandle, (u32) &gISVDbgPrnAdrs->put, &data);
    start = data;
    end = start + count;
    if (end >= 0xFFE0) {
        end -= 0xFFE0;
        if (pos < end || start < pos) {
            return (void*) 1;
        }
    } else {
        if (start < pos && pos < end) {
            return (void*) 1;
        }
    }
    while (count) {
        u32 addr = (u32) &gISVDbgPrnAdrs->data + (start & 0xFFFFFFC);
        s32 shift = ((3 - (start & 3)) * 8);

        if (*str) {
            osEPiReadIo(sISVHandle, addr, &data);
            osEPiWriteIo(sISVHandle, addr, (*str << shift) | (data & ~(0xFF << shift)));

            start++;
            if (start >= 0xFFE0) {
                start -= 0xFFE0;
            }
        }
        count--;
        str++;
    }
    osEPiWriteIo(sISVHandle, (u32) &gISVDbgPrnAdrs->put, start);

    return (void*) 1;
}
