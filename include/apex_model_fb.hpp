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
    std::string name;  // layer name for per-output VA matching
};

struct LayerInfo {
    std::string name;
    size_t      size_bytes;
    int         y_dim, x_dim, z_dim;
    platforms::darwinn::DataType data_type;
};

struct ApexModelFb {
    // executable[0]: main inference (EXECUTION_ONLY or STAND_ALONE)
    std::vector<uint8_t>    bitstream;
    std::vector<FieldPatch> patches;
    std::vector<LayerInfo>  input_layers;
    std::vector<LayerInfo>  output_layers;
    size_t                  scratch_size_bytes;
    size_t                  total_output_size_bytes; // sum of PageAlignUp(each output layer)

    // executable[1]: parameter caching (PARAMETER_CACHING, may be empty)
    std::vector<uint8_t>    param_bitstream;  // exe1 bitstream (~9KB)
    std::vector<FieldPatch> param_patches;    // exe1 VA patches
    std::vector<uint8_t>    parameters;       // exe1 raw parameter data (~6MB)
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

    // 5a. Dump all executables (chip, name, sizes) before committing to [0]
    for (uint32_t ei = 0; ei < execs_vec->size(); ei++) {
        const auto* se = execs_vec->Get(ei);
        if (!se) { std::cout << "[LoadModel] executable[" << ei << "] is null" << std::endl; continue; }
        std::cout << "[LoadModel] executable[" << ei << "] raw size: " << se->size() << " bytes" << std::endl;
        const auto* ex = ::flatbuffers::GetRoot<platforms::darwinn::Executable>(se->data());
        if (!ex) { std::cout << "[LoadModel] executable[" << ei << "] parse failed" << std::endl; continue; }
        std::cout << "[LoadModel] executable[" << ei << "] name=" << (ex->name() ? ex->name()->c_str() : "N/A")
                  << " chip=" << (ex->chip() ? ex->chip()->c_str() : "N/A")
                  << " scratch=" << ex->scratch_size_bytes() << "B" << std::endl;
        const auto* ibs_v = ex->instruction_bitstreams();
        if (ibs_v && ibs_v->size() > 0 && ibs_v->Get(0)) {
            const auto* bs = ibs_v->Get(0)->bitstream();
            std::cout << "[LoadModel] executable[" << ei << "] bitstream=" << (bs ? bs->size() : 0) << " bytes" << std::endl;
        }
        const auto* in_v = ex->input_layers();
        if (in_v) for (uint32_t li = 0; li < in_v->size(); li++) {
            const auto* l = in_v->Get(li);
            if (l) std::cout << "[LoadModel] executable[" << ei << "] input[" << li << "]: "
                             << (l->name() ? l->name()->c_str() : "?")
                             << " " << l->y_dim() << "x" << l->x_dim() << "x" << l->z_dim()
                             << " = " << l->size_bytes() << "B" << std::endl;
        }
        const auto* out_v = ex->output_layers();
        if (out_v) for (uint32_t li = 0; li < out_v->size(); li++) {
            const auto* l = out_v->Get(li);
            if (l) std::cout << "[LoadModel] executable[" << ei << "] output[" << li << "]: "
                             << (l->name() ? l->name()->c_str() : "?")
                             << " " << l->y_dim() << "x" << l->x_dim() << "x" << l->z_dim()
                             << " = " << l->size_bytes() << "B" << std::endl;
        }
    }

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
            if (fo->meta()->name()) p.name = fo->meta()->name()->str();

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

    // Compute total_output_size_bytes (sum of PageAlignUp of each output layer)
    model.total_output_size_bytes = 0;
    for (const auto& layer : model.output_layers)
        model.total_output_size_bytes += PageAlignUp(layer.size_bytes);
    std::cout << "[LoadModel] total_output_size_bytes: " << model.total_output_size_bytes << std::endl;

