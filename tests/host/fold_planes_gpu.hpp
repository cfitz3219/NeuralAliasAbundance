#pragma once
#include "core/fold_shading.hpp"
#include "host_dx12/j2/fold_shader.hpp"

inline int AuditCandidateRanks(screen_audit::Gpu& gpu, screen_audit::Programs& programs) {
    using namespace screen_audit;
    ComPtr<ID3DBlob> shader, error;
    REQUIRE(SUCCEEDED(D3DCompile(na::j2::kCandidateShader, sizeof(na::j2::kCandidateShader)-1,
        "candidate-audit", nullptr, nullptr, "cand_pick", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &error)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = programs.root.Get();
    desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    programs.psos[1].Reset();
    REQUIRE(SUCCEEDED(gpu.d.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&programs.psos[1]))));
    for (unsigned count : {0u, 3u, 8u, 113u}) {
        std::vector<uint32_t> histogram(256*256), expected;
        for (unsigned i = 0; i < count; ++i) {
            const unsigned key = (i * 4733 + 107) & 65535;
            const uint32_t votes = (i % 11 == 0) ? 70000 : 6 + i % 7;
            histogram[key] = votes;
            if (votes >= 8) expected.push_back(((std::min)(votes,65535u) << 16) | key);
        }
        std::sort(expected.rbegin(), expected.rend());
        expected.resize(8,0);
        auto input = gpu.Upload(256,256,DXGI_FORMAT_R32_UINT,4,histogram.data());
        auto votes = test::CreateTexture(gpu.d.device.Get(),256,256,DXGI_FORMAT_R32_UINT,
            D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto result = test::CreateBuffer(gpu.d.device.Get(),32,D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto zero = test::CreateBuffer(gpu.d.device.Get(),32,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        auto read = test::CreateBuffer(gpu.d.device.Get(),32,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        REQUIRE(input && votes && result && zero && read);
        void* mapped = nullptr;
        REQUIRE(SUCCEEDED(zero->Map(0,nullptr,&mapped)));
        std::memset(mapped,0,32);
        zero->Unmap(0,nullptr);
        gpu.Barrier(input.Get(),S,D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.l.list->CopyResource(votes.Get(),input.Get());
        gpu.l.list->CopyBufferRegion(result.Get(),0,zero.Get(),0,32);
        gpu.Barrier(votes.Get(),D3D12_RESOURCE_STATE_COPY_DEST,U);
        gpu.Barrier(result.Get(),D3D12_RESOURCE_STATE_COPY_DEST,U);
        programs.Defaults();
        programs.Uav(1,8,votes.Get(),DXGI_FORMAT_R32_UINT);
        programs.Raw(1,result.Get(),8);
        std::array<unsigned,28> constants{};
        constants[7] = 8;
        programs.Dispatch(1,constants,32,32);
        gpu.Barrier(result.Get(),U,D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.l.list->CopyBufferRegion(read.Get(),0,result.Get(),0,32);
        REQUIRE(gpu.Execute() == 0);
        REQUIRE(SUCCEEDED(read->Map(0,nullptr,&mapped)));
        CHECK(std::memcmp(mapped, expected.data(),32) == 0);
        read->Unmap(0,nullptr);
        gpu.Reset();
    }
    return 0;
}

// The display and warm-up use the same program. Compare all three planes to
// the exact source texels, including partial thread groups and odd image sizes.
inline int AuditFoldPlanes(test::Device& device) {
    using namespace screen_audit;
    Gpu gpu(device);
    Programs programs(gpu);
    REQUIRE(programs.Init() == 0);
    REQUIRE(AuditCandidateRanks(gpu, programs) == 0);
    ComPtr<ID3DBlob> shader, error;
    REQUIRE(SUCCEEDED(D3DCompile(na::j2::kFoldHlsl, sizeof(na::j2::kFoldHlsl) - 1,
        "fold-audit", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &error)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = programs.root.Get();
    pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    programs.psos[0].Reset();
    REQUIRE(SUCCEEDED(device.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&programs.psos[0]))));
    constexpr unsigned w = 65, h = 37;
    for (const auto color : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        const DXGI_FORMAT formats[] = {color, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT};
        const unsigned pixel_bytes[] = {color == DXGI_FORMAT_R8G8B8A8_UNORM ? 4u : 8u, 4, 8};
        std::vector<uint8_t> bytes[3];
        ComPtr<ID3D12Resource> inputs[3];
        for (unsigned i = 0; i < 3; ++i) {
            bytes[i].resize(w * h * pixel_bytes[i]);
            for (unsigned p = 0; p < w * h; ++p) {
                uint8_t* pixel = bytes[i].data() + p * pixel_bytes[i];
                if (i == 0 && pixel_bytes[i] == 8) {
                    for (unsigned c = 0; c < 4; ++c) {
                        // Finite half-floats of both signs, including highlights above 1.
                        const uint16_t v = uint16_t((p % 2 ? 0x8000 : 0) | (0x3800 + (p * 13 + c * 97) % 0x1800));
                        std::memcpy(pixel + c * 2, &v, 2);
                    }
                } else if (i == 0) {
                    for (unsigned c = 0; c < 4; ++c) pixel[c] = uint8_t(p * 31 + c * 19);
                } else {
                    for (unsigned c = 0; c < pixel_bytes[i] / 4; ++c) {
                        const float v = float(int(p % 1777) - 888) * (c ? -.03125f : .125f);
                        std::memcpy(pixel + c * 4, &v, 4);
                    }
                }
            }
            inputs[i] = gpu.Upload(w, h, formats[i], pixel_bytes[i], bytes[i].data());
        }
        REQUIRE(gpu.Execute() == 0);
        gpu.Reset();
        for (const auto mode : {na::fold::Mode::Quality, na::fold::Mode::Performance}) {
            const auto block = na::fold::BlockFor(mode), pick = na::fold::Pick(mode);
            const unsigned fw = w / block.x, fh = h / block.y;
            ComPtr<ID3D12Resource> outputs[3], readbacks[3];
            programs.Defaults();
            for (unsigned i = 0; i < 3; ++i) {
                outputs[i] = test::CreateTexture(device.device.Get(), fw, fh, formats[i], U, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                REQUIRE(outputs[i]);
                programs.Srv(0, i, inputs[i].Get(), formats[i]);
                programs.Uav(0, 8 + i, outputs[i].Get(), formats[i]);
            }
            std::array<unsigned,28> constants{block.x,block.y,pick.x,pick.y};
            programs.Dispatch(0, constants, (fw + 7) / 8, (fh + 7) / 8);
            for (unsigned i = 0; i < 3; ++i) readbacks[i] = gpu.Read(outputs[i].Get(), fw, fh, formats[i], pixel_bytes[i], U);
            REQUIRE(gpu.Execute() == 0);
            for (unsigned i = 0; i < 3; ++i) {
                uint8_t* mapped = nullptr;
                REQUIRE(SUCCEEDED(readbacks[i]->Map(0,nullptr,reinterpret_cast<void**>(&mapped))));
                const unsigned pitch = (fw * pixel_bytes[i] + 255u) & ~255u;
                unsigned differences = 0;
                for (unsigned y = 0; y < fh; ++y) for (unsigned x = 0; x < fw; ++x) {
                    const unsigned source = (y * block.y + pick.y) * w + x * block.x + pick.x;
                    differences += std::memcmp(mapped + y * pitch + x * pixel_bytes[i],
                        bytes[i].data() + source * pixel_bytes[i], pixel_bytes[i]) != 0;
                }
                readbacks[i]->Unmap(0,nullptr);
                CHECK(differences == 0);
            }
            gpu.Reset();
        }
    }
    return 0;
}
