#pragma once

// Modern presentation effects drawn on top of the original renderer. Each one has its own switch in the
// Settings window (and Options > Graphics); none of them change game logic.
//
//  - Ball trail: a short fading streak behind a moving ball.
//  - Bumper sparks: a flash and a burst of sparks in the bumper's colour when a bumper lights up.
//  - Nudge shake: the whole table shakes briefly when nudged.
//  - Widescreen backglass: on wide windows, the empty side bars show the backglass art and an enlarged
//    score panel over a blurred, dimmed copy of the table.
//  - CRT filter: scanlines and a soft vignette for an arcade-monitor look.

struct SDL_Texture;
struct SDL_Rect;

class modernfx
{
public:
	// Bumper lit up at (x, y) in 1x vScreen pixels.
	static void BumperHit(float x, float y, float radius, const uint8_t color[3]);
	// Table nudged; dx/dy is the nudge direction in table units.
	static void Nudge(float dx, float dy);

	// Called at the start of PresentVScreen: advances effects and returns the screen shake offset.
	static void BeginFrame(int& shakeX, int& shakeY);
	// Widescreen layout: when the window is wider than the game, returns where the full vScreen would be
	// drawn so that the table alone fills the height, plus the free areas left and right of the table.
	static bool WideLayout(SDL_Rect& screenRect, SDL_Rect& left, SDL_Rect& right);
	// Wide layout only: moves a rect in 1x vScreen pixels from the sidebar to where the enlarged panels
	// show it on screen, and gives the scale used. Returns false when not in the wide layout.
	static bool MapPanelRect(SDL_Rect& rect, float& scale);
	// Blurred, dimmed copy of the table behind everything (widescreen only).
	static void DrawAmbient(SDL_Texture* screenTexture, bool wide);
	// Trail and sparks, drawn over the table (board transform).
	static void DrawEffects();
	// Backglass (left) and enlarged score panel with its dot-matrix boxes (right).
	static void DrawPanels(SDL_Texture* screenTexture, int hdScale, const SDL_Rect& screenRect, const SDL_Rect& left,
	                       const SDL_Rect& right);
	// CRT filter over the whole window, drawn last.
	static void DrawCrt();

	// Settings window with every graphics switch.
	static bool SettingsOpen;
	static void RenderSettingsWindow();
	// "Settings" button: big, in the left panel, in the wide layout; otherwise on the table while no game is running.
	static void RenderTitleButton();

	static void Uninit();

private:
	struct Particle
	{
		float X, Y, VX, VY, Life, MaxLife, Size;
		uint8_t Color[3];
	};

	static std::vector<Particle> Particles;
	static std::unordered_map<const void*, std::vector<float>> Trails; // x, y, depth triples, newest first
	static float ShakeTime, ShakeDirX, ShakeDirY;
	static uint64_t LastTicks;
	static float FrameDt;
	static SDL_Texture *Dot, *Ambient, *Scanlines, *Vignette;
	static int ScanlinesHeight;

	static void PanelRegions(SDL_Rect& logo, SDL_Rect& scores);
	// Where the backglass and score panel are drawn in the wide layout.
	static void PanelRects(const SDL_Rect& screenRect, const SDL_Rect& left, const SDL_Rect& right, SDL_Rect dst[2]);
	static void FitRect(const SDL_Rect& src, const SDL_Rect& area, float maxScale, SDL_Rect& dst);
	static SDL_Texture* GetDot();
};