    // 14. Parse executable[1] (PARAMETER_CACHING) if present
    std::cout << "[LoadModel] Step 14: executable[1]..." << std::endl;
    if (execs_vec->size() > 1) {
        const auto* se1 = execs_vec->Get(1);
        if (se1) {
            const auto* ex1 = ::flatbuffers::GetRoot<platforms::darwinn::Executable>(se1->data());
            if (ex1) {
                const auto* ibs_vec1 = ex1->instruction_bitstreams();
                if (ibs_vec1 && ibs_vec1->size() > 0) {
                    const auto* ibs1 = ibs_vec1->Get(0);
                    if (ibs1) {
                        const auto* bs1 = ibs1->bitstream();
                        if (bs1 && bs1->size() > 0)
                            model.param_bitstream.assign(bs1->begin(), bs1->end());
                        const auto* fo_vec1 = ibs1->field_offsets();
                        if (fo_vec1) {
                            for (uint32_t i = 0; i < fo_vec1->size(); i++) {
                                const auto* fo = fo_vec1->Get(i);
                                if (!fo || !fo->meta()) continue;
                                FieldPatch p;
                                p.offset_bit = fo->offset_bit();
                                p.desc       = fo->meta()->desc();
                                p.position   = fo->meta()->position();
                                if (p.offset_bit / 8 + 4 <= (int32_t)model.param_bitstream.size())
                                    model.param_patches.push_back(p);
                            }
                        }
                    }
                }
                const auto* params = ex1->parameters();
                if (params && params->size() > 0)
                    model.parameters.assign(params->begin(), params->end());
                std::cout << "[LoadModel] exe1 bitstream: " << model.param_bitstream.size()
                          << " bytes, patches: " << model.param_patches.size()
                          << ", parameters: " << model.parameters.size() << " bytes" << std::endl;
            }
        }
    } else {
        std::cout << "[LoadModel] No exe[1] (STAND_ALONE model)" << std::endl;
    }

    std::cout << "[LoadModel] Done!" << std::endl;
    return model;
}

inline void DumpPatchedVAs(const ApexModelFb& model) {
    using namespace platforms::darwinn;
    uint64_t input_lo = 0, input_hi = 0;
    uint64_t param_lo = 0, param_hi = 0;
    uint64_t scratch_lo = 0, scratch_hi = 0;

    // per-output layer: lo/hi pair indexed by output_layers order
    std::vector<uint64_t> out_lo(model.output_layers.size(), 0);
    std::vector<uint64_t> out_hi(model.output_layers.size(), 0);

    for (const auto& p : model.patches) {
        if (p.offset_bit / 8 + 4 > (int32_t)model.bitstream.size()) continue;
        uint32_t val = 0;
        std::memcpy(&val, model.bitstream.data() + p.offset_bit / 8, sizeof(uint32_t));
        bool lo = (p.position == Position_LOWER_32BIT);
        switch (p.desc) {
        case Description_BASE_ADDRESS_INPUT_ACTIVATION:
            if (lo) input_lo = val; else input_hi = val; break;
        case Description_BASE_ADDRESS_OUTPUT_ACTIVATION:
            for (size_t i = 0; i < model.output_layers.size(); i++) {
                if (model.output_layers[i].name == p.name) {
                    if (lo) out_lo[i] = val; else out_hi[i] = val;
                    break;
                }
            }
            break;
        case Description_BASE_ADDRESS_PARAMETER:
            if (lo) param_lo = val; else param_hi = val; break;
        case Description_BASE_ADDRESS_SCRATCH:
            if (lo) scratch_lo = val; else scratch_hi = val; break;
        default: break;
        }
    }

    auto va64 = [](uint64_t hi, uint64_t lo) { return (hi << 32) | lo; };
    std::cout << std::hex << std::setfill('0');
    std::cout << "  INPUT   VA: 0x" << std::setw(16) << va64(input_hi, input_lo) << std::endl;
    for (size_t i = 0; i < model.output_layers.size(); i++)
        std::cout << "  OUTPUT[" << i << "] VA: 0x" << std::setw(16) << va64(out_hi[i], out_lo[i])
                  << "  (" << model.output_layers[i].name << ")" << std::endl;
    std::cout << "  PARAM   VA: 0x" << std::setw(16) << va64(param_hi, param_lo)   << std::endl;
    std::cout << "  SCRATCH VA: 0x" << std::setw(16) << va64(scratch_hi, scratch_lo) << std::endl;
    std::cout << std::dec;
    std::cout << "  scratch_size_bytes: " << model.scratch_size_bytes << std::endl;
    if (model.scratch_size_bytes > 0 && va64(scratch_hi, scratch_lo) == 0)
        std::cout << "  WARNING: scratch_size > 0 but SCRATCH VA = 0" << std::endl;
}

