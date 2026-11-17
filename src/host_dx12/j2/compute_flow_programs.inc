// Shared pipeline construction adapted from j2_flow.cpp.
    ComPtr<ID3DBlob> cs_encode, cs_accept, cs_measure, cs_cut_state, cs_cut_show, err;
    if (FAILED(D3DCompile(na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1, "j2_flow", nullptr, nullptr, "encode", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_encode, &err)) ||
        FAILED(D3DCompile(na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1, "j2_flow", nullptr, nullptr, "accept", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_accept, &err)))
        return false;
    // The camera-free programs. A failure here leaves the residual path above working: measured
    // motion is then unavailable and says so (Record, Frame::total).
    const bool camera_free =
        SUCCEEDED(D3DCompile(na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1, "j2_flow", nullptr, nullptr, "measure", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_measure, &err)) &&
        SUCCEEDED(D3DCompile(na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1, "j2_flow", nullptr, nullptr, "cut_state", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_cut_state, &err)) &&
        SUCCEEDED(D3DCompile(na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1, "j2_flow", nullptr, nullptr, "cut_show", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_cut_show, &err));
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 8;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 4;
    ranges[1].OffsetInDescriptorsFromTableStart = 8;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = sizeof(Constants)/4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC smp{};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 2;
    rd.pParameters = params;
    rd.NumStaticSamplers = 1;
    rd.pStaticSamplers = &smp;
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.root))))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g.root.Get();
    pd.CS = {cs_encode->GetBufferPointer(), cs_encode->GetBufferSize()};
    if (FAILED(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g.pso_encode)))) return false;
    pd.CS = {cs_accept->GetBufferPointer(), cs_accept->GetBufferSize()};
    if (FAILED(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g.pso_accept)))) return false;
    if (camera_free) {
        pd.CS = {cs_measure->GetBufferPointer(), cs_measure->GetBufferSize()};
        const bool a = SUCCEEDED(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g.pso_measure)));
        pd.CS = {cs_cut_state->GetBufferPointer(), cs_cut_state->GetBufferSize()};
        const bool b = SUCCEEDED(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g.pso_cut_state)));
        pd.CS = {cs_cut_show->GetBufferPointer(), cs_cut_show->GetBufferSize()};
        const bool c = SUCCEEDED(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g.pso_cut_show)));
        if (!a || !b || !c) { g.pso_measure.Reset(); g.pso_cut_state.Reset(); g.pso_cut_show.Reset(); }
    }
    {
        // VARIANT: motion/screen-fit (flow_shaders.hpp fit_sample / fit_accum / fit_solve). All three
        // programs or none: without them `measure` has no model to follow, and the j2screenfit line
        // says on=0.
        static const char* const entries[3] = {"fit_sample", "fit_accum", "fit_solve"};
        ComPtr<ID3D12PipelineState>* const states[3] = {&g.pso_fit_sample, &g.pso_fit_accum, &g.pso_fit_solve};
        bool all = camera_free && g.pso_measure;
        for (int i = 0; i < 3 && all; ++i) {
            ComPtr<ID3DBlob> cs;
            all = SUCCEEDED(D3DCompile(na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1, "j2_flow", nullptr, nullptr, entries[i], "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err));
            if (all) {
                pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
                all = SUCCEEDED(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(states[i]->ReleaseAndGetAddressOf())));
            }
        }
        if (!all) for (auto* state : states) state->Reset();
    }
    {
        // VARIANT: flow/frame-candidates (flow_shaders.hpp kCandidateShader). All four programs or
        // none: without them the search's grid is used as it is, and the j2candidates line says on=0.
        static const char* const entries[4] = {"cand_clear", "cand_count", "cand_pick", "cand_apply"};
        ComPtr<ID3D12PipelineState>* const states[4] = {&g.pso_cand_clear, &g.pso_cand_count, &g.pso_cand_pick, &g.pso_cand_apply};
        bool all = true;
        for (int i = 0; i < 4 && all; ++i) {
            ComPtr<ID3DBlob> cs;
            all = SUCCEEDED(D3DCompile(na::j2::kCandidateShader, sizeof na::j2::kCandidateShader - 1, "j2_candidates", nullptr, nullptr, entries[i], "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err));
            if (all) {
                pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
                all = SUCCEEDED(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(states[i]->ReleaseAndGetAddressOf())));
            }
        }
        if (!all) for (auto* state : states) state->Reset();
    }
    return true;
