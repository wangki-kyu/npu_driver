#pragma once
#include <vector>
#include <string>
#include <fstream>
#include <cstdint>
#include <cstring>

// FlatBuffers read-only binary parser — no flatbuffers library dependency.
// Works with any C++ standard and doesn't conflict with Windows.h macros.

namespace fb_raw {

static inline uint32_t rd_u32(const uint8_t* p) {
    uint32_t v; std::memcpy(&v, p, 4); return v;
}
static inline uint16_t rd_u16(const uint8_t* p) {
    uint16_t v; std::memcpy(&v, p, 2); return v;
}
static inline int16_t rd_i16(const uint8_t* p) {
    int16_t v; std::memcpy(&v, p, 2); return v;
}
static inline int32_t rd_i32(const uint8_t* p) {
    int32_t v; std::memcpy(&v, p, 4); return v;
}

// Follow root offset: buf[0..3] is offset from buf to root table
static inline const uint8_t* get_root(const uint8_t* buf) {
    return buf + rd_u32(buf);
}

// vtable is at table_ptr - *(int32_t*)table_ptr
static inline const uint8_t* get_vtable(const uint8_t* tbl) {
    int32_t soff = rd_i32(tbl);
    return tbl - soff;
}

// field_id = (VT_const - 4) / 2  (VT_4=0, VT_6=1, VT_8=2, VT_10=3, ...)
// Returns 0 if field not present.
static inline uint16_t vt_field(const uint8_t* tbl, int field_id) {
    const uint8_t* vt = get_vtable(tbl);
    uint16_t vtsize  = rd_u16(vt);
    uint16_t needed  = (uint16_t)(4 + field_id * 2);
    if (needed + 2 > vtsize) return 0;
    return rd_u16(vt + needed);
}

// Follow indirect offset for vector/table/string fields
static inline const uint8_t* field_indirect(const uint8_t* tbl, int field_id) {
    uint16_t off = vt_field(tbl, field_id);
    if (!off) return nullptr;
    const uint8_t* fp = tbl + off;
    uint32_t delta = rd_u32(fp);
    return delta ? fp + delta : nullptr;
}

// Read inline int16 scalar field
static inline int16_t field_i16(const uint8_t* tbl, int field_id, int16_t def = 0) {
    uint16_t off = vt_field(tbl, field_id);
    if (!off) return def;
    return rd_i16(tbl + off);
}

// Read inline int32 scalar field
static inline int32_t field_i32(const uint8_t* tbl, int field_id, int32_t def = 0) {
    uint16_t off = vt_field(tbl, field_id);
    if (!off) return def;
    return rd_i32(tbl + off);
}

// Vector element count (first 4 bytes of vector)
static inline uint32_t vec_len(const uint8_t* vec) {
    return vec ? rd_u32(vec) : 0;
}

// Pointer to raw bytes in a byte vector (Vector<uint8_t>) or String
static inline const uint8_t* vec_bytes(const uint8_t* vec) {
    return vec ? (vec + 4) : nullptr;
}

// Get element i from a vector-of-offsets (Vector<Offset<T>>)
// Each element is uint32_t offset relative to element's address
static inline const uint8_t* vec_table(const uint8_t* vec, uint32_t i) {
    if (!vec || i >= vec_len(vec)) return nullptr;
    const uint8_t* ep = vec + 4 + i * 4;
    uint32_t off = rd_u32(ep);
    return off ? ep + off : nullptr;
}

} // namespace fb_raw

namespace apex {

// Description values (from executable.fbs)
// BASE_ADDRESS_OUTPUT_ACTIVATION = 0
// BASE_ADDRESS_INPUT_ACTIVATION  = 1
static const int DESC_OUTPUT = 0;
static const int DESC_INPUT  = 1;

// Position values
// LOWER_32BIT = 0
// UPPER_32BIT = 1
static const int POS_LOWER = 0;
static const int POS_UPPER = 1;

struct FieldPatch {
    int32_t offset_bit;
    int     desc;      // DESC_INPUT or DESC_OUTPUT
    int     position;  // POS_LOWER or POS_UPPER
};

struct ApexModel {
    std::vector<uint8_t>    bitstream;   // mutable copy of instruction bitstream
    std::vector<FieldPatch> patches;     // patch locations
};

inline uint64_t PageAlignUp(uint64_t n) { return (n + 4095ULL) & ~4095ULL; }

// VTable field IDs (= (VT_const - 4) / 2)
// Package:
//   VT_SERIALIZED_MULTI_EXECUTABLE = 6  → id = 1
// MultiExecutable:
//   VT_SERIALIZED_EXECUTABLES = 4       → id = 0
// Executable:
//   VT_INSTRUCTION_BITSTREAMS = 14      → id = 5
// InstructionBitstream:
//   VT_BITSTREAM = 4                    → id = 0
//   VT_FIELD_OFFSETS = 6                → id = 1
// FieldOffset:
//   VT_META = 4                         → id = 0
//   VT_OFFSET_BIT = 6                   → id = 1
// Meta:
//   VT_DESC = 4                         → id = 0
//   VT_POSITION = 10                    → id = 3

inline ApexModel LoadModel(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};

