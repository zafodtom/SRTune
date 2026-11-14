#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "comctl32.lib")

namespace
{
    constexpr wchar_t kTitle[] = L"SRTune Update";
    constexpr wchar_t kLatestRelease[] =
        L"https://api.github.com/repos/zafodtom/SRTune/releases/latest";
    constexpr wchar_t kUninstallKey[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        L"{9DA1AFE7-287A-44A5-84DB-4F9ADB5A988C}_is1";
    constexpr std::uint64_t kMaxInstallerBytes = 700ull * 1024ull * 1024ull;

    struct SemVer
    {
        int major{};
        int minor{};
        int patch{};
    };

    struct ProgressWindow
    {
        HWND window{};
        HWND label{};
        HWND progress{};

        explicit ProgressWindow(std::wstring const& text)
        {
            INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_PROGRESS_CLASS };
            InitCommonControlsEx(&controls);

            auto const instance = GetModuleHandleW(nullptr);
            static bool registered = false;
            if (!registered)
            {
                WNDCLASSW type{};
                type.lpfnWndProc = DefWindowProcW;
                type.hInstance = instance;
                type.lpszClassName = L"SRTuneUpdateProgress";
                type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
                type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                RegisterClassW(&type);
                registered = true;
            }

            window = CreateWindowExW(
                WS_EX_DLGMODALFRAME,
                L"SRTuneUpdateProgress",
                kTitle,
                WS_CAPTION | WS_VISIBLE,
                (GetSystemMetrics(SM_CXSCREEN) - 470) / 2,
                (GetSystemMetrics(SM_CYSCREEN) - 145) / 2,
                470, 145,
                nullptr, nullptr, instance, nullptr);

            if (!window)
                return;

            label = CreateWindowExW(
                0, L"STATIC", text.c_str(),
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                18, 24, 425, 30,
                window, nullptr, instance, nullptr);
            SendMessageW(label, WM_SETFONT,
                reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);

            progress = CreateWindowExW(
                0, PROGRESS_CLASSW, nullptr,
                WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                28, 66, 405, 20,
                window, nullptr, instance, nullptr);
            SendMessageW(progress, PBM_SETRANGE32, 0, 100);
            UpdateWindow(window);
            SetForegroundWindow(window);
        }

        ~ProgressWindow()
        {
            close();
        }

        void close()
        {
            if (window)
            {
                DestroyWindow(window);
                window = nullptr;
                label = nullptr;
                progress = nullptr;
            }
        }

        void set(int value)
        {
            if (!progress)
                return;
            SendMessageW(progress, PBM_SETPOS,
                static_cast<WPARAM>(std::clamp(value, 0, 100)), 0);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        void text(std::wstring const& value)
        {
            if (label)
                SetWindowTextW(label, value.c_str());
        }
    };

    std::wstring quote(std::wstring const& value)
    {
        std::wstring out = L"\"";
        size_t slashes = 0;
        for (wchar_t c : value)
        {
            if (c == L'\\')
            {
                ++slashes;
                continue;
            }
            if (c == L'\"')
            {
                out.append(slashes * 2 + 1, L'\\');
                out += c;
                slashes = 0;
            }
            else
            {
                out.append(slashes, L'\\');
                slashes = 0;
                out += c;
            }
        }
        out.append(slashes * 2, L'\\');
        out += L'\"';
        return out;
    }

    void message(std::wstring const& value, UINT icon = MB_ICONINFORMATION)
    {
        MessageBoxW(nullptr, value.c_str(), kTitle, MB_OK | icon);
    }

    bool parseVersion(std::wstring const& text, SemVer& version)
    {
        static std::wregex const pattern(L"^([0-9]+)\\.([0-9]+)\\.([0-9]+)$");
        std::wsmatch match;
        if (!std::regex_match(text, match, pattern))
            return false;
        try
        {
            version.major = std::stoi(match[1].str());
            version.minor = std::stoi(match[2].str());
            version.patch = std::stoi(match[3].str());
            return version.major >= 0 && version.minor >= 0 && version.patch >= 0;
        }
        catch (...)
        {
            return false;
        }
    }

    int compare(SemVer const& left, SemVer const& right)
    {
        if (left.major != right.major) return left.major < right.major ? -1 : 1;
        if (left.minor != right.minor) return left.minor < right.minor ? -1 : 1;
        if (left.patch != right.patch) return left.patch < right.patch ? -1 : 1;
        return 0;
    }

    bool queryRegistryString(HKEY root, wchar_t const* subkey,
        wchar_t const* name, std::wstring& value)
    {
        HKEY key{};
        if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
            return false;

        DWORD type{};
        DWORD bytes{};
        auto result = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
        if (result != ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ) ||
            bytes < sizeof(wchar_t))
        {
            RegCloseKey(key);
            return false;
        }

        std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
        result = RegQueryValueExW(key, name, nullptr, &type,
            reinterpret_cast<BYTE*>(buffer.data()), &bytes);
        RegCloseKey(key);
        if (result != ERROR_SUCCESS)
            return false;

        value.assign(buffer.data());
        return !value.empty();
    }

