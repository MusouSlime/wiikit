// wiikit runtime — the GameCube's memory card: its storage, the EXI device
// that talks to it, and the u-code that unlocks it.
//
// The card is a flash chip on the EXI bus. The SDK reaches it through the
// EXI's channels: slot A is channel 0, device 0; slot B is channel 1,
// device 0; and channel 0, device 1 is the boot ROM's own chip (the RTC and
// the SRAM), which hw.cpp serves.
//
// A card's storage is a file in Dolphin's raw format: the whole card, its
// blocks of 8 KB, without a header of its own. A card that is not there
// yet is created and formatted, as the console's BIOS would format one: a
// header block, two directory blocks and two block-allocation tables, the
// rest empty.
//
// The card is unlocked by a second micro-code, the one the SDK loads into
// the DSP to unlock the card (Dolphin's CARD u-code, hash 0x65d6cc6f). The
// DSP's boot task brings it in, the CPU starts it with 0xFF000000, and it
// answers 0xDCD10003 when the card is unlocked.
//
// This card is unlocked from the start, as it is in Dolphin: the real
// card's handshake (which the micro-code's own work carries out) is not
// needed, and the SDK cannot tell the difference.
#include "mem.h"
#include "rt.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace {

// ---- the card's shape ---------------------------------------------------------
constexpr uint32_t BLOCK_SIZE = 0x2000;      // 8 KB a block
constexpr uint32_t MBIT_TO_BLOCKS = 16;      // blocks per Mbit
constexpr uint32_t MC_FST_BLOCKS = 5;        // the header, two directories, two tables
constexpr uint16_t SIZE_59 = 0x04;           // 64 blocks, 59 of them free
constexpr uint16_t SIZE_251 = 0x10;          // 256 blocks, 251 of them free
constexpr uint16_t CARD_ID = 0xC221;         // a Nintendo card
constexpr uint32_t DIRLEN = 0x7F;            // the files a directory holds
constexpr uint32_t DENTRY_SIZE = 0x40;
constexpr uint16_t BAT_SIZE = 0xFFB;

// the card's status
enum : uint8_t {
    ST_BUSY = 0x80, ST_UNLOCKED = 0x40, ST_SLEEP = 0x20, ST_ERASE_ERROR = 0x10,
    ST_PROGRAM_ERROR = 0x08, ST_READY = 0x01,
};

// the card's commands
enum : uint8_t {
    CMD_NINTENDO_ID = 0x00, CMD_READ_ARRAY = 0x52, CMD_ARRAY_TO_BUFFER = 0x53,
    CMD_SET_INTERRUPT = 0x81, CMD_WRITE_BUFFER = 0x82, CMD_READ_STATUS = 0x83,
    CMD_READ_ID = 0x85, CMD_READ_ERROR_BUFFER = 0x86, CMD_WAKE_UP = 0x87,
    CMD_SLEEP = 0x88, CMD_CLEAR_STATUS = 0x89, CMD_SECTOR_ERASE = 0xF1,
    CMD_PAGE_PROGRAM = 0xF2, CMD_EXTRA_BYTE_PROGRAM = 0xF3, CMD_CHIP_ERASE = 0xF4,
};

uint16_t get_be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
void put_be16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
uint16_t swap16(uint16_t v) { return (uint16_t)(v >> 8 | v << 8); }
uint32_t get_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

// The card's checksums: the sum of an area's 16-bit words and the sum of
// their inverses. The card keeps both in its own order (big-endian); the
// console compares the two byte-swapped, and a sum of 0xFFFF means none.
void checksums(const uint8_t* data, size_t size, uint16_t& sum, uint16_t& inv) {
    uint16_t s = 0, i = 0;
    for (size_t k = 0; k + 1 < size; k += 2) {
        uint16_t w = get_be16(data + k);
        s += w;
        i += (uint16_t)(w ^ 0xFFFF);
    }
    sum = s;
    inv = i;
    if (swap16(s) == 0xFFFF) sum = 0;
    if (swap16(i) == 0xFFFF) inv = 0;
}

