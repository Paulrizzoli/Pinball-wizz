#pragma once

// LED lighting.
//
// The original table lights switch instantly between two pre-drawn bitmaps (lamp off / lamp on).
// This module makes them look like modern LED inserts:
//  - Smooth fades: when a light changes bitmap, it cross-fades over a few frames instead of snapping.
//  - LED colours: the lit parts of light bitmaps get more saturated, brighter colours.
//  - Glow: lit lamps get a soft additive bloom drawn over the table when the frame is presented.
//
// Game logic is untouched; this only changes how the light sprites are painted.
// Everything is built at runtime from the player's own table bitmaps, so no art ships with the game.

struct render_sprite;
struct gdrv_bitmap8;
struct zmap_header_type;
struct SDL_Texture;

class ledfx
{
public:
	struct RefBitmap
	{
		gdrv_bitmap8* Bmp;
		int X, Y;
	};

	// Marks a sprite as a light (called by TLight and TBumper). With dimWhenOff, the lamp art printed on
	// the table under the light is darkened while the light is off, like an unlit LED insert.
	static void Register(render_sprite* sprite, bool dimWhenOff);
	// Bumpers also get a flash and sparks (motion effects) when they light up.
	static void MarkBumper(render_sprite* sprite);

	// By default the table art under a lamp is its unlit look. Parts with their own unlit bitmap
	// (bumpers) name it here, so only the difference counts as lamp light.
	static void SetUnlit(gdrv_bitmap8* bmp, RefBitmap unlit);
	static void Unregister(render_sprite* sprite);

	static bool Enabled();
	static bool GlowEnabled();

	// Called by render_sprite::set before a light switches bitmap; starts the fade.
	static void SpriteChanged(render_sprite& sprite, gdrv_bitmap8* bmp, zmap_header_type* zMap, int xPos, int yPos);

	// Advances fades; called once per game update before the renderer repaints dirty sprites.
	static void Update();

	// LED-coloured copy of a light bitmap (with a matching HD twin when an HD pack is loaded).
	static gdrv_bitmap8* LedBitmap(gdrv_bitmap8* bmp, int xPos, int yPos);

	// Dimmed lamp drawn while a dimWhenOff light shows nothing, or nullptr.
	static gdrv_bitmap8* OffBitmap(const render_sprite& sprite);

	// Fade curve for a 0..1 fade position.
	static float Ease(float t);

	// Draws the glow of lit lamps on top of the presented table.
	static void DrawGlow();

	// Score and message boxes drawn as a dot-matrix display of little round LEDs (1x vScreen rectangle).
	static void AddDisplay(int x, int y, int width, int height);
	static void DrawDisplays();
	// Same, with a custom 1x-to-window transform, for the displays inside region (1x vScreen pixels).
	static void DrawDisplays(float originX, float originY, float scaleX, float scaleY, const struct SDL_Rect& region);

	// Positions of the dot-matrix boxes (1x vScreen pixels).
	static std::vector<SDL_Rect> DisplayRects();

	// Lamp colour of a light bitmap (average lit colour); false when it has no lamp light.
	static bool LampColor(gdrv_bitmap8* bmp, int xPos, int yPos, uint8_t color[3]);

	// Lit lamps (and lit bumpers) right now: centre in 1x vScreen pixels, strength 0..1 and colour.
	struct LitLamp
	{
		float X, Y, Strength;
		uint8_t Color[3];
	};
	static void LitLamps(std::vector<LitLamp>& out);

	// Repaints all lights after the LED option changes.
	static void SettingsChanged();

	static void Uninit();

private:
	struct BmpInfo
	{
		gdrv_bitmap8* Led = nullptr;
		SDL_Texture* Glow = nullptr;
		SDL_Texture* Spill = nullptr;
		float Emission = 0;
		float CenterX = 0, CenterY = 0; // light-weighted centre, bitmap-local 1x pixels
		uint8_t Color[3]{};             // average lamp colour
		bool Ready = false;
	};

	static std::vector<render_sprite*> Lights;
	static std::vector<render_sprite*> Bumpers;
	static std::unordered_map<const gdrv_bitmap8*, BmpInfo> Cache;
	static std::unordered_map<const gdrv_bitmap8*, RefBitmap> Refs;
	static std::unordered_map<const gdrv_bitmap8*, gdrv_bitmap8*> OffCache;
	static uint64_t LastTicks;

	struct Display
	{
		int X, Y, Width, Height;
		std::vector<uint32_t> Pixels;
		SDL_Texture* Dots = nullptr;
		SDL_Texture* Glow = nullptr;
	};
	static std::vector<Display> Displays;
	static void RebuildDisplay(Display& display);

	static BmpInfo& Info(gdrv_bitmap8* bmp, int xPos, int yPos);
	static float Emission(gdrv_bitmap8* bmp, int xPos, int yPos);
	static SDL_Texture* GlintTexture;
	static void DrawGlowLayer(gdrv_bitmap8* bmp, int xPos, int yPos, float weight, float originX, float originY,
	                          float scaleX, float scaleY, bool spill);
	static void DrawGlints(float originX, float originY, float scaleX, float scaleY);
};
