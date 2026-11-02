#include "pch.h"
#include <cctype>
#include "tableart.h"

#include "gdrv.h"

#include <limits>

float ArtDot(const ArtVec& a, const ArtVec& b)
{
	return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}

ArtVec ArtCross(const ArtVec& a, const ArtVec& b)
{
	return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}

ArtVec ArtNormalize(const ArtVec& a)
{
	auto len = std::sqrt(ArtDot(a, a));
	if (len < 1e-9f)
		return {0, 0, 1};
	return a * (1.0f / len);
}

void Art2D::Picture(const unsigned char* rgb, int width, int height)
{
	if (!rgb || width <= 0 || height <= 0)
		return;
	auto w = Width * Scale, h = Height * Scale;
	for (int y = 0; y < h; y++)
	{
		auto fy = std::max(0.0f, (y + 0.5f) * height / h - 0.5f);
		auto y0 = std::min(height - 1, static_cast<int>(fy)), y1 = std::min(height - 1, y0 + 1);
		auto ty = fy - y0;
		for (int x = 0; x < w; x++)
		{
			auto fx = std::max(0.0f, (x + 0.5f) * width / w - 0.5f);
			auto x0 = std::min(width - 1, static_cast<int>(fx)), x1 = std::min(width - 1, x0 + 1);
			auto tx = fx - x0;
			auto p = &RGBA[(static_cast<size_t>(y) * w + x) * 4];
			for (int c = 0; c < 3; c++)
			{
				auto a = rgb[(static_cast<size_t>(y0) * width + x0) * 3 + c] * (1 - tx) +
					rgb[(static_cast<size_t>(y0) * width + x1) * 3 + c] * tx;
				auto b = rgb[(static_cast<size_t>(y1) * width + x0) * 3 + c] * (1 - tx) +
					rgb[(static_cast<size_t>(y1) * width + x1) * 3 + c] * tx;
				p[c] = (a * (1 - ty) + b * ty) / 255.0f;
			}
			p[3] = 1.0f;
		}
	}
}

namespace
{
	float Clamp01(float v)
	{
		return v < 0 ? 0 : (v > 1 ? 1 : v);
	}

	uint32_t Pack(const ArtColor& c)
	{
		auto r = static_cast<uint8_t>(Clamp01(c.R) * 255.0f + 0.5f);
		auto g = static_cast<uint8_t>(Clamp01(c.G) * 255.0f + 0.5f);
		auto b = static_cast<uint8_t>(Clamp01(c.B) * 255.0f + 0.5f);
		return ColorRgba{r, g, b, 255}.Color;
	}

	void Unpack(uint32_t value, float& r, float& g, float& b)
	{
		ColorRgba c{value};
		r = c.GetRed() / 255.0f;
		g = c.GetGreen() / 255.0f;
		b = c.GetBlue() / 255.0f;
	}

	int HexValue(char c)
	{
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	}

	// Light comes from the upper left, above the table; the viewer sits in front of the table.
	const ArtVec LightDir = ArtNormalize({-0.35f, -0.55f, 0.85f});
	const ArtVec FillDir = ArtNormalize({0.6f, 0.3f, 0.5f});
	const ArtVec ViewDir = ArtNormalize({0.0f, 0.45f, 0.9f});
}

ArtColor ArtColor::FromHex(const std::string& hex, ArtColor fallback)
{
	std::string h = hex;
	if (!h.empty() && h[0] == '#')
		h = h.substr(1);
	if (h.size() != 6)
		return fallback;
	int v[6];
	for (int i = 0; i < 6; i++)
	{
		v[i] = HexValue(h[i]);
		if (v[i] < 0)
			return fallback;
	}
	return {(v[0] * 16 + v[1]) / 255.0f, (v[2] * 16 + v[3]) / 255.0f, (v[4] * 16 + v[5]) / 255.0f};
}

