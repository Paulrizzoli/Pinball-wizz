#pragma once

// Software painter for custom tables: draws the playfield, walls and parts in 3D with the game's camera,
// and draws flat panel art and lettering, so tables made in the editor look finished without any art files.

#include <functional>
#include <string>
#include <vector>
#include <cstdint>

struct ArtVec
{
	float X, Y, Z;
};

inline ArtVec operator+(const ArtVec& a, const ArtVec& b) { return {a.X + b.X, a.Y + b.Y, a.Z + b.Z}; }
inline ArtVec operator-(const ArtVec& a, const ArtVec& b) { return {a.X - b.X, a.Y - b.Y, a.Z - b.Z}; }
inline ArtVec operator*(const ArtVec& a, float s) { return {a.X * s, a.Y * s, a.Z * s}; }
float ArtDot(const ArtVec& a, const ArtVec& b);
ArtVec ArtCross(const ArtVec& a, const ArtVec& b);
ArtVec ArtNormalize(const ArtVec& a);

// Colour with straight floats, 0..1.
struct ArtColor
{
	float R, G, B;
	static ArtColor FromHex(const std::string& hex, ArtColor fallback);
	ArtColor Scaled(float s) const { return {R * s, G * s, B * s}; }
	ArtColor Mix(const ArtColor& o, float t) const { return {R + (o.R - R) * t, G + (o.G - G) * t, B + (o.B - B) * t}; }
};

struct ArtMaterial
{
	ArtColor Color{0.8f, 0.8f, 0.8f};
	float Ambient = 0.38f;
	float Diffuse = 0.75f;
	float Specular = 0.35f;
	float Shininess = 28.0f;
	float Glow = 0.0f; // adds the base colour unlit, for lamps and lit parts
};

// The custom table camera, in the same form as the game's projection (proj.cpp).
struct TableCamera
{
	float Matrix[12]{};
	float D = 0, CenterX = 0, CenterY = 0, ZMin = 0, ZScaler = 1;

	// Builds a camera tilted by tiltDeg that looks at the table centre from distance.
	void Setup(float tiltDeg, float distance, float focal, float centerX, float centerY);
	// Table point to 1x screen position and camera distance (the game's z_distance).
	void Project(const ArtVec& p, float& sx, float& sy, float& depth) const;
	void ToCamera(const ArtVec& p, ArtVec& cam) const;
	uint16_t NormalizeDepth(float depth) const;
};

// Pixels made by the painters, ready to become game bitmaps.
struct ArtImage
{
	int Width = 0, Height = 0;
	std::vector<uint32_t> Pixels; // ColorRgba values, 0 = transparent
	std::vector<uint16_t> Depth; // the game's zMap values, 0xFFFF = nothing here
};

// 3D painter with a depth buffer. Coordinates of the area are 1x screen pixels; it draws at `scale` (the HD scale)
// with supersampling, then hands out images at 1x and at the HD scale.
class Art3D
{
public:
	using Shader = std::function<ArtColor(const ArtVec& pos, const ArtVec& normal)>;

	Art3D(const TableCamera& camera, int x0, int y0, int width, int height, int scale);

	void Triangle(const ArtVec& a, const ArtVec& b, const ArtVec& c,
	              const ArtVec& na, const ArtVec& nb, const ArtVec& nc, const Shader& shader);
	void FlatTriangle(const ArtVec& a, const ArtVec& b, const ArtVec& c, const Shader& shader);
	void Quad(const ArtVec& a, const ArtVec& b, const ArtVec& c, const ArtVec& d, const Shader& shader);

	// Solid shapes.
	void Prism(const std::vector<ArtVec>& outline, float z0, float z1, const Shader& top, const Shader& side,
	           bool smoothSides = false);
	void Cylinder(float cx, float cy, float radius, float z0, float z1, const Shader& top, const Shader& side,
	              int segments = 0);
	void Cone(float cx, float cy, float r0, float r1, float z0, float z1, const Shader& side, int segments = 0);
	void Disc(float cx, float cy, float radius, float z, const Shader& top, int segments = 0);

	ArtImage Resolve(int factor) const; // factor = samples per output pixel side
	int SuperScale() const { return Scale * SS; }
	int X0, Y0, Width, Height, Scale, SS;

private:
	const TableCamera& Camera;
	std::vector<uint32_t> Color;
	std::vector<float> Depth;
	int BufW, BufH;
};

// Shading helper: lights a material at a point.
ArtColor ArtShade(const ArtMaterial& m, const ArtVec& pos, const ArtVec& normal);
Art3D::Shader ArtSolid(const ArtMaterial& m);

// Flat 2D painter for panels and lettering, with smooth edges. Coordinates are 1x pixels.
class Art2D
{
public:
	Art2D(int width, int height, int scale);
	void Clear(ArtColor c, float alpha = 1.0f);
	// Covers the whole canvas with an 8-bit RGB picture, stretched to fit (smoothly sampled).
	void Picture(const unsigned char* rgb, int width, int height);
	void FillRect(float x, float y, float w, float h, ArtColor c, float radius = 0, float alpha = 1.0f);
	void VerticalGradient(float x, float y, float w, float h, ArtColor top, ArtColor bottom, float radius = 0);
	void StrokeRect(float x, float y, float w, float h, float lineWidth, ArtColor c, float radius = 0,
	                float alpha = 1.0f);
	void Line(float x0, float y0, float x1, float y1, float width, ArtColor c, float alpha = 1.0f);
	void Circle(float cx, float cy, float r, ArtColor c, float alpha = 1.0f);
	void Glow(float cx, float cy, float r, ArtColor c, float strength);
	// Stroke lettering. size = capital height in pixels; align 0 left, 1 centre, 2 right. Returns the width.
	float Text(const std::string& text, float x, float y, float size, float weight, ArtColor c, int align = 0,
	           float alpha = 1.0f);
	static float TextWidth(const std::string& text, float size);
	ArtImage Resolve(int factor) const;

	int Width, Height, Scale;

private:
	std::vector<float> RGBA; // premultiplied
	void Blend(int px, int py, ArtColor c, float a);
};