// ---- one slot ------------------------------------------------------------------
struct Slot {
    std::vector<uint8_t> data;                // the whole card
    uint16_t size_code = SIZE_251;            // the card's size, in the code the card answers with
    std::string path;                         // the file it is in

    // the card's state
    uint8_t command = CMD_NINTENDO_ID;
    uint32_t position = 0;                    // the byte's place in the command
    uint32_t address = 0;                     // the address the command carries
    uint8_t status = ST_BUSY | ST_UNLOCKED | ST_READY;
    int interrupt_switch = 0;
    bool interrupt_set = false;
    uint8_t programming[128] = {};            // the bytes a page program waits in

    bool present() const { return !data.empty(); }
    uint8_t* block(uint32_t n) { return data.data() + (size_t)n * BLOCK_SIZE; }
    uint32_t size() const { return (uint32_t)data.size(); }
};

Slot g_slots[2];                             // 0: slot A (channel 0), 1: slot B (channel 1)

// ---- the card's storage ----------------------------------------------------------
// Make the directory the cards are in, if it is not there.
void make_dir(const char* dir) {
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
}

// The header block: the serial, the time of the format, and the size.
void format_header(Slot& s, uint64_t format_time) {
    uint8_t* h = s.block(0);
    std::memset(h, 0xFF, BLOCK_SIZE);

    // The serial: twelve bytes from the format time, the way Nintendo's
    // algorithm does it (the SDK's own flash id is not there to mix in).
    uint64_t rand = format_time;
    for (int i = 0; i < 12; ++i) {
        rand = ((rand * 0x0000000041C64E6DULL) + 0x0000000000003039ULL) >> 16;
        h[i] = (uint8_t)rand;
        rand = ((rand * 0x0000000041C64E6DULL) + 0x0000000000003039ULL) >> 16;
        rand &= 0x7FFF;
    }
    put_be32(h + 0x0C, (uint32_t)(format_time >> 32));    // the time of the format
    put_be32(h + 0x10, (uint32_t)format_time);
    put_be32(h + 0x14, 0);                                // the SRAM's bias
    put_be32(h + 0x18, 1);                                // the SRAM's language: English
    put_be32(h + 0x1C, 0);                                // the VI's DTV status
    put_be16(h + 0x20, 0);                                // the device: slot A
    put_be16(h + 0x22, s.size_code);                      // the size, in Mbits
    put_be16(h + 0x24, 0);                                // the encoding: Windows-1252
    put_be16(h + 0x1FA, 0xFFFF);                           // the update counter
    uint16_t sum, inv;
    checksums(h, 0x1FC, sum, inv);
    put_be16(h + 0x1FC, sum);
    put_be16(h + 0x1FE, inv);
}

// The directory block: the files, empty for a new card.
void format_directory(Slot& s, uint32_t n) {
    uint8_t* d = s.block(n);
    std::memset(d, 0xFF, BLOCK_SIZE);
    put_be16(d + 0x1FFA, 0);                              // the update counter
    uint16_t sum, inv;
    checksums(d, 0x1FFC, sum, inv);
    put_be16(d + 0x1FFC, sum);
    put_be16(d + 0x1FFE, inv);
}

// The block allocation table: which blocks are free.
void format_bat(Slot& s, uint32_t n) {
    uint8_t* b = s.block(n);
    std::memset(b, 0, BLOCK_SIZE);
    put_be16(b + 0x00, 0);                                // the checksums, below
    put_be16(b + 0x02, 0);
    put_be16(b + 0x04, 0);                                // the update counter
    put_be16(b + 0x06, (uint16_t)(s.size_code * MBIT_TO_BLOCKS - MC_FST_BLOCKS));   // the free blocks
    put_be16(b + 0x08, 4);                                // the last block allocated
    uint16_t sum, inv;
    checksums(b + 0x04, BLOCK_SIZE - 0x04, sum, inv);
    put_be16(b + 0x00, sum);
    put_be16(b + 0x02, inv);
}