void TableCamera::Setup(float tiltDeg, float distance, float focal, float centerX, float centerY)
{
	auto t = tiltDeg * 3.14159265f / 180.0f;
	auto c = std::cos(t), s = std::sin(t);
	// Rows of the 3x4 view matrix: x is negated because the focal length is negative, as in the game's own camera.
	float m[12] =
	{
		-1, 0, 0, 0,
		0, -c, s, 0,
		0, -s, -c, distance
	};
	std::memcpy(Matrix, m, sizeof m);
	D = -focal;
	CenterX = centerX;
	CenterY = centerY;
	// Depth range covers everything from the front of the cabinet to the far end of the table.
	ZMin = distance - 18.0f;
	ZScaler = 65535.0f / 36.0f;
}

void TableCamera::ToCamera(const ArtVec& p, ArtVec& cam) const
{
	cam.X = Matrix[0] * p.X + Matrix[1] * p.Y + Matrix[2] * p.Z + Matrix[3];
	cam.Y = Matrix[4] * p.X + Matrix[5] * p.Y + Matrix[6] * p.Z + Matrix[7];
	cam.Z = Matrix[8] * p.X + Matrix[9] * p.Y + Matrix[10] * p.Z + Matrix[11];
}

void TableCamera::Project(const ArtVec& p, float& sx, float& sy, float& depth) const
{
	ArtVec cam;
	ToCamera(p, cam);
	auto coef = D / cam.Z;
	sx = cam.X * coef + CenterX;
	sy = cam.Y * coef + CenterY;
	depth = std::sqrt(ArtDot(cam, cam));
}

uint16_t TableCamera::NormalizeDepth(float depth) const
{
	if (depth < ZMin)
		return 0;
	auto scaled = (depth - ZMin) * ZScaler;
	if (scaled >= 65534.0f)
		return 65534;
	return static_cast<uint16_t>(scaled);
}

ArtColor ArtShade(const ArtMaterial& m, const ArtVec& pos, const ArtVec& normal)
{
	auto n = normal;
	if (ArtDot(n, ViewDir) < 0)
		n = n * -1.0f;
	auto diff = std::max(0.0f, ArtDot(n, LightDir));
	auto fill = std::max(0.0f, ArtDot(n, FillDir)) * 0.25f;
	auto half = ArtNormalize(LightDir + ViewDir);
	auto spec = std::pow(std::max(0.0f, ArtDot(n, half)), m.Shininess) * m.Specular;
	auto k = m.Ambient + m.Diffuse * diff + fill;
	return {
		m.Color.R * (k + m.Glow) + spec,
		m.Color.G * (k + m.Glow) + spec,
		m.Color.B * (k + m.Glow) + spec
	};
}

Art3D::Shader ArtSolid(const ArtMaterial& m)
{
	return [m](const ArtVec& pos, const ArtVec& normal) { return ArtShade(m, pos, normal); };
}

Art3D::Art3D(const TableCamera& camera, int x0, int y0, int width, int height, int scale)
	: X0(x0), Y0(y0), Width(width), Height(height), Scale(std::max(1, scale)), Camera(camera)
{
	// Supersampling smooths edges; at high HD scales the pixels are already small enough.
	SS = Scale >= 3 ? 1 : (Scale == 2 ? 2 : 3);
	BufW = Width * Scale * SS;
	BufH = Height * Scale * SS;
	Color.assign(static_cast<size_t>(BufW) * BufH, 0);
	Depth.assign(static_cast<size_t>(BufW) * BufH, std::numeric_limits<float>::infinity());
}

