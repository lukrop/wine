/*
 * ACM wrapper filter unit tests
 *
 * Copyright 2018 Zebediah Figura
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
 */

#define COBJMACROS
#include "dshow.h"
#include "wine/test.h"

static const WCHAR sink_id[] = {'I','n',0};
static const WCHAR source_id[] = {'O','u','t',0};
static const WCHAR sink_name[] = {'I','n','p','u','t',0};
static const WCHAR source_name[] = {'O','u','t','p','u','t',0};

static IBaseFilter *create_acm_wrapper(void)
{
    IBaseFilter *filter = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_ACMWrapper, NULL, CLSCTX_INPROC_SERVER,
            &IID_IBaseFilter, (void **)&filter);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    return filter;
}

static ULONG get_refcount(void *iface)
{
    IUnknown *unknown = iface;
    IUnknown_AddRef(unknown);
    return IUnknown_Release(unknown);
}

#define check_interface(a, b, c) check_interface_(__LINE__, a, b, c)
static void check_interface_(unsigned int line, void *iface_ptr, REFIID iid, BOOL supported)
{
    IUnknown *iface = iface_ptr;
    HRESULT hr, expected_hr;
    IUnknown *unk;

    expected_hr = supported ? S_OK : E_NOINTERFACE;

    hr = IUnknown_QueryInterface(iface, iid, (void **)&unk);
    ok_(__FILE__, line)(hr == expected_hr, "Got hr %#x, expected %#x.\n", hr, expected_hr);
    if (SUCCEEDED(hr))
        IUnknown_Release(unk);
}

static void test_interfaces(void)
{
    IBaseFilter *filter = create_acm_wrapper();

    check_interface(filter, &IID_IBaseFilter, TRUE);
    check_interface(filter, &IID_IMediaFilter, TRUE);
    check_interface(filter, &IID_IPersist, TRUE);
    todo_wine check_interface(filter, &IID_IPersistPropertyBag, TRUE);
    check_interface(filter, &IID_IUnknown, TRUE);

    check_interface(filter, &IID_IAMFilterMiscFlags, FALSE);
    check_interface(filter, &IID_IBasicAudio, FALSE);
    check_interface(filter, &IID_IBasicVideo, FALSE);
    check_interface(filter, &IID_IKsPropertySet, FALSE);
    todo_wine check_interface(filter, &IID_IMediaPosition, FALSE);
    todo_wine check_interface(filter, &IID_IMediaSeeking, FALSE);
    check_interface(filter, &IID_IPin, FALSE);
    todo_wine check_interface(filter, &IID_IQualityControl, FALSE);
    check_interface(filter, &IID_IQualProp, FALSE);
    check_interface(filter, &IID_IReferenceClock, FALSE);
    check_interface(filter, &IID_IVideoWindow, FALSE);

    IBaseFilter_Release(filter);
}