void format_card(Slot& s, uint64_t format_time) {
    format_header(s, format_time);
    format_directory(s, 1);
    format_directory(s, 2);
    format_bat(s, 3);
    format_bat(s, 4);
}

bool write_file(Slot& s) {
    FILE* f = std::fopen(s.path.c_str(), "wb");
    if (!f) {
        rt_log("memcard: cannot write %s", s.path.c_str());
        return false;
    }
    std::fwrite(s.data.data(), 1, s.data.size(), f);
    std::fclose(f);
    return true;
}

// Load the card in the file, or make a new one.
void load_card(Slot& s, const std::string& path, uint16_t size_code, uint64_t format_time) {
    s.path = path;
    s.size_code = size_code;
    const uint32_t want = size_code * MBIT_TO_BLOCKS * BLOCK_SIZE;

    std::vector<uint8_t> d;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        d.resize((size_t)std::ftell(f));
        std::fseek(f, 0, SEEK_SET);
        if (d.size() != want) d.clear();
        if (!d.empty() && std::fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
        std::fclose(f);
    }
    if (d.empty()) {
        s.data.assign(want, 0xFF);
        format_card(s, format_time);
        rt_log("memcard: %s: a new card, %u blocks", path.c_str(), want / BLOCK_SIZE);
        write_file(s);
    } else {
        s.data = std::move(d);
        rt_log("memcard: %s: %u blocks", path.c_str(), want / BLOCK_SIZE);
    }
}

// ---- the card's commands ---------------------------------------------------------
// One byte through the card: the command comes first, then its arguments,
// then the data. The card answers 0xFF where it has nothing to say.
uint8_t card_byte(Slot& s, uint8_t in) {
    uint8_t out = 0xFF;
    if (s.position == 0) {
        s.command = in;
        if (s.command == CMD_CLEAR_STATUS) {
            s.status &= ~(ST_PROGRAM_ERROR | ST_ERASE_ERROR);
            s.status |= ST_READY;
            s.interrupt_set = false;
            s.position = 0;
            return 0xFF;
        }
        s.position = 1;
        return 0xFF;
    }
    switch (s.command) {
    case CMD_NINTENDO_ID:
        // The size, in the code the card answers with: the dummy cycle,
        // then the four bytes of the size.
        if (s.position == 1) out = 0x80;
        else out = (uint8_t)(s.size_code >> (24 - (((s.position - 2) & 3) * 8)));
        break;
    case CMD_READ_ARRAY: {
        if (s.position == 1) { s.address = (uint32_t)in << 17; out = 0xFF; break; }
        if (s.position == 2) { s.address |= (uint32_t)in << 9; break; }
        if (s.position == 3) { s.address |= (uint32_t)(in & 3) << 7; break; }
        if (s.position == 4) { s.address |= (uint32_t)(in & 0x7F); break; }
        if (s.position > 1) {
            uint32_t a = s.address & (s.size() - 1);
            out = a < s.size() ? s.data[a] : 0xFF;
            // the address moves on, within the block
            if (s.position >= 9) s.address = (s.address & ~0x1FF) | ((s.address + 1) & 0x1FF);
        }
        break;
    }
    case CMD_READ_STATUS:
        out = s.status;
        break;
    case CMD_READ_ID:
        out = s.position == 1 ? (uint8_t)(CARD_ID >> 8) : (uint8_t)CARD_ID;
        break;
    case CMD_SET_INTERRUPT:
        if (s.position == 1) s.interrupt_switch = in;
        out = 0xFF;
        break;
    case CMD_SECTOR_ERASE:
        if (s.position == 1) s.address = (uint32_t)in << 17;
        else if (s.position == 2) s.address |= (uint32_t)in << 9;
        out = 0xFF;
        break;
    case CMD_PAGE_PROGRAM:
        if (s.position == 1) s.address = (uint32_t)in << 17;
        else if (s.position == 2) s.address |= (uint32_t)in << 9;
        else if (s.position == 3) s.address |= (uint32_t)(in & 3) << 7;
        else if (s.position == 4) s.address |= (uint32_t)(in & 0x7F);
        if (s.position >= 5) s.programming[(s.position - 5) & 0x7F] = in;
        out = 0xFF;
        break;
    case CMD_CHIP_ERASE:
        out = 0xFF;
        break;
    default:
        out = 0xFF;
        break;
    }
    ++s.position;
    return out;
}

