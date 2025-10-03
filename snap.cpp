#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <fstream>
#include <webp/encode.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "libwebp.lib")
#pragma comment(lib, "libsharpyuv.lib")

// Service-related globals
SERVICE_STATUS g_ServiceStatus = { 0 };
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE g_ServiceStopEvent = INVALID_HANDLE_VALUE;

#define SERVICE_NAME L"cam"

// Helper function to get the directory of the executable
std::wstring GetExeDirectory() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath); // Remove the executable name to get the directory
    return std::wstring(exePath);
}

// Helper function to ensure Temp directory exists
std::wstring EnsureTempDirectory() {
    std::wstring tempDir = GetExeDirectory() + L"\\Temp";
    CreateDirectoryW(tempDir.c_str(), NULL); // Create if it doesn't exist
    return tempDir;
}

// Simple file logging for service debugging
void LogToFile(const std::wstring& message) {
    std::wstring logPath = EnsureTempDirectory() + L"\\service.log"; // Log in Temp folder
    std::wofstream file(logPath, std::ios::app);
    if (file.is_open()) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t timeStr[64];
        swprintf_s(timeStr, L"%04d-%02d-%02d %02d:%02d:%02d: ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        file << timeStr << message << L"\n";
        file.close();
    }
}

// Save BGRA bitmap to WebP using libwebp
void SaveBitmapToWebP(BYTE* pData, UINT32 width, UINT32 height, const std::wstring& filePath) {
    // pData is BGRA; convert to RGBA for WebP
    std::vector<BYTE> rgbaData(width * height * 4);
    BYTE* rgba = rgbaData.data();
    for (UINT32 y = 0; y < height; ++y) {
        BYTE* srcRow = pData + y * width * 4;
        BYTE* dstRow = rgba + y * width * 4;
        for (UINT32 x = 0; x < width; ++x) {
            BYTE b = srcRow[x * 4 + 0];
            BYTE g = srcRow[x * 4 + 1];
            BYTE r = srcRow[x * 4 + 2];
            BYTE a = srcRow[x * 4 + 3];
            dstRow[x * 4 + 0] = r;
            dstRow[x * 4 + 1] = g;
            dstRow[x * 4 + 2] = b;
            dstRow[x * 4 + 3] = a;
        }
    }

    uint8_t* webpData = nullptr;
    int webpSize = WebPEncodeRGBA(rgba, width, height, width * 4, 80.0f, &webpData);
    if (webpSize == 0) {
        LogToFile(L"Failed to encode WebP");
        WebPFree(webpData);
        return;
    }

    std::ofstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        LogToFile(L"Failed to open WebP file for writing: " + filePath);
        WebPFree(webpData);
        return;
    }

    file.write(reinterpret_cast<char*>(webpData), webpSize);
    file.close();
    WebPFree(webpData);

    LogToFile(L"Saved frame to: " + filePath);
}

// Utility: Converts YUY2 buffer to BGRA buffer (Windows RGB32 convention)
void YUY2ToRGB32(const BYTE* yuy2, BYTE* bgra32, UINT32 width, UINT32 height) {
    for (UINT32 i = 0; i < width * height; i += 2) {
        int y0 = yuy2[i * 2 + 0];
        int u = yuy2[i * 2 + 1] - 128;
        int y1 = yuy2[i * 2 + 2];
        int v = yuy2[i * 2 + 3] - 128;

        auto clamp = [](int val) {
            if (val < 0) return 0;
            if (val > 255) return 255;
            return val;
            };

        int c0 = y0 - 16;
        int c1 = y1 - 16;
        int d = u;
        int e = v;

        int r0 = clamp((298 * c0 + 409 * e + 128) >> 8);
        int g0 = clamp((298 * c0 - 100 * d - 208 * e + 128) >> 8);
        int b0 = clamp((298 * c0 + 516 * d + 128) >> 8);

        int r1 = clamp((298 * c1 + 409 * e + 128) >> 8);
        int g1 = clamp((298 * c1 - 100 * d - 208 * e + 128) >> 8);
        int b1 = clamp((298 * c1 + 516 * d + 128) >> 8);

        bgra32[i * 4 + 0] = b0;
        bgra32[i * 4 + 1] = g0;
        bgra32[i * 4 + 2] = r0;
        bgra32[i * 4 + 3] = 255;

        bgra32[(i + 1) * 4 + 0] = b1;
        bgra32[(i + 1) * 4 + 1] = g1;
        bgra32[(i + 1) * 4 + 2] = r1;
        bgra32[(i + 1) * 4 + 3] = 255;
    }
}