void Art3D::Triangle(const ArtVec& a, const ArtVec& b, const ArtVec& c,
                     const ArtVec& na, const ArtVec& nb, const ArtVec& nc, const Shader& shader)
{
	const ArtVec* pts[3] = {&a, &b, &c};
	const ArtVec* nrm[3] = {&na, &nb, &nc};
	float sx[3], sy[3], invZ[3];
	ArtVec cam[3];
	auto s = static_cast<float>(SuperScale());
	for (int i = 0; i < 3; i++)
	{
		Camera.ToCamera(*pts[i], cam[i]);
		if (cam[i].Z <= 0.01f)
			return;
		invZ[i] = 1.0f / cam[i].Z;
		sx[i] = (cam[i].X * Camera.D * invZ[i] + Camera.CenterX - X0) * s;
		sy[i] = (cam[i].Y * Camera.D * invZ[i] + Camera.CenterY - Y0) * s;
	}

	auto area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
	if (std::fabs(area) < 1e-6f)
		return;

	auto minX = std::max(0, static_cast<int>(std::floor(std::min({sx[0], sx[1], sx[2]}))));
	auto maxX = std::min(BufW - 1, static_cast<int>(std::ceil(std::max({sx[0], sx[1], sx[2]}))));
	auto minY = std::max(0, static_cast<int>(std::floor(std::min({sy[0], sy[1], sy[2]}))));
	auto maxY = std::min(BufH - 1, static_cast<int>(std::ceil(std::max({sy[0], sy[1], sy[2]}))));
	if (minX > maxX || minY > maxY)
		return;

	auto invArea = 1.0f / area;
	for (int y = minY; y <= maxY; y++)
	{
		auto py = y + 0.5f;
		for (int x = minX; x <= maxX; x++)
		{
			auto px = x + 0.5f;
			auto w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) * invArea;
			auto w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) * invArea;
			auto w2 = 1.0f - w0 - w1;
			if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f)
				continue;

			// Perspective-correct interpolation.
			auto w = w0 * invZ[0] + w1 * invZ[1] + w2 * invZ[2];
			auto cz = 1.0f / w;
			auto k0 = w0 * invZ[0] * cz, k1 = w1 * invZ[1] * cz, k2 = w2 * invZ[2] * cz;
			ArtVec camP = cam[0] * k0 + cam[1] * k1 + cam[2] * k2;
			auto depth = std::sqrt(ArtDot(camP, camP));
			auto idx = static_cast<size_t>(y) * BufW + x;
			if (depth >= Depth[idx])
				continue;

			ArtVec pos = *pts[0] * k0 + *pts[1] * k1 + *pts[2] * k2;
			ArtVec n = ArtNormalize(*nrm[0] * k0 + *nrm[1] * k1 + *nrm[2] * k2);
			Depth[idx] = depth;
			Color[idx] = Pack(shader(pos, n));
		}
	}
}

void Art3D::FlatTriangle(const ArtVec& a, const ArtVec& b, const ArtVec& c, const Shader& shader)
{
	auto n = ArtNormalize(ArtCross(b - a, c - a));
	if (n.Z < 0)
		n = n * -1.0f;
	Triangle(a, b, c, n, n, n, shader);
}

void Art3D::Quad(const ArtVec& a, const ArtVec& b, const ArtVec& c, const ArtVec& d, const Shader& shader)
{
	auto n = ArtNormalize(ArtCross(b - a, d - a));
	Triangle(a, b, c, n, n, n, shader);
	Triangle(a, c, d, n, n, n, shader);
}

namespace
{
	float SignedArea(const std::vector<ArtVec>& poly)
	{
		float area = 0;
		for (size_t i = 0; i < poly.size(); i++)
		{
			auto& p = poly[i];
			auto& q = poly[(i + 1) % poly.size()];
			area += p.X * q.Y - q.X * p.Y;
		}
		return area * 0.5f;
	}

	bool PointInTriangle(const ArtVec& p, const ArtVec& a, const ArtVec& b, const ArtVec& c)
	{
		auto d1 = (p.X - b.X) * (a.Y - b.Y) - (a.X - b.X) * (p.Y - b.Y);
		auto d2 = (p.X - c.X) * (b.Y - c.Y) - (b.X - c.X) * (p.Y - c.Y);
		auto d3 = (p.X - a.X) * (c.Y - a.Y) - (c.X - a.X) * (p.Y - a.Y);
		bool neg = d1 < 0 || d2 < 0 || d3 < 0;
		bool pos = d1 > 0 || d2 > 0 || d3 > 0;
		return !(neg && pos);
	}

