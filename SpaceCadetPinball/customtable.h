#pragma once

// Custom tables: original tables made for Pinball Wizz, stored as .pwtable files.
// A .pwtable file is either plain table text (JSON) or a zip with table.json and the table's images.
// Loading one builds the same records the engine reads from PINBALL.DAT, so the existing physics,
// parts and effects all work unchanged; the art is painted from the table description.

#include <string>
#include <vector>
#include <map>

class DatFile;
class TPinballComponent;

struct CustomTableEntry
{
	std::string Path;
	std::string Name;
	std::string Author;
};

class customtable
{
public:
	// True while a custom table is loaded (instead of Space Cadet).
	static bool Active;
	static std::string Name;
	static std::string Path;
	static int BallsPerGame;

	// Builds game records for the table at path. Returns nullptr and fills error on failure.
	static DatFile* Load(const std::string& path, std::string& error);
	// Copies the painted colours into the bitmaps; call after gdrv::display_palette.
	static void ApplyColors();
	// HD: registers the sharp HD versions of the painted bitmaps; call after hdgfx::BeginCustom.
	static void ApplyHd();
	static int HdScale() { return BuildScale; }
	static void Unload();

	// Scoring value of a part, 0 when it scores nothing.
	static int ScoreFor(const TPinballComponent* component);

	// Lists the .pwtable files in the Tables folders of the given base folders.
	static std::vector<CustomTableEntry> FindTables(const std::vector<std::string>& baseFolders);
	// Reads just the name and author of a table file.
	static bool ReadInfo(const std::string& path, CustomTableEntry& entry);

	// Folders (each ending with a path separator, or empty for the working folder) whose Tables folder is searched.
	static std::vector<std::string> SearchFolders;
	// Picks the table to load at start: the saved one if it still exists, otherwise the first table found when
	// there is no Space Cadet data. Returns the path, or an empty string for Space Cadet.
	static std::string ResolveStartTable(const std::string& saved, bool haveSpaceCadet);

	// Exposed for the table builder.
	static std::map<int, int> Scores;
	static int BuildScale;
};