inline void PatchVAs(ApexModelFb& model, uint64_t input_va, uint64_t output_va,
                     uint64_t param_va = 0, uint64_t scratch_va = 0) {
    using namespace platforms::darwinn;
    for (const auto& p : model.patches) {
        uint64_t va = 0;
        switch (p.desc) {
        case Description_BASE_ADDRESS_INPUT_ACTIVATION:  va = input_va;   break;
        case Description_BASE_ADDRESS_OUTPUT_ACTIVATION: {
            // match by layer name to compute per-layer device VA
            uint64_t cur = output_va;
            for (const auto& layer : model.output_layers) {
                if (layer.name == p.name) { va = cur; break; }
                cur += PageAlignUp(layer.size_bytes);
            }
            if (va == 0) va = output_va;  // fallback for single-output or unnamed
            break;
        }
        case Description_BASE_ADDRESS_PARAMETER:         va = param_va;   break;
        case Description_BASE_ADDRESS_SCRATCH:           va = scratch_va; break;
        default: continue;
        }

        uint32_t val = (p.position == Position_LOWER_32BIT)
                       ? (uint32_t)(va & 0xFFFFFFFF)
                       : (uint32_t)(va >> 32);

        std::memcpy(model.bitstream.data() + p.offset_bit / 8, &val, sizeof(uint32_t));
    }
}

// Patch exe1 (PARAMETER_CACHING) bitstream: write param_va into BASE_ADDRESS_PARAMETER fields.
inline void PatchParamBitstreamVAs(ApexModelFb& model, uint64_t param_va) {
    using namespace platforms::darwinn;
    for (const auto& p : model.param_patches) {
        if (p.offset_bit / 8 + 4 > (int32_t)model.param_bitstream.size()) continue;
        uint64_t va = 0;
        switch (p.desc) {
        case Description_BASE_ADDRESS_PARAMETER: va = param_va; break;
        default: continue;
        }
        uint32_t val = (p.position == Position_LOWER_32BIT)
                       ? (uint32_t)(va & 0xFFFFFFFF)
                       : (uint32_t)(va >> 32);
        std::memcpy(model.param_bitstream.data() + p.offset_bit / 8, &val, sizeof(uint32_t));
    }
}

inline void DumpParamPatchedVAs(const ApexModelFb& model) {
    using namespace platforms::darwinn;
    uint64_t param_lo = 0, param_hi = 0;
    for (const auto& p : model.param_patches) {
        if (p.offset_bit / 8 + 4 > (int32_t)model.param_bitstream.size()) continue;
        uint32_t val = 0;
        std::memcpy(&val, model.param_bitstream.data() + p.offset_bit / 8, sizeof(uint32_t));
        bool lo = (p.position == Position_LOWER_32BIT);
        if (p.desc == Description_BASE_ADDRESS_PARAMETER) {
            if (lo) param_lo = val; else param_hi = val;
        }
    }
    auto va64 = [](uint64_t hi, uint64_t lo) { return (hi << 32) | lo; };
    std::cout << std::hex << std::setfill('0');
    std::cout << "  [param exe] PARAMETER VA: 0x" << std::setw(16) << va64(param_hi, param_lo) << std::dec << std::endl;
    std::cout << "  [param exe] bitstream: " << model.param_bitstream.size()
              << " bytes, parameters: " << model.parameters.size() << " bytes" << std::endl;
}

} // namespace apex_fb