	// Ear clipping; returns index triples.
	std::vector<int> Triangulate(const std::vector<ArtVec>& poly)
	{
		std::vector<int> result;
		std::vector<int> idx;
		for (int i = 0; i < static_cast<int>(poly.size()); i++)
			idx.push_back(i);
		if (SignedArea(poly) < 0)
			std::reverse(idx.begin(), idx.end());

		int guard = 0;
		while (idx.size() > 3 && guard++ < 10000)
		{
			bool clipped = false;
			for (size_t i = 0; i < idx.size(); i++)
			{
				auto i0 = idx[(i + idx.size() - 1) % idx.size()], i1 = idx[i], i2 = idx[(i + 1) % idx.size()];
				auto& a = poly[i0];
				auto& b = poly[i1];
				auto& c = poly[i2];
				auto cross = (b.X - a.X) * (c.Y - a.Y) - (b.Y - a.Y) * (c.X - a.X);
				if (cross <= 1e-7f)
					continue;
				bool inside = false;
				for (auto j : idx)
				{
					if (j == i0 || j == i1 || j == i2)
						continue;
					if (PointInTriangle(poly[j], a, b, c))
					{
						inside = true;
						break;
					}
				}
				if (inside)
					continue;
				result.push_back(i0);
				result.push_back(i1);
				result.push_back(i2);
				idx.erase(idx.begin() + i);
				clipped = true;
				break;
			}
			if (!clipped)
			{
				// Degenerate outline: fall back to a fan so something is drawn.
				for (size_t i = 1; i + 1 < idx.size(); i++)
				{
					result.push_back(idx[0]);
					result.push_back(idx[i]);
					result.push_back(idx[i + 1]);
				}
				return result;
			}
		}
		if (idx.size() == 3)
		{
			result.push_back(idx[0]);
			result.push_back(idx[1]);
			result.push_back(idx[2]);
		}
		return result;
	}
}

void Art3D::Prism(const std::vector<ArtVec>& outline, float z0, float z1, const Shader& top, const Shader& side,
                  bool smoothSides)
{
	auto count = outline.size();
	if (count < 3)
		return;

	// Top face.
	std::vector<ArtVec> topPts;
	for (auto& p : outline)
		topPts.push_back({p.X, p.Y, z1});
	auto tris = Triangulate(topPts);
	ArtVec up{0, 0, 1};
	for (size_t i = 0; i + 2 < tris.size(); i += 3)
		Triangle(topPts[tris[i]], topPts[tris[i + 1]], topPts[tris[i + 2]], up, up, up, top);

	// Sides, with normals pointing out of the shape.
	auto sign = SignedArea(outline) > 0 ? 1.0f : -1.0f;
	std::vector<ArtVec> edgeNormals(count);
	for (size_t i = 0; i < count; i++)
	{
		auto& p = outline[i];
		auto& q = outline[(i + 1) % count];
		edgeNormals[i] = ArtNormalize(ArtVec{(q.Y - p.Y) * sign, -(q.X - p.X) * sign, 0});
	}
	for (size_t i = 0; i < count; i++)
	{
		auto& p = outline[i];
		auto& q = outline[(i + 1) % count];
		ArtVec n0 = edgeNormals[i], n1 = edgeNormals[i];
		if (smoothSides)
		{
			n0 = ArtNormalize(edgeNormals[i] + edgeNormals[(i + count - 1) % count]);
			n1 = ArtNormalize(edgeNormals[i] + edgeNormals[(i + 1) % count]);
		}
		ArtVec a{p.X, p.Y, z0}, b{q.X, q.Y, z0}, c{q.X, q.Y, z1}, d{p.X, p.Y, z1};
		Triangle(a, b, c, n0, n1, n1, side);
		Triangle(a, c, d, n0, n1, n0, side);
	}
}