// The chip was released: the card does the work that waits for it.
void card_deselect(Slot& s) {
    if (s.command == CMD_SECTOR_ERASE) {
        if (s.position > 2) {
            uint32_t a = s.address & (s.size() - 1) & ~(BLOCK_SIZE - 1);
            for (uint32_t k = 0; k < BLOCK_SIZE && a + k < s.size(); ++k) s.data[a + k] = 0xFF;
            s.status |= ST_BUSY;
            s.status &= ~ST_READY;
        }
    } else if (s.command == CMD_PAGE_PROGRAM) {
        if (s.position >= 5) {
            uint32_t count = s.position - 5;
            s.status &= ~ST_BUSY;
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t a = s.address & (s.size() - 1);
                if (a < s.size()) s.data[a] = s.programming[i & 0x7F];
                s.address = (s.address & ~0x1FF) | ((s.address + 1) & 0x1FF);
            }
            write_file(s);
        }
    } else if (s.command == CMD_CHIP_ERASE) {
        if (s.position > 2) {
            std::memset(s.data.data(), 0xFF, s.size());
            format_card(s, 0);
            write_file(s);
        }
    }
}

}  // namespace

// ---- the runtime's side ----------------------------------------------------------------

bool memcard_init(const char* dir) {
    // The directory the cards are in, made if it is not there.
    make_dir(dir);
    // The GameCube's clock: the seconds since 2000, the epoch the card's
    // header counts in.
    const uint64_t now = (uint64_t)std::time(nullptr) - 946684800;
    const std::string base = std::string(dir) + "/";
    load_card(g_slots[0], base + "A.raw", SIZE_251, now);
    load_card(g_slots[1], base + "B.raw", SIZE_251, now);
    return true;
}

bool memcard_present(int slot) { return slot >= 0 && slot < 2 && g_slots[slot].present(); }

uint8_t memcard_byte(int slot, uint8_t in) {
    if (slot < 0 || slot >= 2) return 0xFF;
    return card_byte(g_slots[slot], in);
}

// The card's data, read or written in one go (the EXI's DMA). The address
// is the guest's.
void memcard_dma_read(int slot, uint32_t addr, uint32_t size) {
    if (slot < 0 || slot >= 2) return;
    Slot& s = g_slots[slot];
    for (uint32_t i = 0; i < size; ++i) {
        uint32_t a = (s.address + i) & (s.size() - 1);
        uint8_t b = 0xFF;
        if (a < s.size()) b = s.data[a];
        mem_write(addr + i, &b, 1);
    }
}

void memcard_dma_write(int slot, uint32_t addr, uint32_t size) {
    if (slot < 0 || slot >= 2) return;
    Slot& s = g_slots[slot];
    for (uint32_t i = 0; i < size; ++i) {
        uint32_t a = (s.address + i) & (s.size() - 1);
        if (a >= s.size()) continue;
        uint8_t b = 0xFF;
        mem_read(addr + i, &b, 1);
        s.data[a] = b;
    }
    write_file(s);
}

void memcard_deselect(int slot) {
    if (slot < 0 || slot >= 2) return;
    card_deselect(g_slots[slot]);
}

// The chip was selected: the card starts a new command.
void memcard_select(int slot) {
    if (slot < 0 || slot >= 2) return;
    g_slots[slot].position = 0;
}

// The card wants the CPU's attention: the EXI's interrupt.
bool memcard_interrupt(int slot) {
    if (slot < 0 || slot >= 2) return false;
    Slot& s = g_slots[slot];
    return s.interrupt_switch && s.interrupt_set;
}
