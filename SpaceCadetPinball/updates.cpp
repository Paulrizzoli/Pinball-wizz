#include "pch.h"
#include "updates.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace
{
	std::wstring Widen(const std::string& text)
	{
		auto length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
		std::wstring wide(length > 0 ? length : 1, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wide[0], length);
		wide.resize(wide.size() - 1);
		return wide;
	}
}
#endif

std::string updates::UpdaterPath()
{
	auto base = SDL_GetBasePath();
	std::string path = base ? base : "";
	SDL_free(base);
	return path + "Update Pinball Wizz.exe";
}

bool updates::Available()
{
#ifdef _WIN32
	static int available = -1;
	if (available < 0)
	{
		auto file = fopenu(UpdaterPath().c_str(), "rb");
		available = file != nullptr;
		if (file)
			fclose(file);
	}
	return available == 1;
#else
	return false;
#endif
}

bool updates::LaunchUpdater()
{
#ifdef _WIN32
	auto path = UpdaterPath();
	auto folder = path.substr(0, path.find_last_of("\\/"));
	auto exe = Widen(path);
	auto commandLine = L"\"" + exe + L"\" --from-game";
	auto dir = Widen(folder);

	STARTUPINFOW startup{};
	startup.cb = sizeof startup;
	PROCESS_INFORMATION process{};
	if (!CreateProcessW(exe.c_str(), &commandLine[0], nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
	                    dir.c_str(), &startup, &process))
		return false;
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return true;
#else
	return false;
#endif
}