void Art3D::Cylinder(float cx, float cy, float radius, float z0, float z1, const Shader& top, const Shader& side,
                     int segments)
{
	if (segments <= 0)
		segments = 48;
	std::vector<ArtVec> outline;
	for (int i = 0; i < segments; i++)
	{
		auto a = 2.0f * 3.14159265f * i / segments;
		outline.push_back({cx + std::cos(a) * radius, cy + std::sin(a) * radius, 0});
	}
	Prism(outline, z0, z1, top, side, true);
}

void Art3D::Cone(float cx, float cy, float r0, float r1, float z0, float z1, const Shader& side, int segments)
{
	if (segments <= 0)
		segments = 48;
	auto slope = (r0 - r1) / std::max(0.001f, z1 - z0);
	for (int i = 0; i < segments; i++)
	{
		auto a0 = 2.0f * 3.14159265f * i / segments;
		auto a1 = 2.0f * 3.14159265f * (i + 1) / segments;
		ArtVec d0{std::cos(a0), std::sin(a0), 0}, d1{std::cos(a1), std::sin(a1), 0};
		ArtVec n0 = ArtNormalize({d0.X, d0.Y, slope}), n1 = ArtNormalize({d1.X, d1.Y, slope});
		ArtVec p0{cx + d0.X * r0, cy + d0.Y * r0, z0}, p1{cx + d1.X * r0, cy + d1.Y * r0, z0};
		ArtVec q0{cx + d0.X * r1, cy + d0.Y * r1, z1}, q1{cx + d1.X * r1, cy + d1.Y * r1, z1};
		Triangle(p0, p1, q1, n0, n1, n1, side);
		Triangle(p0, q1, q0, n0, n1, n0, side);
	}
}

void Art3D::Disc(float cx, float cy, float radius, float z, const Shader& top, int segments)
{
	if (segments <= 0)
		segments = 48;
	ArtVec up{0, 0, 1}, center{cx, cy, z};
	for (int i = 0; i < segments; i++)
	{
		auto a0 = 2.0f * 3.14159265f * i / segments;
		auto a1 = 2.0f * 3.14159265f * (i + 1) / segments;
		ArtVec p0{cx + std::cos(a0) * radius, cy + std::sin(a0) * radius, z};
		ArtVec p1{cx + std::cos(a1) * radius, cy + std::sin(a1) * radius, z};
		Triangle(center, p0, p1, up, up, up, top);
	}
}

ArtImage Art3D::Resolve(int factor) const
{
	ArtImage img;
	img.Width = BufW / factor;
	img.Height = BufH / factor;
	img.Pixels.assign(static_cast<size_t>(img.Width) * img.Height, 0);
	img.Depth.assign(static_cast<size_t>(img.Width) * img.Height, 0xFFFF);
	auto total = factor * factor;
	for (int y = 0; y < img.Height; y++)
	{
		for (int x = 0; x < img.Width; x++)
		{
			float r = 0, g = 0, b = 0, minDepth = std::numeric_limits<float>::infinity();
			int covered = 0;
			for (int sy = 0; sy < factor; sy++)
			{
				for (int sx = 0; sx < factor; sx++)
				{
					auto idx = static_cast<size_t>(y * factor + sy) * BufW + (x * factor + sx);
					if (Depth[idx] == std::numeric_limits<float>::infinity())
						continue;
					float cr, cg, cb;
					Unpack(Color[idx], cr, cg, cb);
					r += cr;
					g += cg;
					b += cb;
					minDepth = std::min(minDepth, Depth[idx]);
					covered++;
				}
			}
			if (covered * 2 < total)
				continue;
			auto out = static_cast<size_t>(y) * img.Width + x;
			img.Pixels[out] = Pack({r / covered, g / covered, b / covered});
			img.Depth[out] = Camera.NormalizeDepth(minDepth);
		}
	}
	return img;
}

// ---------------------------------------------------------------------------------------------
// 2D painter

