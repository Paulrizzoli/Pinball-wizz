#pragma once

// "Choose a Table" window: a card for Space Cadet and for each table in the Tables folder, with a sketch of
// the layout, the author and the best score. Picking one restarts the game on that table.

class tablepicker
{
public:
	static bool Open;
	static void Show();
	static void Render();
	// Opens the picker once, the first time the game starts with custom tables available.
	static void FirstStart();
};
