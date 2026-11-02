#pragma once

// One-click updates: "Update Pinball Wizz.exe" ships next to the game. The game only starts it and
// closes, so the updater can replace the program files; the updater starts the game again when done.
class updates
{
public:
	// True when the updater is next to the game (Windows builds only).
	static bool Available();

	// Starts the updater. On success the game should quit right away.
	static bool LaunchUpdater();

private:
	static std::string UpdaterPath();
};