namespace
{
	// Stroke lettering on a 4 x 6 grid; each glyph is a list of polylines of "xy" points.
	const char* GlyphFor(char ch)
	{
		switch (ch)
		{
		case 'A': return "06 01 10 30 41 46|03 43";
		case 'B': return "06 00 30 41 42 33 03|33 44 45 36 06";
		case 'C': return "41 30 10 01 05 16 36 45";
		case 'D': return "00 30 41 45 36 06 00";
		case 'E': return "40 00 06 46|03 33";
		case 'F': return "40 00 06|03 33";
		case 'G': return "41 30 10 01 05 16 36 45 43 23";
		case 'H': return "00 06|40 46|03 43";
		case 'I': return "10 30|20 26|16 36";
		case 'J': return "40 45 36 16 05";
		case 'K': return "00 06|40 03 46";
		case 'L': return "00 06 46";
		case 'M': return "06 00 23 40 46";
		case 'N': return "06 00 46 40";
		case 'O': return "10 30 41 45 36 16 05 01 10";
		case 'P': return "06 00 30 41 42 33 03";
		case 'Q': return "10 30 41 45 36 16 05 01 10|24 46";
		case 'R': return "06 00 30 41 42 33 03|23 46";
		case 'S': return "41 30 10 01 02 13 33 44 45 36 16 05";
		case 'T': return "00 40|20 26";
		case 'U': return "00 05 16 36 45 40";
		case 'V': return "00 26 40";
		case 'W': return "00 16 23 36 40";
		case 'X': return "00 46|40 06";
		case 'Y': return "00 23 40|23 26";
		case 'Z': return "00 40 06 46";
		case '0': return "10 30 41 45 36 16 05 01 10";
		case '1': return "11 20 26|16 36";
		case '2': return "01 10 30 41 42 06 46";
		case '3': return "01 10 30 41 42 33 13|33 44 45 36 16 05";
		case '4': return "36 30 03 43";
		case '5': return "40 00 03 33 44 45 36 16 05";
		case '6': return "41 30 10 01 05 16 36 45 44 33 03";
		case '7': return "00 40 26";
		case '8': return "13 02 01 10 30 41 42 33 13 04 05 16 36 45 44 33";
		case '9': return "05 16 36 45 41 30 10 01 02 13 43";
		case '-': return "03 43";
		case '+': return "03 43|21 25";
		case '.': return "26 26";
		case ',': return "26 17";
		case ':': return "22 22|25 25";
		case '!': return "20 24|26 26";
		case '?': return "01 10 30 41 42 23 24|26 26";
		case '/': return "06 40";
		case '\'': return "20 21";
		case '&': return "46 01 10 20 31 22 05 16 26 44";
		default: return "";
		}
	}

	const float GlyphAdvance = 5.6f; // grid units per letter, including the gap
}

Art2D::Art2D(int width, int height, int scale) : Width(width), Height(height), Scale(std::max(1, scale))
{
	RGBA.assign(static_cast<size_t>(Width * Scale) * (Height * Scale) * 4, 0.0f);
}

void Art2D::Blend(int px, int py, ArtColor c, float a)
{
	if (a <= 0 || px < 0 || py < 0 || px >= Width * Scale || py >= Height * Scale)
		return;
	a = std::min(a, 1.0f);
	auto p = &RGBA[(static_cast<size_t>(py) * Width * Scale + px) * 4];
	p[0] = c.R * a + p[0] * (1 - a);
	p[1] = c.G * a + p[1] * (1 - a);
	p[2] = c.B * a + p[2] * (1 - a);
	p[3] = a + p[3] * (1 - a);
}

void Art2D::Clear(ArtColor c, float alpha)
{
	for (size_t i = 0; i < RGBA.size(); i += 4)
	{
		RGBA[i] = c.R * alpha;
		RGBA[i + 1] = c.G * alpha;
		RGBA[i + 2] = c.B * alpha;
		RGBA[i + 3] = alpha;
	}
}

namespace
{
	float RoundRectDistance(float px, float py, float x, float y, float w, float h, float r)
	{
		auto cx = x + w * 0.5f, cy = y + h * 0.5f;
		auto qx = std::fabs(px - cx) - (w * 0.5f - r);
		auto qy = std::fabs(py - cy) - (h * 0.5f - r);
		auto ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
		return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
	}
}

