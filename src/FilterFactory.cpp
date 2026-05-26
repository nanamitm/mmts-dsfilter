// Filter factory registration - uses DirectShow BaseClasses mechanism.
#include <streams.h>
#include "Guids.h"
#include "MmtTlvSplitter.h"

// ---------------------------------------------------------------------------
// Filter registration info
// ---------------------------------------------------------------------------
static const WCHAR kFilterName[] = L"MMT/TLV Splitter";

// File extensions handled by this filter
static const REGPINTYPES kVideoTypes[] = {
    { &MEDIATYPE_Video, &MEDIASUBTYPE_HEVC }
};
static const REGPINTYPES kAudioTypes[] = {
    { &MEDIATYPE_Audio, &MEDIASUBTYPE_RAW_AAC1 }
};
static const REGFILTERPINS kPins[] = {
    // Video output
    { nullptr, FALSE, TRUE, FALSE, FALSE, &CLSID_NULL, nullptr,
      ARRAYSIZE(kVideoTypes), kVideoTypes },
    // Audio output
    { nullptr, FALSE, TRUE, FALSE, FALSE, &CLSID_NULL, nullptr,
      ARRAYSIZE(kAudioTypes), kAudioTypes },
};

// CLSID_LegacyAmFilterCategory = {083863F1-70DE-11d0-BD40-00A0C911CE86}
static const GUID CLSID_LegacyAmFilter = { 0x083863F1, 0x70DE, 0x11d0,
    { 0xBD, 0x40, 0x00, 0xA0, 0xC9, 0x11, 0xCE, 0x86 } };

static const AMOVIESETUP_FILTER kFilterSetup = {
    &CLSID_MmtTlvSplitter,
    kFilterName,
    MERIT_NORMAL,
    ARRAYSIZE(kPins),
    kPins
};

// ---------------------------------------------------------------------------
// BaseClasses template table - one entry per filter in this DLL
// ---------------------------------------------------------------------------
CFactoryTemplate g_Templates[] = {
    {
        kFilterName,
        &CLSID_MmtTlvSplitter,
        CMmtTlvSplitter::CreateInstance,
        nullptr,
        &kFilterSetup
    }
};
int g_cTemplates = ARRAYSIZE(g_Templates);

// ---------------------------------------------------------------------------
// Register/unregister ".mmts" extension so DirectShow picks up this filter
// HKEY_CLASSES_ROOT\Media Type\Extensions\.mmts  "Source Filter" = CLSID
// ---------------------------------------------------------------------------
static const WCHAR kExtKeyPath[] = L"Media Type\\Extensions\\.mmts";
static const WCHAR kSourceFilterValue[] = L"Source Filter";
// {8b7d1a60-3c4e-4f2a-9b8e-1234567890ab}
static const WCHAR kClsidStr[] = L"{8B7D1A60-3C4E-4F2A-9B8E-1234567890AB}";

static HRESULT RegisterExtension()
{
    HKEY hKey = nullptr;
    LONG r = RegCreateKeyExW(HKEY_CLASSES_ROOT, kExtKeyPath, 0, nullptr,
                             REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
    r = RegSetValueExW(hKey, kSourceFilterValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(kClsidStr),
                       static_cast<DWORD>((wcslen(kClsidStr) + 1) * sizeof(WCHAR)));
    RegCloseKey(hKey);
    return HRESULT_FROM_WIN32(r);
}

static HRESULT UnregisterExtension()
{
    RegDeleteKeyW(HKEY_CLASSES_ROOT, kExtKeyPath);
    return S_OK;
}

// ---------------------------------------------------------------------------
// Standard DLL exports used by regsvr32
// ---------------------------------------------------------------------------
STDAPI DllRegisterServer()
{
    HRESULT hr = AMovieDllRegisterServer2(TRUE);
    if (SUCCEEDED(hr)) hr = RegisterExtension();
    return hr;
}

STDAPI DllUnregisterServer()
{
    UnregisterExtension();
    return AMovieDllRegisterServer2(FALSE);
}

// Wire Windows DllMain → BaseClasses DllEntryPoint (which sets g_hInst).
extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE hInstance, ULONG ulReason, LPVOID pv);

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID pv)
{
    return DllEntryPoint(hInstance, dwReason, pv);
}
