#pragma once
#include <vector>
#include <string>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iomanip>

// FlatBuffers generated parser — official libedgetpu schema-based implementation
// Uses executable_generated.h (v24.3.25) with flatbuffers library
// Provides additional metadata (input/output layer info) vs apex_model.hpp

// Prevent Windows.h min/max macro conflicts with flatbuffers
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "executable_generated.h"
#include "flatbuffers/flatbuffers.h"

namespace apex_fb {

struct FieldPatch {
    int32_t offset_bit;
    platforms::darwinn::Description desc;
    platforms::darwinn::Position    position;
};

struct LayerInfo {
    std::string name;
    size_t      size_bytes;
    int         y_dim, x_dim, z_dim;
    platforms::darwinn::DataType data_type;
};

struct ApexModelFb {
    std::vector<uint8_t>    bitstream;
    std::vector<FieldPatch> patches;
    std::vector<LayerInfo>  input_layers;
    std::vector<LayerInfo>  output_layers;
    size_t                  scratch_size_bytes;
};

inline uint64_t PageAlignUp(uint64_t n) { return (n + 4095ULL) & ~4095ULL; }

inline ApexModelFb LoadModel(const std::string& path) {
    std::cout << "[LoadModel] Opening: " << path << std::endl;

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        std::cout << "[LoadModel] FAIL: cannot open file" << std::endl;
        return {};
    }

    size_t fileSize = (size_t)f.tellg();
    std::cout << "[LoadModel] File size: " << fileSize << " bytes" << std::endl;