void Art2D::FillRect(float x, float y, float w, float h, ArtColor c, float radius, float alpha)
{
	radius = std::min(radius, std::min(w, h) * 0.5f);
	auto s = static_cast<float>(Scale);
	int x0 = static_cast<int>(std::floor(x * s)) - 1, x1 = static_cast<int>(std::ceil((x + w) * s)) + 1;
	int y0 = static_cast<int>(std::floor(y * s)) - 1, y1 = static_cast<int>(std::ceil((y + h) * s)) + 1;
	for (int py = y0; py <= y1; py++)
		for (int px = x0; px <= x1; px++)
		{
			auto d = RoundRectDistance((px + 0.5f) / s, (py + 0.5f) / s, x, y, w, h, radius);
			Blend(px, py, c, Clamp01(0.5f - d * s) * alpha);
		}
}

void Art2D::VerticalGradient(float x, float y, float w, float h, ArtColor top, ArtColor bottom, float radius)
{
	radius = std::min(radius, std::min(w, h) * 0.5f);
	auto s = static_cast<float>(Scale);
	int x0 = static_cast<int>(std::floor(x * s)) - 1, x1 = static_cast<int>(std::ceil((x + w) * s)) + 1;
	int y0 = static_cast<int>(std::floor(y * s)) - 1, y1 = static_cast<int>(std::ceil((y + h) * s)) + 1;
	for (int py = y0; py <= y1; py++)
	{
		auto t = Clamp01(((py + 0.5f) / s - y) / h);
		auto c = top.Mix(bottom, t);
		for (int px = x0; px <= x1; px++)
		{
			auto d = RoundRectDistance((px + 0.5f) / s, (py + 0.5f) / s, x, y, w, h, radius);
			Blend(px, py, c, Clamp01(0.5f - d * s));
		}
	}
}

void Art2D::StrokeRect(float x, float y, float w, float h, float lineWidth, ArtColor c, float radius, float alpha)
{
	radius = std::min(radius, std::min(w, h) * 0.5f);
	auto s = static_cast<float>(Scale);
	int x0 = static_cast<int>(std::floor((x - lineWidth) * s)) - 1;
	int x1 = static_cast<int>(std::ceil((x + w + lineWidth) * s)) + 1;
	int y0 = static_cast<int>(std::floor((y - lineWidth) * s)) - 1;
	int y1 = static_cast<int>(std::ceil((y + h + lineWidth) * s)) + 1;
	for (int py = y0; py <= y1; py++)
		for (int px = x0; px <= x1; px++)
		{
			auto d = std::fabs(RoundRectDistance((px + 0.5f) / s, (py + 0.5f) / s, x, y, w, h, radius)) -
				lineWidth * 0.5f;
			Blend(px, py, c, Clamp01(0.5f - d * s) * alpha);
		}
}

void Art2D::Line(float x0, float y0, float x1, float y1, float width, ArtColor c, float alpha)
{
	auto s = static_cast<float>(Scale);
	auto half = width * 0.5f;
	int bx0 = static_cast<int>(std::floor((std::min(x0, x1) - half) * s)) - 1;
	int bx1 = static_cast<int>(std::ceil((std::max(x0, x1) + half) * s)) + 1;
	int by0 = static_cast<int>(std::floor((std::min(y0, y1) - half) * s)) - 1;
	int by1 = static_cast<int>(std::ceil((std::max(y0, y1) + half) * s)) + 1;
	auto dx = x1 - x0, dy = y1 - y0;
	auto len2 = dx * dx + dy * dy;
	for (int py = by0; py <= by1; py++)
		for (int px = bx0; px <= bx1; px++)
		{
			auto fx = (px + 0.5f) / s, fy = (py + 0.5f) / s;
			auto t = len2 > 0 ? Clamp01(((fx - x0) * dx + (fy - y0) * dy) / len2) : 0.0f;
			auto ex = fx - (x0 + dx * t), ey = fy - (y0 + dy * t);
			auto d = std::sqrt(ex * ex + ey * ey) - half;
			Blend(px, py, c, Clamp01(0.5f - d * s) * alpha);
		}
}

