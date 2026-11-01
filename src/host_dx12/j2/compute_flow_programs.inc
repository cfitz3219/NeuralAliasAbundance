// Shared pipeline construction adapted from j2_flow.cpp.
    ComPtr<ID3DBlob> cs_encode, cs_accept, err;
    if (FAILED(D3DCompile(na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1, "j2_flow", nullptr, nullptr, "encode", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_encode, &err)) ||
        FAILED(D3DCompile(na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1, "j2_flow", nullptr, nullptr, "accept", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_accept, &err)))
        return false;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 7;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 3;
    ranges[1].OffsetInDescriptorsFromTableStart = 7;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 16;
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
    return true;
