/*
 * Copyright 2018 Józef Kucia for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */

#include "config.h"
#include "wine/port.h"

#define COBJMACROS
#define VK_NO_PROTOTYPES
#define VKD3D_NO_VULKAN_H
#define VKD3D_NO_WIN32_TYPES
#define WINE_VK_HOST

#include "wine/debug.h"
#include "wine/heap.h"
#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

#include "dxgi1_6.h"
#include "d3d12.h"

#include <vkd3d.h>

WINE_DEFAULT_DEBUG_CHANNEL(d3d12);
WINE_DECLARE_DEBUG_CHANNEL(winediag);

HRESULT WINAPI D3D12GetDebugInterface(REFIID iid, void **debug)
{
    TRACE("iid %s, debug %p.\n", debugstr_guid(iid), debug);

    WARN("Returning DXGI_ERROR_SDK_COMPONENT_MISSING.\n");
    return DXGI_ERROR_SDK_COMPONENT_MISSING;
}

HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT feature_count,
        const IID *iids, void *configurations, UINT *configurations_sizes)
{
    FIXME("feature_count %u, iids %p, configurations %p, configurations_sizes %p stub!\n",
            feature_count, iids, configurations, configurations_sizes);

    return E_NOINTERFACE;
}

static HRESULT d3d12_signal_event(HANDLE event)
{
    return SetEvent(event) ? S_OK : E_FAIL;
}

struct d3d12_thread_data
{
    PFN_vkd3d_thread main_pfn;
    void *data;
};

static DWORD WINAPI d3d12_thread_main(void *data)
{
    struct d3d12_thread_data *thread_data = data;

    thread_data->main_pfn(thread_data->data);
    heap_free(thread_data);
    return 0;
}

static void *d3d12_create_thread(PFN_vkd3d_thread main_pfn, void *data)
{
    struct d3d12_thread_data *thread_data;
    HANDLE thread;

    if (!(thread_data = heap_alloc(sizeof(*thread_data))))
    {
        ERR("Failed to allocate thread data.\n");
        return NULL;
    }

    thread_data->main_pfn = main_pfn;
    thread_data->data = data;

    if (!(thread = CreateThread(NULL, 0, d3d12_thread_main, thread_data, 0, NULL)))
        heap_free(thread_data);

    return thread;
}

static HRESULT d3d12_join_thread(void *handle)
{
    HANDLE thread = handle;
    DWORD ret;

    if ((ret = WaitForSingleObject(thread, INFINITE)) != WAIT_OBJECT_0)
        ERR("Failed to wait for thread, ret %#x.\n", ret);
    CloseHandle(thread);
    return ret == WAIT_OBJECT_0 ? S_OK : E_FAIL;
}

static const struct vulkan_funcs *get_vk_funcs(void)
{
    const struct vulkan_funcs *vk_funcs;
    HDC hdc;

    hdc = GetDC(0);
    vk_funcs = __wine_get_vulkan_driver(hdc, WINE_VULKAN_DRIVER_VERSION);
    ReleaseDC(0, hdc);
    return vk_funcs;
}

static HRESULT d3d12_get_adapter(IUnknown **adapter, LUID *luid)
{
    DXGI_ADAPTER_DESC adapter_desc;
    IDXGIFactory4 *factory = NULL;
    IDXGIAdapter *dxgi_adapter;
    HRESULT hr;

    if (!*adapter)
    {
        if (FAILED(hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory)))
        {
            WARN("Failed to create DXGI factory, hr %#x.\n", hr);
            goto done;
        }

        if (FAILED(hr = IDXGIFactory4_EnumAdapters(factory, 0, &dxgi_adapter)))
        {
            WARN("Failed to enumerate primary adapter, hr %#x.\n", hr);
            goto done;
        }
    }
    else if (FAILED(hr = IUnknown_QueryInterface(*adapter, &IID_IDXGIAdapter, (void **)&dxgi_adapter)))
    {
        WARN("Invalid adapter %p, hr %#x.\n", adapter, hr);
        goto done;
    }

    if (SUCCEEDED(hr = IDXGIAdapter_GetDesc(dxgi_adapter, &adapter_desc)))
    {
        *adapter = (IUnknown *)dxgi_adapter;
        *luid = adapter_desc.AdapterLuid;
    }
    else
    {
        IDXGIAdapter_Release(dxgi_adapter);
    }