    bool installedVersion(std::wstring& version)
    {
        return queryRegistryString(HKEY_CURRENT_USER, kUninstallKey,
            L"DisplayVersion", version);
    }

    bool allowedHost(std::wstring host)
    {
        std::transform(host.begin(), host.end(), host.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        return host == L"api.github.com" ||
            host == L"github.com" ||
            host == L"release-assets.githubusercontent.com" ||
            host == L"objects.githubusercontent.com";
    }

    bool allowedHttpsUrl(std::wstring const& url)
    {
        URL_COMPONENTSW parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t scheme[16]{};
        wchar_t host[256]{};
        parts.lpszScheme = scheme;
        parts.dwSchemeLength = static_cast<DWORD>(std::size(scheme));
        parts.lpszHostName = host;
        parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
            return false;
        return parts.nScheme == INTERNET_SCHEME_HTTPS &&
            (parts.nPort == INTERNET_DEFAULT_HTTPS_PORT || parts.nPort == 443) &&
            allowedHost(std::wstring(host, parts.dwHostNameLength));
    }

    bool finalRequestUrlAllowed(HINTERNET request)
    {
        DWORD bytes = 0;
        WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &bytes);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t))
            return false;
        std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
        if (!WinHttpQueryOption(request, WINHTTP_OPTION_URL, buffer.data(), &bytes))
            return false;
        return allowedHttpsUrl(buffer.data());
    }

    bool openRequest(std::wstring const& url,
        HINTERNET& session, HINTERNET& connection, HINTERNET& request,
        std::wstring& error)
    {
        session = connection = request = nullptr;
        if (!allowedHttpsUrl(url))
        {
            error = L"Adresa aktualizace není povolená.";
            return false;
        }

        URL_COMPONENTSW parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256]{};
        wchar_t path[4096]{};
        parts.lpszHostName = host;
        parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
        {
            error = L"Neplatná adresa aktualizace.";
            return false;
        }

        session = WinHttpOpen(L"SRTune-Update/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
        {
            error = L"Nepodařilo se inicializovat síťové připojení.";
            return false;
        }

        WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);

        connection = WinHttpConnect(session,
            std::wstring(host, parts.dwHostNameLength).c_str(),
            parts.nPort, 0);
        if (!connection)
        {
            error = L"Nepodařilo se připojit ke GitHubu.";
            return false;
        }

        std::wstring object(path, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength && parts.lpszExtraInfo)
            object.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

        request = WinHttpOpenRequest(connection, L"GET", object.c_str(),
            nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request)
        {
            error = L"Nepodařilo se vytvořit požadavek aktualizace.";
            return false;
        }

        wchar_t const headers[] =
            L"Accept: application/vnd.github+json\r\n"
            L"X-GitHub-Api-Version: 2022-11-28\r\n";
        WinHttpAddRequestHeaders(request, headers, -1,
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

        if (!WinHttpSendRequest(request,
                WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr))
        {
            error = L"GitHub neodpověděl na požadavek aktualizace.";
            return false;
        }

        DWORD status{};
        DWORD size = sizeof(status);
        if (!WinHttpQueryHeaders(request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                WINHTTP_NO_HEADER_INDEX) ||
            status != 200)
        {
            error = L"GitHub vrátil chybu HTTP " + std::to_wstring(status) + L".";
            return false;
        }

        if (!finalRequestUrlAllowed(request))
        {
            error = L"GitHub přesměroval aktualizaci na nepovolenou adresu.";
            return false;
        }

        return true;
    }

    void closeInternet(HINTERNET request, HINTERNET connection, HINTERNET session)
    {
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
    }

    bool readUrl(std::wstring const& url, std::size_t maxBytes,
        std::string& output, std::wstring& error)
    {
        HINTERNET session{}, connection{}, request{};
        if (!openRequest(url, session, connection, request, error))
        {
            closeInternet(request, connection, session);
            return false;
        }

        output.clear();
        std::vector<char> buffer(64 * 1024);
        for (;;)
        {
            DWORD read{};
            if (!WinHttpReadData(request, buffer.data(),
                    static_cast<DWORD>(buffer.size()), &read))
            {
                error = L"Čtení dat aktualizace selhalo.";
                closeInternet(request, connection, session);
                return false;
            }
            if (!read)
                break;
            if (output.size() + read > maxBytes)
            {
                error = L"Metadata aktualizace překročila bezpečný limit.";
                closeInternet(request, connection, session);
                return false;
            }
            output.append(buffer.data(), read);
        }

        closeInternet(request, connection, session);
        return true;
    }

    std::wstring utf8(std::string const& text)
    {
        if (text.empty())
            return {};
        auto const count = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS,
            text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (count <= 0)
            return {};
        std::wstring result(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.data(), static_cast<int>(text.size()),
            result.data(), count);
        return result;
    }

    bool regexValue(std::wstring const& text,
        std::wstring const& pattern, std::wstring& value)
    {
        std::wsmatch match;
        if (!std::regex_search(text, match, std::wregex(pattern)) ||
            match.size() < 2)
            return false;
        value = match[1].str();
        return true;
    }

    bool latestVersion(std::wstring& version, std::wstring& error)
    {
        std::string bytes;
        if (!readUrl(kLatestRelease, 2 * 1024 * 1024, bytes, error))
            return false;

        auto const json = utf8(bytes);
        if (json.empty() ||
            json.find(L"\"draft\":true") != std::wstring::npos ||
            json.find(L"\"prerelease\":true") != std::wstring::npos)
        {
            error = L"GitHub vrátil neplatné stabilní vydání.";
            return false;
        }

        if (!regexValue(json,
                L"\\\"tag_name\\\"\\s*:\\s*\\\"v([0-9]+\\.[0-9]+\\.[0-9]+)\\\"",
                version))
        {
            error = L"Nelze zjistit verzi posledního vydání.";
            return false;
        }

        SemVer parsed{};
        if (!parseVersion(version, parsed))
        {
            error = L"GitHub vrátil neplatné číslo verze.";
            return false;
        }
        return true;
    }

    struct Manifest
    {
        std::wstring version;
        std::wstring name;
        std::uint64_t size{};
        std::wstring sha256;
    };

    bool loadManifest(std::wstring const& version,
        Manifest& manifest, std::wstring& error)
    {
        auto const url =
            L"https://github.com/zafodtom/SRTune/releases/download/v" +
            version + L"/srtune-update.json";

        std::string bytes;
        if (!readUrl(url, 64 * 1024, bytes, error))
            return false;

        auto const json = utf8(bytes);
        std::wstring sizeText;
        if (!regexValue(json, L"\\\"version\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"", manifest.version) ||
            !regexValue(json, L"\\\"name\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"", manifest.name) ||
            !regexValue(json, L"\\\"size\\\"\\s*:\\s*([0-9]+)", sizeText) ||
            !regexValue(json, L"\\\"sha256\\\"\\s*:\\s*\\\"([0-9a-fA-F]{64})\\\"", manifest.sha256))
        {
            error = L"Manifest aktualizace je neúplný.";
            return false;
        }

        try
        {
            manifest.size = std::stoull(sizeText);
        }
        catch (...)
        {
            error = L"Manifest obsahuje neplatnou velikost instalátoru.";
            return false;
        }

        std::transform(manifest.sha256.begin(), manifest.sha256.end(),
            manifest.sha256.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

        auto const expectedName =
            L"SRTune-" + version + L"-Setup-x64.exe";
        if (manifest.version != version ||
            manifest.name != expectedName ||
            manifest.size == 0 ||
            manifest.size > kMaxInstallerBytes)
        {
            error = L"Manifest aktualizace neodpovídá vydání.";
            return false;
        }
        return true;
    }

    bool downloadFile(std::wstring const& url,
        std::filesystem::path const& destination,
        std::uint64_t expectedSize,
        ProgressWindow& progress,
        std::wstring& error)
    {
        HINTERNET session{}, connection{}, request{};
        if (!openRequest(url, session, connection, request, error))
        {
            closeInternet(request, connection, session);
            return false;
        }

        HANDLE file = CreateFileW(destination.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = L"Nelze vytvořit dočasný instalační soubor.";
            closeInternet(request, connection, session);
            return false;
        }

        bool ok = true;
        std::uint64_t total = 0;
        std::vector<unsigned char> buffer(256 * 1024);
        while (ok)
        {
            DWORD read{};
            if (!WinHttpReadData(request, buffer.data(),
                    static_cast<DWORD>(buffer.size()), &read))
            {
                error = L"Stahování aktualizace bylo přerušeno.";
                ok = false;
                break;
            }
            if (!read)
                break;

            total += read;
            if (total > expectedSize || total > kMaxInstallerBytes)
            {
                error = L"Stažený soubor má neočekávanou velikost.";
                ok = false;
                break;
            }

            DWORD written{};
            if (!WriteFile(file, buffer.data(), read, &written, nullptr) ||
                written != read)
            {
                error = L"Staženou aktualizaci nelze uložit.";
                ok = false;
                break;
            }

            progress.set(static_cast<int>((total * 100) / expectedSize));
        }

        FlushFileBuffers(file);
        CloseHandle(file);
        closeInternet(request, connection, session);

        if (!ok || total != expectedSize)
        {
            DeleteFileW(destination.c_str());
            if (ok)
                error = L"Stažený soubor nemá očekávanou velikost.";
            return false;
        }

        progress.set(100);
        return true;
    }

    bool verifySha256(std::filesystem::path const& path,
        std::wstring const& expected)
    {
        if (expected.size() != 64)
            return false;

        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(file, &size) &&
            size.QuadPart > 0 &&
            static_cast<std::uint64_t>(size.QuadPart) <= kMaxInstallerBytes;

        BCRYPT_ALG_HANDLE algorithm{};
        BCRYPT_HASH_HANDLE hash{};
        DWORD objectLength{}, resultLength{};
        if (ok)
            ok = BCryptOpenAlgorithmProvider(
                &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
        if (ok)
            ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectLength),
                sizeof(objectLength), &resultLength, 0) >= 0;

        std::vector<UCHAR> object(objectLength);
        UCHAR digest[32]{};
        if (ok)
            ok = BCryptCreateHash(algorithm, &hash,
                object.data(), objectLength, nullptr, 0, 0) >= 0;

        std::vector<UCHAR> buffer(256 * 1024);
        while (ok)
        {
            DWORD read{};
            if (!ReadFile(file, buffer.data(),
                    static_cast<DWORD>(buffer.size()), &read, nullptr))
            {
                ok = false;
                break;
            }
            if (!read)
                break;
            ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
        }

        if (ok)
            ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;

        if (hash)
            BCryptDestroyHash(hash);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(file);

        if (!ok)
            return false;

        wchar_t const* hex = L"0123456789abcdef";
        std::wstring actual;
        actual.reserve(64);
        for (auto byte : digest)
        {
            actual += hex[byte >> 4];
            actual += hex[byte & 15];
        }
        return actual == expected;
    }

    BOOL CALLBACK closeWindowForProcess(HWND hwnd, LPARAM value)
    {
        DWORD pid{};
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == static_cast<DWORD>(value) &&
            IsWindowVisible(hwnd))
        {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return TRUE;
    }

    bool waitForApplicationExit(DWORD pid)
    {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!process)
            return GetLastError() == ERROR_INVALID_PARAMETER;

        EnumWindows(closeWindowForProcess, static_cast<LPARAM>(pid));
        auto const result = WaitForSingleObject(process, 120000);
        CloseHandle(process);
        return result == WAIT_OBJECT_0;
    }

    bool launch(std::wstring const& exe,
        std::wstring const& args,
        bool wait, DWORD& exitCode)
    {
        std::wstring command = quote(exe);
        if (!args.empty())
            command += L" " + args;
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe.c_str(), mutableCommand.data(),
                nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                &startup, &process))
            return false;

        CloseHandle(process.hThread);
        if (wait)
        {
            WaitForSingleObject(process.hProcess, INFINITE);
            GetExitCodeProcess(process.hProcess, &exitCode);
        }
        CloseHandle(process.hProcess);
        return true;
    }

    void markCompleted(std::filesystem::path const& folder)
    {
        std::ofstream marker(folder / L"completed", std::ios::binary);
        marker << "done\n";
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc != 3)
    {
        if (argv) LocalFree(argv);
        message(L"Updater byl spuštěn s neplatnými parametry.", MB_ICONERROR);
        return 1;
    }

    auto const parentPid = static_cast<DWORD>(wcstoul(argv[1], nullptr, 10));
    std::filesystem::path const appPath{ argv[2] };
    LocalFree(argv);

    wchar_t helperBuffer[32768]{};
    GetModuleFileNameW(nullptr, helperBuffer,
        static_cast<DWORD>(std::size(helperBuffer)));
    std::filesystem::path const tempFolder =
        std::filesystem::path(helperBuffer).parent_path();

    struct Completion
    {
        std::filesystem::path folder;
        ~Completion() { markCompleted(folder); }
    } completion{ tempFolder };

    std::wstring currentText;
    if (!installedVersion(currentText))
    {
        message(
            L"Integrovaná aktualizace je určená pro nainstalovanou verzi SRTune.\n\n"
            L"Portable verzi aktualizujte nahrazením novým ZIP balíčkem.");
        return 0;
    }

    SemVer current{};
    if (!parseVersion(currentText, current))
    {
        message(L"Nelze určit aktuálně nainstalovanou verzi SRTune.", MB_ICONERROR);
        return 1;
    }

    ProgressWindow progress(L"Kontroluji dostupnost aktualizace…");

    std::wstring latestText;
    std::wstring error;
    if (!latestVersion(latestText, error))
    {
        progress.close();
        message(error, MB_ICONERROR);
        return 1;
    }

    SemVer latest{};
    if (!parseVersion(latestText, latest))
    {
        progress.close();
        message(L"GitHub vrátil neplatnou verzi aktualizace.", MB_ICONERROR);
        return 1;
    }

    if (compare(latest, current) <= 0)
    {
        progress.close();
        message(L"Používáte aktuální verzi SRTune " + currentText + L".");
        return 0;
    }

    Manifest manifest;
    progress.text(L"Ověřuji informace o nové verzi…");
    if (!loadManifest(latestText, manifest, error))
    {
        progress.close();
        message(error, MB_ICONERROR);
        return 1;
    }

    progress.close();

    auto const answer = MessageBoxW(nullptr,
        (L"Je dostupná nová verze SRTune " + latestText +
         L".\n\nAktuální verze: " + currentText +
         L"\n\nStáhnout aktualizaci?").c_str(),
        kTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
    if (answer != IDYES)
        return 0;

    auto const installer = tempFolder / manifest.name;
    std::error_code fileError;
    std::filesystem::remove(installer, fileError);

    ProgressWindow download(L"Stahuji aktualizaci SRTune…");
    auto const installerUrl =
        L"https://github.com/zafodtom/SRTune/releases/download/v" +
        latestText + L"/" + manifest.name;

    if (!downloadFile(installerUrl, installer,
            manifest.size, download, error) ||
        !verifySha256(installer, manifest.sha256))
    {
        download.close();
        std::filesystem::remove(installer, fileError);
        message(error.empty()
            ? L"Stažený instalátor neprošel kontrolou SHA-256."
            : error, MB_ICONERROR);
        return 1;
    }

    download.close();

    if (MessageBoxW(nullptr,
        (L"Aktualizace SRTune " + latestText +
         L" je stažená a ověřená.\n\n"
         L"SRTune se nyní zavře, aktualizuje a znovu spustí. Pokračovat?").c_str(),
        kTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1) != IDYES)
    {
        std::filesystem::remove(installer, fileError);
        return 0;
    }

    if (!waitForApplicationExit(parentPid))
    {
        message(
            L"SRTune se nepodařilo ukončit. Aktualizace nebyla spuštěna.",
            MB_ICONERROR);
        std::filesystem::remove(installer, fileError);
        return 1;
    }

    // Verify the exact file again immediately before Inno Setup is started.
    if (!verifySha256(installer, manifest.sha256))
    {
        message(
            L"Instalátor neprošel opakovanou kontrolou. Aktualizace nebyla spuštěna.",
            MB_ICONERROR);
        std::filesystem::remove(installer, fileError);
        DWORD ignored{};
        launch(appPath.wstring(), L"", false, ignored);
        return 1;
    }

    ProgressWindow installing(
        L"Instaluji aktualizaci… SRTune se poté znovu otevře.");

    auto const appDir = appPath.parent_path().wstring();
    std::wstring const args =
        L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART "
        L"/RESTARTEXITCODE=3010 /SP- /CLOSEAPPLICATIONS "
        L"/NORESTARTAPPLICATIONS /DIR=" + quote(appDir);

    DWORD installerCode = 1;
    auto const started = launch(
        installer.wstring(), args, true, installerCode);

    installing.close();
    std::filesystem::remove(installer, fileError);

    if (!started || (installerCode != 0 && installerCode != 3010))
    {
        message(
            L"Aktualizace se nezdařila. SRTune se znovu otevře.",
            MB_ICONERROR);
        DWORD ignored{};
        launch(appPath.wstring(), L"", false, ignored);
        return 1;
    }

    if (installerCode == 3010)
    {
        message(
            L"Aktualizace vyžaduje restart Windows. Po restartu spusťte SRTune.");
        return 0;
    }

    DWORD ignored{};
    if (!launch(appPath.wstring(), L"", false, ignored))
    {
        message(
            L"Aktualizace proběhla, ale SRTune se nepodařilo automaticky znovu spustit.\n"
            L"Spusťte jej prosím ze zástupce.");
    }

    return 0;
}