static void test_enum_pins(void)
{
    IBaseFilter *filter = create_acm_wrapper();
    IEnumPins *enum1, *enum2;
    ULONG count, ref;
    IPin *pins[3];
    HRESULT hr;

    ref = get_refcount(filter);
    ok(ref == 1, "Got unexpected refcount %d.\n", ref);

    hr = IBaseFilter_EnumPins(filter, NULL);
    ok(hr == E_POINTER, "Got hr %#x.\n", hr);

    hr = IBaseFilter_EnumPins(filter, &enum1);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    ref = get_refcount(filter);
    ok(ref == 2, "Got unexpected refcount %d.\n", ref);
    ref = get_refcount(enum1);
    ok(ref == 1, "Got unexpected refcount %d.\n", ref);

    hr = IEnumPins_Next(enum1, 1, NULL, NULL);
    ok(hr == E_POINTER, "Got hr %#x.\n", hr);

    hr = IEnumPins_Next(enum1, 1, pins, NULL);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    ref = get_refcount(filter);
todo_wine
    ok(ref == 3, "Got unexpected refcount %d.\n", ref);
    ref = get_refcount(pins[0]);
todo_wine
    ok(ref == 3, "Got unexpected refcount %d.\n", ref);
    ref = get_refcount(enum1);
    ok(ref == 1, "Got unexpected refcount %d.\n", ref);
    IPin_Release(pins[0]);
    ref = get_refcount(filter);
    ok(ref == 2, "Got unexpected refcount %d.\n", ref);

    hr = IEnumPins_Next(enum1, 1, pins, NULL);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    ref = get_refcount(filter);
todo_wine
    ok(ref == 3, "Got unexpected refcount %d.\n", ref);
    ref = get_refcount(pins[0]);
todo_wine
    ok(ref == 3, "Got unexpected refcount %d.\n", ref);
    ref = get_refcount(enum1);
    ok(ref == 1, "Got unexpected refcount %d.\n", ref);
    IPin_Release(pins[0]);
    ref = get_refcount(filter);
    ok(ref == 2, "Got unexpected refcount %d.\n", ref);

    hr = IEnumPins_Next(enum1, 1, pins, NULL);
    ok(hr == S_FALSE, "Got hr %#x.\n", hr);

    hr = IEnumPins_Reset(enum1);
    ok(hr == S_OK, "Got hr %#x.\n", hr);

    hr = IEnumPins_Next(enum1, 1, pins, &count);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    ok(count == 1, "Got count %u.\n", count);
    IPin_Release(pins[0]);

    hr = IEnumPins_Next(enum1, 1, pins, &count);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    ok(count == 1, "Got count %u.\n", count);
    IPin_Release(pins[0]);

    hr = IEnumPins_Next(enum1, 1, pins, &count);
    ok(hr == S_FALSE, "Got hr %#x.\n", hr);
    ok(!count, "Got count %u.\n", count);

    hr = IEnumPins_Reset(enum1);
    ok(hr == S_OK, "Got hr %#x.\n", hr);

    hr = IEnumPins_Next(enum1, 2, pins, NULL);
    ok(hr == E_INVALIDARG, "Got hr %#x.\n", hr);

    hr = IEnumPins_Next(enum1, 2, pins, &count);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    ok(count == 2, "Got count %u.\n", count);
    IPin_Release(pins[0]);
    IPin_Release(pins[1]);

    hr = IEnumPins_Next(enum1, 2, pins, &count);
    ok(hr == S_FALSE, "Got hr %#x.\n", hr);
    ok(!count, "Got count %u.\n", count);

    hr = IEnumPins_Reset(enum1);
    ok(hr == S_OK, "Got hr %#x.\n", hr);

    hr = IEnumPins_Next(enum1, 3, pins, &count);
    ok(hr == S_FALSE, "Got hr %#x.\n", hr);
    ok(count == 2, "Got count %u.\n", count);
    IPin_Release(pins[0]);
    IPin_Release(pins[1]);

    hr = IEnumPins_Reset(enum1);
    ok(hr == S_OK, "Got hr %#x.\n", hr);

    hr = IEnumPins_Clone(enum1, &enum2);
    ok(hr == S_OK, "Got hr %#x.\n", hr);

    hr = IEnumPins_Skip(enum1, 3);
    ok(hr == S_FALSE, "Got hr %#x.\n", hr);

    hr = IEnumPins_Skip(enum1, 2);
    ok(hr == S_OK, "Got hr %#x.\n", hr);

    hr = IEnumPins_Skip(enum1, 1);
    ok(hr == S_FALSE, "Got hr %#x.\n", hr);

    hr = IEnumPins_Next(enum1, 1, pins, NULL);
    ok(hr == S_FALSE, "Got hr %#x.\n", hr);

    hr = IEnumPins_Next(enum2, 1, pins, NULL);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    IPin_Release(pins[0]);

    IEnumPins_Release(enum2);
    IEnumPins_Release(enum1);
    ref = IBaseFilter_Release(filter);
    ok(!ref, "Got outstanding refcount %d.\n", ref);
}