void Art2D::Circle(float cx, float cy, float r, ArtColor c, float alpha)
{
	FillRect(cx - r, cy - r, r * 2, r * 2, c, r, alpha);
}

void Art2D::Glow(float cx, float cy, float r, ArtColor c, float strength)
{
	auto s = static_cast<float>(Scale);
	int x0 = static_cast<int>((cx - r) * s), x1 = static_cast<int>((cx + r) * s);
	int y0 = static_cast<int>((cy - r) * s), y1 = static_cast<int>((cy + r) * s);
	for (int py = y0; py <= y1; py++)
		for (int px = x0; px <= x1; px++)
		{
			auto dx = (px + 0.5f) / s - cx, dy = (py + 0.5f) / s - cy;
			auto t = 1.0f - std::sqrt(dx * dx + dy * dy) / r;
			if (t > 0)
				Blend(px, py, c, t * t * strength);
		}
}

float Art2D::TextWidth(const std::string& text, float size)
{
	if (text.empty())
		return 0;
	auto unit = size / 6.0f;
	return (text.size() * GlyphAdvance - (GlyphAdvance - 4.0f)) * unit;
}

float Art2D::Text(const std::string& text, float x, float y, float size, float weight, ArtColor c, int align,
                  float alpha)
{
	auto unit = size / 6.0f;
	auto width = TextWidth(text, size);
	if (align == 1)
		x -= width * 0.5f;
	else if (align == 2)
		x -= width;

	auto penX = x;
	for (auto raw : text)
	{
		auto ch = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
		std::string glyph = GlyphFor(ch);
		size_t pos = 0;
		while (pos < glyph.size())
		{
			auto end = glyph.find('|', pos);
			if (end == std::string::npos)
				end = glyph.size();
			auto stroke = glyph.substr(pos, end - pos);
			pos = end + 1;

			float prevX = 0, prevY = 0;
			bool havePrev = false;
			for (size_t i = 0; i + 1 < stroke.size() + 1; i += 3)
			{
				if (i + 1 >= stroke.size())
					break;
				auto gx = static_cast<float>(stroke[i] - '0'), gy = static_cast<float>(stroke[i + 1] - '0');
				auto px = penX + gx * unit, py = y + gy * unit;
				if (havePrev)
					Line(prevX, prevY, px, py, weight, c, alpha);
				else if (stroke.size() == 2)
					Line(px, py, px, py, weight, c, alpha);
				prevX = px;
				prevY = py;
				havePrev = true;
			}
		}
		penX += GlyphAdvance * unit;
	}
	return width;
}

ArtImage Art2D::Resolve(int factor) const
{
	ArtImage img;
	auto bufW = Width * Scale, bufH = Height * Scale;
	img.Width = bufW / factor;
	img.Height = bufH / factor;
	img.Pixels.assign(static_cast<size_t>(img.Width) * img.Height, 0);
	img.Depth.assign(static_cast<size_t>(img.Width) * img.Height, 0);
	auto total = static_cast<float>(factor * factor);
	for (int y = 0; y < img.Height; y++)
		for (int x = 0; x < img.Width; x++)
		{
			float r = 0, g = 0, b = 0, a = 0;
			for (int sy = 0; sy < factor; sy++)
				for (int sx = 0; sx < factor; sx++)
				{
					auto p = &RGBA[(static_cast<size_t>(y * factor + sy) * bufW + (x * factor + sx)) * 4];
					r += p[0];
					g += p[1];
					b += p[2];
					a += p[3];
				}
			a /= total;
			if (a < 0.5f)
				continue;
			// Opaque art: colours stay premultiplied over black, which is what the panel sits on.
			img.Pixels[static_cast<size_t>(y) * img.Width + x] = Pack({r / total, g / total, b / total});
		}
	return img;
}