    std::vector<uint8_t> raw((size_t)f.tellg());
    f.seekg(0);
    f.read(reinterpret_cast<char*>(raw.data()), (std::streamsize)raw.size());

    // 1. Package root
    const uint8_t* pkg = fb_raw::get_root(raw.data());

    // 2. serialized_multi_executable (Vector<uint8_t>) - field_id 1
    const uint8_t* multi_vec = fb_raw::field_indirect(pkg, 1);
    if (!multi_vec) return {};
    const uint8_t* multi_buf = fb_raw::vec_bytes(multi_vec);

    // 3. MultiExecutable root (nested FlatBuffer)
    const uint8_t* multi = fb_raw::get_root(multi_buf);

    // 4. serialized_executables (Vector<Offset<String>>) - field_id 0
    const uint8_t* execs_vec = fb_raw::field_indirect(multi, 0);
    if (!execs_vec || fb_raw::vec_len(execs_vec) == 0) return {};

    // 5. First serialized executable: a String (4-byte length + FlatBuffer bytes)
    const uint8_t* ser_exec_str = fb_raw::vec_table(execs_vec, 0);
    if (!ser_exec_str) return {};
    const uint8_t* exec_buf = ser_exec_str + 4; // skip String length field

    // 6. Executable root (nested FlatBuffer)
    const uint8_t* exec = fb_raw::get_root(exec_buf);

    // 7. instruction_bitstreams (Vector<Offset<InstructionBitstream>>) - field_id 5
    const uint8_t* ibs_vec = fb_raw::field_indirect(exec, 5);
    if (!ibs_vec || fb_raw::vec_len(ibs_vec) == 0) return {};

    // 8. First InstructionBitstream
    const uint8_t* ibs = fb_raw::vec_table(ibs_vec, 0);
    if (!ibs) return {};

    // 9. bitstream bytes (Vector<uint8_t>) - field_id 0
    const uint8_t* bs_vec = fb_raw::field_indirect(ibs, 0);
    if (!bs_vec || fb_raw::vec_len(bs_vec) == 0) return {};

    ApexModel model;
    uint32_t bs_len = fb_raw::vec_len(bs_vec);
    const uint8_t* bs_data = fb_raw::vec_bytes(bs_vec);
    model.bitstream.assign(bs_data, bs_data + bs_len);

    // 10. field_offsets (Vector<Offset<FieldOffset>>) - field_id 1
    const uint8_t* fo_vec = fb_raw::field_indirect(ibs, 1);
    uint32_t fo_count = fb_raw::vec_len(fo_vec);

    for (uint32_t i = 0; i < fo_count; i++) {
        const uint8_t* fo = fb_raw::vec_table(fo_vec, i);
        if (!fo) continue;

        int32_t offset_bit = fb_raw::field_i32(fo, 1); // VT_OFFSET_BIT=6, id=1

        const uint8_t* meta = fb_raw::field_indirect(fo, 0); // VT_META=4, id=0
        if (!meta) continue;

        int desc     = fb_raw::field_i16(meta, 0); // VT_DESC=4, id=0
        int position = fb_raw::field_i16(meta, 3); // VT_POSITION=10, id=3

        if (offset_bit / 8 + 4 > (int32_t)model.bitstream.size()) continue;

        FieldPatch p;
        p.offset_bit = offset_bit;
        p.desc       = desc;
        p.position   = position;
        model.patches.push_back(p);
    }

    return model;
}

inline void PatchVAs(ApexModel& model, uint64_t input_va, uint64_t output_va) {
    for (const auto& p : model.patches) {
        if (p.desc != DESC_INPUT && p.desc != DESC_OUTPUT) continue;

        uint64_t va  = (p.desc == DESC_INPUT) ? input_va : output_va;
        uint32_t val = (p.position == POS_LOWER)
                       ? (uint32_t)(va & 0xFFFFFFFF)
                       : (uint32_t)(va >> 32);
        std::memcpy(model.bitstream.data() + p.offset_bit / 8, &val, sizeof(uint32_t));
    }
}

} // namespace apex
