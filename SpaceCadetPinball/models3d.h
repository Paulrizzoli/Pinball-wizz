#pragma once

// Real 3D models drawn over the table.
//
// The table art is a picture taken with the game's own camera (proj.cpp). This module builds real 3D models of the
// moving and shiny parts (the ball, the flippers, bumpers, posts and ramp rails), places them at their physics
// positions in table space and draws them with that same camera, so they line up exactly with the art.
//
//  - Each model is rendered in software at the screen's detail level, with per-pixel lighting: a key light,
//    a fill light and the table's lit lamps (their colour falls on nearby parts).
//  - Reflections: the chrome ball (and, softly, the plastics) reflect the live table under them, lamps included,
//    and the room above the cabinet.
//  - Shadows: every model casts a soft shadow away from the key light onto the table.
//  - Depth: models are tested against the table's depth buffer, so ramps, rails and wire forms in front of them
//    still hide them, exactly as the original sprites were hidden.
//
// Physics is untouched: models only read the parts' positions. The original sprites of modelled parts are hidden.

#include <cstdint>
#include <vector>

struct SDL_Texture;

class models3d
{
public:
	// The 3D models switch is on and a table is loaded.
	static bool Active();
	// The ball is drawn as a 3D model (so the renderer skips the ball sprite and its old shadow).
	static bool DrawsBalls();
	// Draws all models over the presented table; call right after the table is blitted.
	static void Draw();
	static void Uninit();

	// Custom tables describe their parts so the models match the painted table (filled by the table builder).
	struct Color3
	{
		float R, G, B;
	};

	struct PostModel
	{
		float X, Y, Radius;
		Color3 Rubber, Metal;
	};

	struct BumperModel
	{
		float X, Y, Radius;
		Color3 Body, Cap, Metal;
	};

	struct RailModel
	{
		std::vector<float> Points; // x, y pairs along one rail
		float Z0, Z1;              // height of the rail tube's bottom and top
		Color3 Metal;
	};

	struct TableLook
	{
		bool Custom = false;
		Color3 FlipperBody{0.95f, 0.95f, 0.97f}, FlipperRubber{0.8f, 0.1f, 0.1f}, Metal{0.78f, 0.8f, 0.86f};
		std::vector<PostModel> Posts;
		std::vector<BumperModel> Bumpers;
		std::vector<RailModel> Rails;
	};

	static TableLook Look;
	static void ResetLook() { Look = TableLook(); }
};
