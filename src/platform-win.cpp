#ifdef _WIN32

#include "platform.h"
#include <shobjidl.h>
#include <windows.h>
#include <SFML/Graphics.hpp>
#include <iostream>

bool loadAppIcon(sf::Image& icon) {
    // 1. Force the Taskbar and Alt-Tab switcher to use your native .ico (IDI_ICON = 1)
    // GetActiveWindow() gets the HWND of our newly spawned SFML window
    HWND hwnd = GetActiveWindow(); 
    if (hwnd) {
        HICON hIconBig = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
        HICON hIconSmall = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(1), IMAGE_ICON, 16, 16, 0);
        
        if (hIconBig) {
            SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
        }
        if (hIconSmall) {
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
        }
    }

    // 2. Load the PNG resource (ID: 101) for SFML's decoration
    HRSRC hRes = FindResource(NULL, MAKEINTRESOURCE(101), RT_RCDATA);
    if (!hRes) return false;

    HGLOBAL hGlob = LoadResource(NULL, hRes);
    if (!hGlob) return false;

    DWORD size = SizeofResource(NULL, hRes);
    const void* data = LockResource(hGlob);

    if (data && size > 0) {
        return icon.loadFromMemory(data, size);
    }

    return false;
}


std::string openFolderDialog()
{
    std::string result;

    IFileDialog* dialog = nullptr;

    HRESULT hr = CoCreateInstance(
        CLSID_FileOpenDialog,
        NULL,
        CLSCTX_ALL,
        IID_PPV_ARGS(&dialog)
    );


    if (SUCCEEDED(hr))
    {
        DWORD options;

        dialog->GetOptions(&options);

        dialog->SetOptions(
            options | FOS_PICKFOLDERS
        );


        if (SUCCEEDED(dialog->Show(NULL)))
        {
            IShellItem* item;

            if (SUCCEEDED(dialog->GetResult(&item)))
            {
                PWSTR path;

                item->GetDisplayName(
                    SIGDN_FILESYSPATH,
                    &path
                );


                char buffer[MAX_PATH];

                WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    path,
                    -1,
                    buffer,
                    MAX_PATH,
                    nullptr,
                    nullptr
                );


                result = buffer;

                CoTaskMemFree(path);
                item->Release();
            }
        }


        dialog->Release();
    }


    return result;
}
#endif