static void test_find_pin(void)
{
    static const WCHAR input_pinW[] = {'i','n','p','u','t',' ','p','i','n',0};
    static const WCHAR output_pinW[] = {'o','u','t','p','u','t',' ','p','i','n',0};
    IBaseFilter *filter = create_acm_wrapper();
    IEnumPins *enum_pins;
    IPin *pin, *pin2;
    HRESULT hr;
    ULONG ref;

    hr = IBaseFilter_EnumPins(filter, &enum_pins);
    ok(hr == S_OK, "Got hr %#x.\n", hr);

    hr = IBaseFilter_FindPin(filter, sink_id, &pin);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    hr = IEnumPins_Next(enum_pins, 1, &pin2, NULL);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    ok(pin == pin2, "Pins didn't match.\n");
    IPin_Release(pin);
    IPin_Release(pin2);

    hr = IBaseFilter_FindPin(filter, source_id, &pin);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    hr = IEnumPins_Next(enum_pins, 1, &pin2, NULL);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    ok(pin == pin2, "Pins didn't match.\n");
    IPin_Release(pin);
    IPin_Release(pin2);

    hr = IBaseFilter_FindPin(filter, sink_name, &pin);
    ok(hr == VFW_E_NOT_FOUND, "Got hr %#x.\n", hr);
    hr = IBaseFilter_FindPin(filter, source_name, &pin);
    ok(hr == VFW_E_NOT_FOUND, "Got hr %#x.\n", hr);
    hr = IBaseFilter_FindPin(filter, input_pinW, &pin);
    ok(hr == VFW_E_NOT_FOUND, "Got hr %#x.\n", hr);
    hr = IBaseFilter_FindPin(filter, output_pinW, &pin);
    ok(hr == VFW_E_NOT_FOUND, "Got hr %#x.\n", hr);

    IEnumPins_Release(enum_pins);
    ref = IBaseFilter_Release(filter);
    ok(!ref, "Got outstanding refcount %d.\n", ref);
}

static void test_pin_info(void)
{
    IBaseFilter *filter = create_acm_wrapper();
    PIN_DIRECTION dir;
    PIN_INFO info;
    HRESULT hr;
    WCHAR *id;
    ULONG ref;
    IPin *pin;

    hr = IBaseFilter_FindPin(filter, sink_id, &pin);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    ref = get_refcount(filter);
    todo_wine ok(ref == 2, "Got unexpected refcount %d.\n", ref);
    ref = get_refcount(pin);
    ok(ref == 2, "Got unexpected refcount %d.\n", ref);

    hr = IPin_QueryPinInfo(pin, &info);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    ok(info.pFilter == filter, "Expected filter %p, got %p.\n", filter, info.pFilter);
    ok(info.dir == PINDIR_INPUT, "Got direction %d.\n", info.dir);
todo_wine
    ok(!lstrcmpW(info.achName, sink_name), "Got name %s.\n", wine_dbgstr_w(info.achName));
    ref = get_refcount(filter);
    todo_wine ok(ref == 3, "Got unexpected refcount %d.\n", ref);
    ref = get_refcount(pin);
    todo_wine ok(ref == 3, "Got unexpected refcount %d.\n", ref);
    IBaseFilter_Release(info.pFilter);

    hr = IPin_QueryDirection(pin, &dir);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    ok(dir == PINDIR_INPUT, "Got direction %d.\n", dir);

    hr = IPin_QueryId(pin, &id);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    ok(!lstrcmpW(id, sink_id), "Got id %s.\n", wine_dbgstr_w(id));
    CoTaskMemFree(id);

    hr = IPin_QueryInternalConnections(pin, NULL, NULL);
    ok(hr == E_NOTIMPL, "Got hr %#x.\n", hr);

    IPin_Release(pin);

    hr = IBaseFilter_FindPin(filter, source_id, &pin);
    ok(hr == S_OK, "Got hr %#x.\n", hr);

    check_interface(pin, &IID_IPin, TRUE);
    check_interface(pin, &IID_IMediaSeeking, TRUE);

    hr = IPin_QueryPinInfo(pin, &info);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    ok(info.pFilter == filter, "Expected filter %p, got %p.\n", filter, info.pFilter);
    ok(info.dir == PINDIR_OUTPUT, "Got direction %d.\n", info.dir);
todo_wine
    ok(!lstrcmpW(info.achName, source_name), "Got name %s.\n", wine_dbgstr_w(info.achName));
    IBaseFilter_Release(info.pFilter);

    hr = IPin_QueryDirection(pin, &dir);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    ok(dir == PINDIR_OUTPUT, "Got direction %d.\n", dir);

    hr = IPin_QueryId(pin, &id);
    ok(hr == S_OK, "Got hr %#x.\n", hr);
    ok(!lstrcmpW(id, source_id), "Got id %s.\n", wine_dbgstr_w(id));
    CoTaskMemFree(id);

    hr = IPin_QueryInternalConnections(pin, NULL, NULL);
    ok(hr == E_NOTIMPL, "Got hr %#x.\n", hr);

    IPin_Release(pin);

    ref = IBaseFilter_Release(filter);
    ok(!ref, "Got outstanding refcount %d.\n", ref);
}

START_TEST(acmwrapper)
{
    CoInitialize(NULL);

    test_interfaces();
    test_enum_pins();
    test_find_pin();
    test_pin_info();

    CoUninitialize();
}