done:
    if (factory)
        IDXGIFactory4_Release(factory);

    return hr;
}

HRESULT WINAPI D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL minimum_feature_level,
        REFIID iid, void **device)
{
    struct vkd3d_instance_create_info instance_create_info;
    struct vkd3d_device_create_info device_create_info;
    const struct vulkan_funcs *vk_funcs;
    LUID adapter_luid;
    HRESULT hr;

    static const char * const instance_extensions[] =
    {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    static const char * const device_extensions[] =
    {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    TRACE("adapter %p, minimum_feature_level %#x, iid %s, device %p.\n",
            adapter, minimum_feature_level, debugstr_guid(iid), device);

    if (!(vk_funcs = get_vk_funcs()))
    {
        ERR_(winediag)("Failed to load Wine Vulkan driver.\n");
        return E_FAIL;
    }

    /* FIXME: Get VkPhysicalDevice for IDXGIAdapter. */
    if (FAILED(hr = d3d12_get_adapter(&adapter, &adapter_luid)))
        return hr;

    memset(&instance_create_info, 0, sizeof(instance_create_info));
    instance_create_info.type = VKD3D_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_create_info.pfn_signal_event = d3d12_signal_event;
    instance_create_info.pfn_create_thread = d3d12_create_thread;
    instance_create_info.pfn_join_thread = d3d12_join_thread;
    instance_create_info.wchar_size = sizeof(WCHAR);
    instance_create_info.pfn_vkGetInstanceProcAddr
            = (PFN_vkGetInstanceProcAddr)vk_funcs->p_vkGetInstanceProcAddr;
    instance_create_info.instance_extensions = instance_extensions;
    instance_create_info.instance_extension_count = ARRAY_SIZE(instance_extensions);

    memset(&device_create_info, 0, sizeof(device_create_info));
    device_create_info.type = VKD3D_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.minimum_feature_level = minimum_feature_level;
    device_create_info.instance_create_info = &instance_create_info;
    device_create_info.device_extensions = device_extensions;
    device_create_info.device_extension_count = ARRAY_SIZE(device_extensions);
    device_create_info.parent = adapter;
    device_create_info.adapter_luid = adapter_luid;

    hr = vkd3d_create_device(&device_create_info, iid, device);
    IUnknown_Release(adapter);
    return hr;
}

HRESULT WINAPI D3D12CreateRootSignatureDeserializer(const void *data, SIZE_T data_size,
        REFIID iid, void **deserializer)
{
    TRACE("data %p, data_size %lu, iid %s, deserializer %p.\n",
            data, data_size, debugstr_guid(iid), deserializer);

    return vkd3d_create_root_signature_deserializer(data, data_size, iid, deserializer);
}

HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC *root_signature_desc,
        D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob **blob, ID3DBlob **error_blob)
{
    TRACE("root_signature_desc %p, version %#x, blob %p, error_blob %p.\n",
            root_signature_desc, version, blob, error_blob);

    return vkd3d_serialize_root_signature(root_signature_desc, version, blob, error_blob);
}

HRESULT WINAPI D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc,
        ID3DBlob **blob, ID3DBlob **error_blob)
{
    FIXME("desc %p, blob %p, error_blob %p partial-stub!\n",
            desc, blob, error_blob);

    if (desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_0)
        return D3D12SerializeRootSignature(&desc->Desc_1_0, desc->Version, blob, error_blob);

    FIXME("Unsupported version %#x.\n", desc->Version);
    return E_NOTIMPL;
}
