#pragma once

#include <d3d11_4.h>

#include <mutex>

namespace D3d11ContextSync {

inline std::recursive_mutex& mutex()
{
    static std::recursive_mutex instance;
    return instance;
}

inline void lock(void*)
{
    mutex().lock();
}

inline void unlock(void*)
{
    mutex().unlock();
}

inline bool enableMultithreadProtection(void* device)
{
    auto* d3dDevice = static_cast<ID3D11Device*>(device);
    if (!d3dDevice) {
        return false;
    }

    ID3D11Multithread* multithread = nullptr;
    const HRESULT queryResult = d3dDevice->QueryInterface(
        __uuidof(ID3D11Multithread),
        reinterpret_cast<void**>(&multithread));
    if (FAILED(queryResult) || !multithread) {
        return false;
    }

    const HRESULT protectResult = multithread->SetMultithreadProtected(TRUE);
    multithread->Release();
    return SUCCEEDED(protectResult);
}

} // namespace D3d11ContextSync