// Function to capture a single frame
DWORD CaptureFrame() {
    HRESULT hr;

    IMFAttributes* pAttributes = nullptr;
    hr = MFCreateAttributes(&pAttributes, 1);
    if (FAILED(hr)) {
        LogToFile(L"MFCreateAttributes failed: 0x" + std::to_wstring(hr));
        return hr;
    }

    hr = pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) {
        LogToFile(L"SetGUID failed: 0x" + std::to_wstring(hr));
        pAttributes->Release();
        return hr;
    }

    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(pAttributes, &ppDevices, &count);
    pAttributes->Release();
    if (FAILED(hr) || count == 0) {
        LogToFile(L"No camera devices found or error: 0x" + std::to_wstring(hr));
        return hr;
    }

    IMFMediaSource* pSource = nullptr;
    hr = ppDevices[0]->ActivateObject(IID_PPV_ARGS(&pSource));
    for (UINT32 i = 0; i < count; i++) {
        ppDevices[i]->Release();
    }
    CoTaskMemFree(ppDevices);
    if (FAILED(hr)) {
        LogToFile(L"ActivateObject failed: 0x" + std::to_wstring(hr));
        return hr;
    }

    IMFSourceReader* pReader = nullptr;
    hr = MFCreateSourceReaderFromMediaSource(pSource, NULL, &pReader);
    if (FAILED(hr)) {
        LogToFile(L"MFCreateSourceReaderFromMediaSource failed: 0x" + std::to_wstring(hr));
        pSource->Release();
        return hr;
    }

    bool foundType = false;
    DWORD i = 0;
    GUID subtype = { 0 };
    while (true) {
        IMFMediaType* pType = nullptr;
        hr = pReader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &pType);
        if (FAILED(hr)) break;

        hr = pType->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (SUCCEEDED(hr)) {
            if (subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_YUY2) {
                hr = pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pType);
                if (SUCCEEDED(hr)) {
                    foundType = true;
                    pType->Release();
                    break;
                }
            }
        }
        pType->Release();
        i++;
    }

    if (!foundType) {
        LogToFile(L"Failed to set a supported video format (RGB32 or YUY2)");
        pReader->Release();
        pSource->Release();
        return ERROR_UNSUPPORTED_TYPE;
    }

    IMFMediaType* pCurrentType = nullptr;
    UINT32 width = 0, height = 0;
    hr = pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrentType);
    if (FAILED(hr) || !pCurrentType) {
        LogToFile(L"GetCurrentMediaType failed: 0x" + std::to_wstring(hr));
        pReader->Release();
        pSource->Release();
        return hr;
    }
    MFGetAttributeSize(pCurrentType, MF_MT_FRAME_SIZE, &width, &height);
    pCurrentType->Release();

    bool captured = false;
    while (!captured) {
        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* pSample = nullptr;

        hr = pReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &timestamp, &pSample);
        if (FAILED(hr)) {
            LogToFile(L"ReadSample failed: 0x" + std::to_wstring(hr));
            break;
        }

        if (!pSample) {
            LogToFile(L"Empty sample, retrying...");
            Sleep(100);
            continue;
        }

        IMFMediaBuffer* pBuffer = nullptr;
        hr = pSample->ConvertToContiguousBuffer(&pBuffer);
        if (FAILED(hr) || !pBuffer) {
            LogToFile(L"ConvertToContiguousBuffer failed: 0x" + std::to_wstring(hr));
            pSample->Release();
            break;
        }

        BYTE* pData = nullptr;
        DWORD maxLen = 0, currentLen = 0;
        hr = pBuffer->Lock(&pData, &maxLen, &currentLen);
        if (FAILED(hr) || !pData) {
            LogToFile(L"Buffer lock failed: 0x" + std::to_wstring(hr));
            pBuffer->Release();
            pSample->Release();
            break;
        }

        std::vector<BYTE> bgraData;
        if (subtype == MFVideoFormat_YUY2) {
            bgraData.resize(width * height * 4);
            YUY2ToRGB32(pData, bgraData.data(), width, height);
        }
        else if (subtype == MFVideoFormat_RGB32) {
            bgraData.assign(pData, pData + width * height * 4);
        }

        wchar_t filePath[MAX_PATH];
        SYSTEMTIME st;
        GetLocalTime(&st);
        std::wstring tempDir = EnsureTempDirectory(); // Use Temp folder in current directory
        swprintf_s(filePath, MAX_PATH, L"%s\\capture_%04d%02d%02d_%02d%02d%02d.webp",
            tempDir.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        SaveBitmapToWebP(bgraData.data(), width, height, filePath);

        pBuffer->Unlock();
        pBuffer->Release();
        pSample->Release();
        captured = true;
    }

    pSource->Stop();
    pReader->Release();
    pSource->Release();
    return S_OK;
}

// Main loop for capturing frames
DWORD CaptureFrameLoop(BOOL isService) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LogToFile(L"CoInitializeEx failed: 0x" + std::to_wstring(hr));
        return hr;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        LogToFile(L"MFStartup failed: 0x" + std::to_wstring(hr));
        CoUninitialize();
        return hr;
    }

    if (isService) {
        while (WaitForSingleObject(g_ServiceStopEvent, 10000) == WAIT_TIMEOUT) {
            CaptureFrame();
        }
    }
    else {
        while (true) {
            CaptureFrame();
            LogToFile(L"Waiting 10 seconds for next capture...");
            Sleep(2000);
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                LogToFile(L"Escape key pressed, exiting...");
                break;
            }
        }
    }

    MFShutdown();
    CoUninitialize();
    return S_OK;
}

// Service control handler
VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode) {
    switch (CtrlCode) {
    case SERVICE_CONTROL_STOP:
        if (g_ServiceStatus.dwCurrentState != SERVICE_RUNNING)
            break;

        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        SetEvent(g_ServiceStopEvent);
        break;

    default:
        break;
    }
}

// Service main function
VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv) {
    g_StatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, ServiceCtrlHandler);
    if (g_StatusHandle == NULL) {
        LogToFile(L"RegisterServiceCtrlHandler failed");
        return;
    }

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (g_ServiceStopEvent == NULL) {
        LogToFile(L"CreateEvent failed");
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    CaptureFrameLoop(TRUE);

    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    CloseHandle(g_ServiceStopEvent);
}

// Main entry point
int wmain() {
    SERVICE_TABLE_ENTRY ServiceTable[] = {
        { (LPWSTR)SERVICE_NAME, (LPSERVICE_MAIN_FUNCTION)ServiceMain },
        { NULL, NULL }
    };

    if (StartServiceCtrlDispatcher(ServiceTable)) {
        return 0;
    }
    else {
        DWORD error = GetLastError();
        if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            LogToFile(L"Running in console mode (press ESC to exit)...");
            return CaptureFrameLoop(FALSE);
        }
        LogToFile(L"StartServiceCtrlDispatcher failed: " + std::to_wstring(error));
        return error;
    }
}