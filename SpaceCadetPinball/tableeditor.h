#pragma once

// Table editor: a top-down view of a custom table where parts can be added, moved and changed, then saved
// and played. Opened from the Table menu.

#include <string>

class tableeditor
{
public:
	static bool Open;
	// Opens the editor on a table file. Returns false (and shows why) when the file can't be edited.
	static bool Edit(const std::string& path);
	// Makes a new table from a simple starter layout in the Tables folder and opens it.
	static void NewTable();
	static void Render();
};
