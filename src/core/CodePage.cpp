// Text in a Windows code page (EOP titles are UTF-8 or GBK) to UTF-8. Kept apart from the JUCE
// sources so <windows.h> never meets JUCE's own Windows declarations.

#include <string>

#ifdef _WIN32
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace fall::detail
{

std::string codePageToUtf8 (const std::string& input, unsigned codePage)
{
#ifdef _WIN32
    if (input.empty()) return {};
    const UINT cp = (UINT) codePage;
    const int wideLength = MultiByteToWideChar (cp, MB_ERR_INVALID_CHARS, input.data(), (int) input.size(), nullptr, 0);
    if (wideLength <= 0) return {};
    std::wstring wide ((size_t) wideLength, L'\0');
    if (! MultiByteToWideChar (cp, MB_ERR_INVALID_CHARS, input.data(), (int) input.size(), wide.data(), wideLength))
        return {};
    const int utf8Length = WideCharToMultiByte (CP_UTF8, 0, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) return {};
    std::string output ((size_t) utf8Length, '\0');
    if (! WideCharToMultiByte (CP_UTF8, 0, wide.data(), wideLength, output.data(), utf8Length, nullptr, nullptr))
        return {};
    return output;
#else
    (void) input; (void) codePage; // the app runs on Windows; elsewhere a GBK title falls back to the file name
    return {};
#endif
}

} // namespace fall::detail
