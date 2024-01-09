#include "Strings.h"

#include <string.h>
#include <mbstring.h>

#include "win32/misc.h"

// Same as gIsEqual but case-insensitive.
bool gIsEqualNoCase(StringView inString1, StringView inString2)
{
	if (inString1.size() != inString2.size())
		return false;

	return _mbsnicmp((const unsigned char*)inString1.data(), (const unsigned char*)inString2.data(), inString1.size()) == 0;
}

// Same as gStartsWith but case-insensitive.
bool gStartsWithNoCase(StringView inString, StringView inStart)
{
	if (inString.size() < inStart.size())
		return false;

	return _mbsnicmp((const unsigned char*)inString.data(), (const unsigned char*)inStart.data(), inStart.size()) == 0;
}

// Same as gEndsWith but case-insensitive.
bool gEndsWithNoCase(StringView inString, StringView inEnd)
{
	if (inString.size() < inEnd.size())
		return false;

	return _mbsnicmp((const unsigned char*)inString.data() + inString.size() - inEnd.size(), (const unsigned char*)inEnd.data(), inEnd.size()) == 0;
}


// Convert wide char string to utf8. Always returns a null terminated string. Return an empty string on failure.
std::optional<StringView> gWideCharToUtf8(std::wstring_view inWString, MutStringView ioBuffer)
{
	// If a null terminator is included in the source, WideCharToMultiByte will also add it in the destination.
	// Otherwise we'll need to add it manually.
	bool source_is_null_terminated = (!inWString.empty() && inWString.back() == 0);

	int available_bytes = (int)ioBuffer.size();

	// If we need to add a null terminator, reserve 1 byte for it.
	if (source_is_null_terminated)
		available_bytes--;

	int written_bytes = WideCharToMultiByte(CP_UTF8, 0, inWString.data(), (int)inWString.size(), ioBuffer.data(), available_bytes, nullptr, nullptr);

	if (written_bytes == 0 && !inWString.empty())
		return std::nullopt; // Failed to convert.

	if (written_bytes == available_bytes)
		return std::nullopt; // Might be cropped, consider failed.

	// If there isn't a null terminator, add it.
	if (!source_is_null_terminated)
		ioBuffer[written_bytes] = 0;
	else
	{
		gAssert(ioBuffer[written_bytes - 1] == 0); // Should already have a null terminator.
		written_bytes--; // Don't count the null terminator in the returned string view.
	}

	return ioBuffer.subspan(0, written_bytes);
}

// Convert utf8 string to wide char. Always returns a null terminated string. Return an empty string on failure.
std::optional<std::wstring_view> gUtf8ToWideChar(StringView inString, std::span<wchar_t> ioBuffer)
{
	// If a null terminator is included in the source, WideCharToMultiByte will also add it in the destination.
	// Otherwise we'll need to add it manually.
	bool source_is_null_terminated = (!inString.empty() && inString.back() == 0);

	int available_wchars = (int)ioBuffer.size();

	// If we need to add a null terminator, reserve 1 byte for it.
	if (source_is_null_terminated)
		available_wchars--;

	int written_wchars = MultiByteToWideChar(CP_UTF8, 0, inString.data(), (int)inString.size(), ioBuffer.data(), available_wchars);

	if (written_wchars == 0 && !inString.empty())
		return std::nullopt; // Failed to convert.

	if (written_wchars == available_wchars)
		return std::nullopt; // Might be cropped, consider failed.

	// If there isn't a null terminator, add it.
	if (!source_is_null_terminated)
		ioBuffer[written_wchars] = 0;
	else
	{
		gAssert(ioBuffer[written_wchars - 1] == 0); // Should already have a null terminator.
		written_wchars--; // Don't count the null terminator in the returned string view.
	}

	return std::wstring_view{ ioBuffer.data(), (size_t)written_wchars };
}