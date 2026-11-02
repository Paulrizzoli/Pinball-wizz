#pragma once

// Custom tables: original tables made for Pinball Wizz, stored as .pwtable files.
// A .pwtable file is either plain table text (JSON) or a zip with table.json and the table's images.
// Loading one builds the same records the engine reads from PINBALL.DAT, so the existing physics,
// parts and effects all work unchanged; the art is painted from the table description.

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace minijson { class Value; }

class DatFile;
class TPinballComponent;

// A flat top-down sketch of a table, for the table picker. Coordinates are table units, origin at the centre.
struct TablePreviewShape
{
	enum Kinds { Line, Circle, Polygon };
	Kinds Kind;
	std::vector<float> Points; // x, y pairs; Circle uses the first pair as its centre
	float Radius = 0;
	float Width = 0; // line width in table units
	uint32_t Color = 0; // 0xAABBGGRR, like ImGui's IM_COL32
	bool Filled = false;
	int PartIndex = -1; // index in the table's "parts" list
};

struct TablePreview
{
	float Width = 16, Length = 29;
	uint32_t Playfield = 0, Playfield2 = 0, Accent = 0;
	std::vector<TablePreviewShape> Shapes;
};

struct CustomTableEntry
{
	std::string Path;
	std::string Name;
	std::string Author;
	TablePreview Preview;
};

// Rules from the table file, with parts and lights as their record (group) numbers.
struct CustomRuleGroup
{
	std::string Name;
	std::vector<int> Parts;
	std::vector<int> Lights; // Lights[i] lights up when Parts[i] is done (or as hits add up)
	int Hits = 0; // 0: hit every part once; otherwise this many hits on any of the parts
	int Score = 5000;
	int Bonus = 0;
	std::string Award; // none, multiplier, extraBall
	std::string Message;
};

struct CustomRules
{
	std::vector<CustomRuleGroup> Groups;
	int ExtraBallLight = -1;
	std::vector<int> MultiplierLights;
	float BallSave = 0; // seconds after a ball starts playing in which a drained ball is given back
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
	// Flat sketch of a table description, for the picker and the editor.
	static void BuildPreview(const minijson::Value& doc, TablePreview& out);
	// Reads a table file's description (plain or zipped). packed is true for a zipped table.
	static bool ReadDocument(const std::string& path, minijson::Value& doc, bool& packed, std::string& error);
	// Reads just the name and author of a table file.
	static bool ReadInfo(const std::string& path, CustomTableEntry& entry);

	// Folders (each ending with a path separator, or empty for the working folder) whose Tables folder is searched.
	static std::vector<std::string> SearchFolders;
	// Picks the table to load at start: the saved one if it still exists, otherwise the first table found when
	// there is no Space Cadet data. Returns the path, or an empty string for Space Cadet.
	static std::string ResolveStartTable(const std::string& saved, bool haveSpaceCadet);

	// Exposed for the table builder.
	static std::map<int, int> Scores;
	static std::map<int, int> Bonus; // end-of-ball bonus a part adds when hit
	static CustomRules Rules;
	static int BuildScale;
};