    std::vector<uint8_t> raw(fileSize);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(raw.data()), (std::streamsize)raw.size());

    // Print file header bytes to identify format
    std::cout << "[LoadModel] File header (bytes 0-11): ";
    for (int i = 0; i < 12 && i < (int)raw.size(); i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)raw[i] << " ";
    }
    std::cout << std::dec << std::endl;
    std::cout << "[LoadModel] File identifier (bytes 4-7): "
              << (char)raw[4] << (char)raw[5] << (char)raw[6] << (char)raw[7] << std::endl;

    // Search for DWN1 signature (Package identifier) in the file
    size_t pkg_offset = 0;
    bool found_dwn1 = false;
    for (size_t i = 4; i + 4 <= raw.size(); i++) {
        if (raw[i]=='D' && raw[i+1]=='W' && raw[i+2]=='N' && raw[i+3]=='1') {
            pkg_offset = i - 4;  // Package starts 4 bytes before the identifier
            found_dwn1 = true;
            std::cout << "[LoadModel] Found DWN1 signature at offset 0x"
                      << std::hex << i << std::dec
                      << " → Package starts at 0x" << std::hex << pkg_offset << std::dec << std::endl;
            break;
        }
    }
    if (!found_dwn1) {
        std::cout << "[LoadModel] DWN1 signature NOT found in file" << std::endl;
    }

    const uint8_t* pkg_buf = raw.data() + pkg_offset;

    // 1. Parse Package root
    std::cout << "[LoadModel] Step 1: GetPackage (offset=" << pkg_offset << ")..." << std::endl;
    const auto* pkg = platforms::darwinn::GetPackage(pkg_buf);
    if (!pkg) {
        std::cout << "[LoadModel] FAIL: GetPackage returned null" << std::endl;
        return {};
    }
    std::cout << "[LoadModel] Package OK (compiler_version: "
              << (pkg->compiler_version() ? pkg->compiler_version()->c_str() : "N/A")
              << ", min_runtime_version: " << pkg->min_runtime_version() << ")" << std::endl;

    // 2. Get serialized_multi_executable
    std::cout << "[LoadModel] Step 2: serialized_multi_executable..." << std::endl;
    const auto* multi_bytes = pkg->serialized_multi_executable();
    if (!multi_bytes || multi_bytes->size() == 0) {
        std::cout << "[LoadModel] FAIL: serialized_multi_executable is null or empty" << std::endl;
        return {};
    }
    std::cout << "[LoadModel] multi_executable bytes: " << multi_bytes->size() << std::endl;

    // 3. Parse nested MultiExecutable FlatBuffer
    std::cout << "[LoadModel] Step 3: GetRoot<MultiExecutable>..." << std::endl;
    const auto* multi = ::flatbuffers::GetRoot<platforms::darwinn::MultiExecutable>(
        multi_bytes->Data());
    if (!multi) {
        std::cout << "[LoadModel] FAIL: MultiExecutable root is null" << std::endl;
        return {};
    }

    // 4. Get serialized_executables
    std::cout << "[LoadModel] Step 4: serialized_executables..." << std::endl;
    const auto* execs_vec = multi->serialized_executables();
    if (!execs_vec || execs_vec->size() == 0) {
        std::cout << "[LoadModel] FAIL: serialized_executables is null or empty" << std::endl;
        return {};
    }
    std::cout << "[LoadModel] executables count: " << execs_vec->size() << std::endl;

    // 5. Get first serialized executable
    std::cout << "[LoadModel] Step 5: Get executable[0]..." << std::endl;
    const auto* ser_exec = execs_vec->Get(0);
    if (!ser_exec) {
        std::cout << "[LoadModel] FAIL: ser_exec[0] is null" << std::endl;
        return {};
    }
    std::cout << "[LoadModel] executable[0] size: " << ser_exec->size() << " bytes" << std::endl;

    // 6. Parse nested Executable FlatBuffer
    std::cout << "[LoadModel] Step 6: GetRoot<Executable>..." << std::endl;
    const auto* exec = ::flatbuffers::GetRoot<platforms::darwinn::Executable>(
        ser_exec->data());
    if (!exec) {
        std::cout << "[LoadModel] FAIL: Executable root is null" << std::endl;
        return {};
    }
    std::cout << "[LoadModel] Executable OK (name: "
              << (exec->name() ? exec->name()->c_str() : "N/A")
              << ", chip: " << (exec->chip() ? exec->chip()->c_str() : "N/A")
              << ", scratch: " << exec->scratch_size_bytes() << " bytes)" << std::endl;

    // 7. Get instruction_bitstreams
    std::cout << "[LoadModel] Step 7: instruction_bitstreams..." << std::endl;
    const auto* ibs_vec = exec->instruction_bitstreams();
    if (!ibs_vec || ibs_vec->size() == 0) {
        std::cout << "[LoadModel] FAIL: instruction_bitstreams is null or empty" << std::endl;
        return {};
    }
    std::cout << "[LoadModel] bitstream count: " << ibs_vec->size() << std::endl;

    // 8. Get first InstructionBitstream
    const auto* ibs = ibs_vec->Get(0);
    if (!ibs) {
        std::cout << "[LoadModel] FAIL: ibs[0] is null" << std::endl;
        return {};
    }

    ApexModelFb model;

    // 9. Extract bitstream bytes
    std::cout << "[LoadModel] Step 9: extracting bitstream bytes..." << std::endl;
    const auto* bs_vec = ibs->bitstream();
    if (bs_vec && bs_vec->size() > 0) {
        model.bitstream.assign(bs_vec->begin(), bs_vec->end());
    }
    std::cout << "[LoadModel] bitstream size: " << model.bitstream.size() << " bytes" << std::endl;

    // 10. Extract scratch_size_bytes
    model.scratch_size_bytes = (size_t)exec->scratch_size_bytes();

    // 11. Collect FieldOffset patches
    std::cout << "[LoadModel] Step 11: collecting field_offsets..." << std::endl;
    const auto* fo_vec = ibs->field_offsets();
    if (fo_vec && fo_vec->size() > 0) {
        for (uint32_t i = 0; i < fo_vec->size(); i++) {
            const auto* fo = fo_vec->Get(i);
            if (!fo || !fo->meta()) continue;

            FieldPatch p;
            p.offset_bit = fo->offset_bit();
            p.desc       = fo->meta()->desc();
            p.position   = fo->meta()->position();

            if (p.offset_bit / 8 + 4 <= (int32_t)model.bitstream.size()) {
                model.patches.push_back(p);
            }
        }
    }
    std::cout << "[LoadModel] patches collected: " << model.patches.size() << std::endl;

    // 12. Extract input layer metadata
    std::cout << "[LoadModel] Step 12: input_layers..." << std::endl;
    const auto* input_vec = exec->input_layers();
    if (input_vec && input_vec->size() > 0) {
        for (uint32_t i = 0; i < input_vec->size(); i++) {
            const auto* layer = input_vec->Get(i);
            if (!layer) continue;

            LayerInfo info;
            if (layer->name()) info.name = layer->name()->str();
            info.size_bytes = (size_t)layer->size_bytes();
            info.y_dim      = (int)layer->y_dim();
            info.x_dim      = (int)layer->x_dim();
            info.z_dim      = (int)layer->z_dim();
            info.data_type  = layer->data_type();

            std::cout << "[LoadModel]   input[" << i << "]: " << info.name
                      << " " << info.y_dim << "x" << info.x_dim << "x" << info.z_dim
                      << " = " << info.size_bytes << " bytes" << std::endl;

            model.input_layers.push_back(info);
        }
    } else {
        std::cout << "[LoadModel]   input_layers: none" << std::endl;
    }

    // 13. Extract output layer metadata
    std::cout << "[LoadModel] Step 13: output_layers..." << std::endl;
    const auto* output_vec = exec->output_layers();
    if (output_vec && output_vec->size() > 0) {
        for (uint32_t i = 0; i < output_vec->size(); i++) {
            const auto* layer = output_vec->Get(i);
            if (!layer) continue;

            LayerInfo info;
            if (layer->name()) info.name = layer->name()->str();
            info.size_bytes = (size_t)layer->size_bytes();
            info.y_dim      = (int)layer->y_dim();
            info.x_dim      = (int)layer->x_dim();
            info.z_dim      = (int)layer->z_dim();
            info.data_type  = layer->data_type();

            std::cout << "[LoadModel]   output[" << i << "]: " << info.name
                      << " " << info.y_dim << "x" << info.x_dim << "x" << info.z_dim
                      << " = " << info.size_bytes << " bytes" << std::endl;

            model.output_layers.push_back(info);
        }
    } else {
        std::cout << "[LoadModel]   output_layers: none" << std::endl;
    }

    std::cout << "[LoadModel] Done!" << std::endl;
    return model;
}

inline void PatchVAs(ApexModelFb& model, uint64_t input_va, uint64_t output_va) {
    using namespace platforms::darwinn;
    for (const auto& p : model.patches) {
        if (p.desc != Description_BASE_ADDRESS_INPUT_ACTIVATION &&
            p.desc != Description_BASE_ADDRESS_OUTPUT_ACTIVATION) {
            continue;
        }

        uint64_t va = (p.desc == Description_BASE_ADDRESS_INPUT_ACTIVATION)
                      ? input_va : output_va;

        uint32_t val = (p.position == Position_LOWER_32BIT)
                       ? (uint32_t)(va & 0xFFFFFFFF)
                       : (uint32_t)(va >> 32);

        std::memcpy(model.bitstream.data() + p.offset_bit / 8, &val, sizeof(uint32_t));
    }
}

} // namespace apex_fb
