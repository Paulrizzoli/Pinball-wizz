#include "pch.h"
#include <limits>
#include "models3d.h"

#include "customtable.h"
#include "hdgfx.h"
#include "loader.h"
#include "ledfx.h"
#include "options.h"
#include "pb.h"
#include "proj.h"
#include "render.h"
#include "TBall.h"
#include "TBumper.h"
#include "TCircle.h"
#include "TFlipper.h"
#include "TFlipperEdge.h"
#include "TPinballTable.h"
#include "winmain.h"

models3d::TableLook models3d::Look;

namespace
{

	struct V3
	{
		float x, y, z;
	};

	inline V3 operator+(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
	inline V3 operator-(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
	inline V3 operator*(const V3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
	inline V3 Mul(const V3& a, const V3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
	inline float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline float Len(const V3& a) { return std::sqrt(Dot(a, a)); }

	inline V3 Norm(const V3& a)
	{
		auto l = Len(a);
		return l > 1e-12f ? a * (1.0f / l) : V3{0, 0, 1};
	}

	inline float Sq(float x) { return x * x; }

	// Schlick's cheap stand-in for pow(x, n), close enough for highlights and much faster.
	inline float SpecPow(float x, float n) { return x / (n - n * x + x); }

	inline V3 Rgb(const models3d::Color3& c) { return {c.R, c.G, c.B}; }

	inline float Smooth(float e0, float e1, float x)
	{
		auto t = std::min(1.0f, std::max(0.0f, (x - e0) / (e1 - e0)));
		return t * t * (3 - 2 * t);
	}

	// The table's own camera (proj.cpp), with helpers to go between table space and 1x screen pixels.
	struct Camera
	{
		float R[3][4]{};
		float D = 1, Cx = 0, Cy = 0;
		V3 Pos{};

		void Load()
		{
			auto& m = proj::Matrix();
			const vector_type4* rows[3] = {&m.Row0, &m.Row1, &m.Row2};
			for (int i = 0; i < 3; i++)
			{
				R[i][0] = rows[i]->X;
				R[i][1] = rows[i]->Y;
				R[i][2] = rows[i]->Z;
				R[i][3] = rows[i]->W;
			}
			D = proj::FocalLength();
			Cx = proj::CenterX();
			Cy = proj::CenterY();
			// The rotation is orthonormal, so the camera sits at -R^T * t.
			Pos = ToTable({-R[0][3], -R[1][3], -R[2][3]});
		}

		V3 ToCam(const V3& p) const
		{
			return {
				R[0][0] * p.x + R[0][1] * p.y + R[0][2] * p.z + R[0][3],
				R[1][0] * p.x + R[1][1] * p.y + R[1][2] * p.z + R[1][3],
				R[2][0] * p.x + R[2][1] * p.y + R[2][2] * p.z + R[2][3]
			};
		}

		// Camera-space direction to table space.
		V3 ToTable(const V3& c) const
		{
			return {
				R[0][0] * c.x + R[1][0] * c.y + R[2][0] * c.z,
				R[0][1] * c.x + R[1][1] * c.y + R[2][1] * c.z,
				R[0][2] * c.x + R[1][2] * c.y + R[2][2] * c.z
			};
		}

		bool Project(const V3& p, float& sx, float& sy) const
		{
			auto c = ToCam(p);
			if (c.z < 0.01f)
				return false;
			sx = c.x * D / c.z + Cx;
			sy = c.y * D / c.z + Cy;
			return true;
		}

		// Table-space direction of the ray through a 1x screen point.
		V3 Ray(float sx, float sy) const
		{
			return Norm(ToTable({(sx - Cx) / D, (sy - Cy) / D, 1.0f}));
		}

		bool OnPlane(float sx, float sy, float z, V3& out) const
		{
			auto d = Ray(sx, sy);
			if (std::fabs(d.z) < 1e-6f)
				return false;
			auto t = (z - Pos.z) / d.z;
			if (t <= 0)
				return false;
			out = Pos + d * t;
			return true;
		}
	};

	// A view of one of the renderer's 1x buffers, or its HD twin.
	struct DepthView
	{
		const uint16_t* Z = nullptr;
		int Stride = 0, W = 0, H = 0;
		float Scale = 1;

		uint16_t At(float sx, float sy) const
		{
			auto x = static_cast<int>(sx * Scale), y = static_cast<int>(sy * Scale);
			if (!Z || x < 0 || y < 0 || x >= W || y >= H)
				return 0xFFFF;
			return Z[Stride * y + x];
		}
	};

	struct ColorView
	{
		const ColorRgba* P = nullptr;
		int Stride = 0, W = 0, H = 0;
		float Scale = 1;

		V3 Px(int x, int y) const
		{
			x = std::max(0, std::min(W - 1, x));
			y = std::max(0, std::min(H - 1, y));
			auto c = P[Stride * y + x];
			return {c.GetRed() / 255.0f, c.GetGreen() / 255.0f, c.GetBlue() / 255.0f};
		}

		// Bilinear sample at a 1x screen point.
		V3 At(float sx, float sy) const
		{
			if (!P)
				return {0, 0, 0};
			auto fx = sx * Scale - 0.5f, fy = sy * Scale - 0.5f;
			auto x = static_cast<int>(std::floor(fx)), y = static_cast<int>(std::floor(fy));
			auto tx = fx - x, ty = fy - y;
			auto a = Px(x, y) * (1 - tx) + Px(x + 1, y) * tx;
			auto b = Px(x, y + 1) * (1 - tx) + Px(x + 1, y + 1) * tx;
			return a * (1 - ty) + b * ty;
		}
	};

	struct Material
	{
		V3 Albedo{0.8f, 0.8f, 0.8f};
		float Spec = 0.4f, Shininess = 30.0f;
		float Reflect = 0.0f; // how much of the environment it mirrors
		float F0 = 0.04f;     // reflection straight on (Fresnel); grazing angles always reflect more
		bool Metal = false;
		V3 Emit{0, 0, 0};
	};

	struct LampLight
	{
		V3 Pos, Color;
		float Strength;
	};

	// Everything a model needs to light itself this frame.
	struct Scene
	{
		Camera Cam;
		DepthView Depth;
		ColorView Table;
		V3 Key{}, Fill{};
		V3 KeyColor{1.0f, 0.97f, 0.92f}, FillColor{0.42f, 0.46f, 0.55f};
		float Ambient = 0.24f;
		std::vector<LampLight> AllLamps;
		LampLight Lamps[4]{};
		int LampCount = 0;
		float LampRange = 1.5f;
		bool Reflections = true;
		float FloorZ = 0;
		float BallRadius = 0.3f;

		void PickLamps(const V3& at)
		{
			LampCount = 0;
			float best[4]{};
			for (auto& lamp : AllLamps)
			{
				auto d = Len(lamp.Pos - at) / LampRange;
				auto w = lamp.Strength / (1 + d * d);
				if (w < 0.03f)
					continue;
				int slot = std::min(LampCount, 3);
				if (LampCount == 4 && w <= best[3])
					continue;
				while (slot > 0 && best[slot - 1] < w)
				{
					best[slot] = best[slot - 1];
					Lamps[slot] = Lamps[slot - 1];
					slot--;
				}
				best[slot] = w;
				Lamps[slot] = lamp;
				LampCount = std::min(4, LampCount + 1);
			}
		}

		// The room above the cabinet: a lit cabinet interior and backbox near the horizon, a brighter ceiling,
		// a soft box where the key light is and a dimmer one for the fill (what makes chrome look like chrome).
		V3 Room(const V3& r) const
		{
			auto up = std::max(0.0f, r.z);
			V3 c = V3{0.2f, 0.2f, 0.23f} + V3{0.32f, 0.32f, 0.36f} * std::sqrt(up);
			auto k = Dot(r, Key);
			c = c + KeyColor * (Smooth(0.9f, 0.965f, k) * 1.6f + Sq(Sq(std::max(0.0f, k))) * 0.35f);
			auto f = Dot(r, Fill);
			c = c + FillColor * (Smooth(0.93f, 0.98f, f) * 1.1f);
			return c;
		}

		// What a mirror at p sees along r: the live table below (lamps and all), or the room above.
		V3 Environment(const V3& p, const V3& r) const
		{
			auto room = Room(r);
			if (!Reflections || r.z > -0.02f)
				return room;
			auto t = (FloorZ - p.z) / r.z;
			auto hit = p + r * t;
			float sx, sy;
			if (!Cam.Project(hit, sx, sy))
				return room;
			// The playfield is lit by the cabinet's own lights; its art is darker than it looks in a reflection.
			auto table = Table.At(sx, sy) * 1.35f + V3{0.04f, 0.04f, 0.05f};
			// Fade into the room near the horizon, where the table reflection would stretch.
			auto k = Smooth(0.02f, 0.18f, -r.z);
			return room * (1 - k) + table * (0.92f * k);
		}

		V3 Shade(const Material& m, const V3& p, V3 n) const
		{
			auto v = Norm(Cam.Pos - p);
			auto nv = Dot(n, v);
			if (nv < 0)
			{
				// Silhouette normals can face slightly away after interpolation; bend them back.
				n = Norm(n - v * (nv * 1.05f));
				nv = 0.0f;
			}
			V3 diff = V3{1, 1, 1} * (Ambient * (0.7f + 0.3f * std::max(0.0f, n.z)));
			V3 spec{0, 0, 0};
			auto light = [&](const V3& l, const V3& color, float amount)
			{
				auto d = Dot(n, l);
				if (d <= 0)
					return;
				diff = diff + color * (d * amount);
				auto h = Norm(l + v);
				auto s = SpecPow(std::max(0.0f, Dot(n, h)), m.Shininess);
				spec = spec + color * (s * amount * m.Spec);
			};
			light(Key, KeyColor, 0.62f);
			light(Fill, FillColor, 0.7f);
			for (int i = 0; i < LampCount; i++)
			{
				auto& lamp = Lamps[i];
				auto l = lamp.Pos - p;
				auto dist = Len(l);
				if (dist < 1e-4f)
					continue;
				auto d = dist / LampRange;
				light(l * (1.0f / dist), lamp.Color, lamp.Strength * 1.6f / (1 + d * d));
			}

			auto g = 1 - nv;
			auto fresnel = m.F0 + (1 - m.F0) * Sq(Sq(g)) * g;
			V3 env{0, 0, 0};
			// Plastics seen face on reflect almost nothing; skip the lookup when it can't show.
			if (m.Reflect > 0 && (m.Metal || fresnel * m.Reflect > 0.025f))
				env = Environment(p, n * (2 * nv) - v);

			V3 c;
			if (m.Metal)
				c = Mul(m.Albedo, env * m.Reflect + diff * (1 - m.Reflect) * 0.6f) + spec;
			else
				c = Mul(m.Albedo, diff) * (1 - fresnel * m.Reflect) + env * (fresnel * m.Reflect) + spec;
			return c + m.Emit;
		}
	};

	struct Vertex
	{
		V3 P, N;
	};

	struct Triangle
	{
		int A, B, C;
		int Mat;
	};

	struct Mesh
	{
		std::vector<Vertex> V;
		std::vector<Triangle> T;
		std::vector<Material> Mats;

		int Add(const V3& p, const V3& n)
		{
			V.push_back({p, Norm(n)});
			return static_cast<int>(V.size()) - 1;
		}

		void Clear()
		{
			V.clear();
			T.clear();
			Mats.clear();
		}

		// Connects two rings of the same size into a band of quads.
		void Band(const std::vector<int>& a, const std::vector<int>& b, int mat, bool closed)
		{
			auto n = a.size();
			for (size_t i = 0; i + (closed ? 0 : 1) < n; i++)
			{
				auto j = (i + 1) % n;
				T.push_back({a[i], a[j], b[j], mat});
				T.push_back({a[i], b[j], b[i], mat});
			}
		}

		void Fan(const V3& center, const V3& normal, const std::vector<int>& ring, int mat)
		{
			auto c = Add(center, normal);
			for (size_t i = 0; i < ring.size(); i++)
				T.push_back({c, ring[i], ring[(i + 1) % ring.size()], mat});
		}
	};

	// An outline in table space: points with outward normals.
	struct Outline
	{
		std::vector<float> X, Y, NX, NY;

		void Push(float x, float y, float nx, float ny)
		{
			X.push_back(x);
			Y.push_back(y);
			NX.push_back(nx);
			NY.push_back(ny);
		}

		size_t Size() const { return X.size(); }
	};

	// The outline of two circles joined by their outer tangents (a flipper). Growing both radii by the same amount
	// keeps every point's angle, so rings of different sizes line up point for point.
	Outline Capsule(float bx, float by, float r0, float tx, float ty, float r1, int tipSteps, int baseSteps)
	{
		Outline o;
		auto dx = tx - bx, dy = ty - by;
		auto d = std::sqrt(dx * dx + dy * dy);
		auto au = std::atan2(dy, dx);
		auto phi = d > 1e-5f ? std::acos(std::max(-1.0f, std::min(1.0f, (r0 - r1) / d))) : Pi / 2;
		for (int i = 0; i <= tipSteps; i++)
		{
			auto a = au - phi + 2 * phi * i / tipSteps;
			o.Push(tx + std::cos(a) * r1, ty + std::sin(a) * r1, std::cos(a), std::sin(a));
		}
		for (int i = 0; i <= baseSteps; i++)
		{
			auto a = au + phi + (2 * Pi - 2 * phi) * i / baseSteps;
			o.Push(bx + std::cos(a) * r0, by + std::sin(a) * r0, std::cos(a), std::sin(a));
		}
		return o;
	}

	Outline CircleOutline(float cx, float cy, float r, int steps)
	{
		Outline o;
		for (int i = 0; i < steps; i++)
		{
			auto a = 2 * Pi * i / steps;
			o.Push(cx + std::cos(a) * r, cy + std::sin(a) * r, std::cos(a), std::sin(a));
		}
		return o;
	}

	// One ring of an outline, grown by grow, at height z; tilt bends the normals up (+) or down (-).
	std::vector<int> Ring(Mesh& mesh, const Outline& o, float grow, float z, float tilt)
	{
		std::vector<int> ring;
		auto side = std::sqrt(std::max(0.0f, 1 - tilt * tilt));
		for (size_t i = 0; i < o.Size(); i++)
			ring.push_back(mesh.Add({o.X[i] + o.NX[i] * grow, o.Y[i] + o.NY[i] * grow, z},
			                        {o.NX[i] * side, o.NY[i] * side, tilt}));
		return ring;
	}

	// Ring around a centre with radius r (for round parts, so grow means radius).
	std::vector<int> RoundRing(Mesh& mesh, float cx, float cy, float r, float z, float tilt, int steps)
	{
		return Ring(mesh, CircleOutline(cx, cy, 1.0f, steps), r - 1.0f, z, tilt);
	}

	// Sub-pixel sample pattern (rotated grid), 4 samples per output pixel.
	const float SampleX[4] = {0.375f, 0.875f, 0.125f, 0.625f};
	const float SampleY[4] = {0.125f, 0.375f, 0.625f, 0.875f};

	// Software render target for one model: colour and depth per sample, plus a soft shadow mask.
	struct Canvas
	{
		int X0 = 0, Y0 = 0; // 1x screen position of the top left corner
		int W = 0, H = 0;   // output pixels
		float S = 1;        // output pixels per 1x pixel
		std::vector<float> Depth;
		std::vector<V3> Color;
		std::vector<float> Shadow;
		std::vector<uint32_t> Pixels;

		void Reset(int x0, int y0, int w1, int h1, float s)
		{
			X0 = x0;
			Y0 = y0;
			S = s;
			W = std::max(1, static_cast<int>(std::ceil(w1 * s)));
			H = std::max(1, static_cast<int>(std::ceil(h1 * s)));
			auto n = static_cast<size_t>(W) * H;
			Depth.assign(n * 4, std::numeric_limits<float>::infinity());
			Color.assign(n * 4, V3{0, 0, 0});
			Shadow.assign(n, 0.0f);
		}

		float ScreenX(float px) const { return X0 + px / S; }
		float ScreenY(float py) const { return Y0 + py / S; }
	};

	// Pixel columns of row py (covering y in [py, py + 1]) that can touch the triangle a, b, c. Returns false if none.
	bool RowSpan(float ax, float ay, float bx, float by, float cx, float cy, int py, int minX, int maxX, int& x0,
	             int& x1)
	{
		float lo = 1e30f, hi = -1e30f;
		auto add = [&](float x)
		{
			lo = std::min(lo, x);
			hi = std::max(hi, x);
		};
		const float ys[2] = {static_cast<float>(py), static_cast<float>(py + 1)};
		const float px[3] = {ax, bx, cx}, pyv[3] = {ay, by, cy};
		for (int e = 0; e < 3; e++)
		{
			auto x0e = px[e], y0e = pyv[e], x1e = px[(e + 1) % 3], y1e = pyv[(e + 1) % 3];
			// Where the edge crosses the row's top and bottom lines (clamped to the edge's ends).
			for (auto y : ys)
			{
				if ((y < std::min(y0e, y1e)) || (y > std::max(y0e, y1e)) || y0e == y1e)
					continue;
				add(x0e + (x1e - x0e) * (y - y0e) / (y1e - y0e));
			}
			if (y0e >= ys[0] && y0e <= ys[1])
				add(x0e);
		}
		if (lo > hi)
			return false;
		x0 = std::max(minX, static_cast<int>(std::floor(lo)) - 1);
		x1 = std::min(maxX, static_cast<int>(std::floor(hi)) + 1);
		return x0 <= x1;
	}

	void RasterMesh(Canvas& cv, const Mesh& mesh, Scene& scene, int depthBias)
	{
		struct Projected
		{
			float X, Y, InvZ;
			V3 Cam;
			bool Ok;
		};
		std::vector<Projected> pv(mesh.V.size());
		for (size_t i = 0; i < mesh.V.size(); i++)
		{
			auto c = scene.Cam.ToCam(mesh.V[i].P);
			auto& q = pv[i];
			q.Ok = c.z > 0.01f;
			if (!q.Ok)
				continue;
			q.Cam = c;
			q.InvZ = 1.0f / c.z;
			q.X = (c.x * scene.Cam.D * q.InvZ + scene.Cam.Cx - cv.X0) * cv.S;
			q.Y = (c.y * scene.Cam.D * q.InvZ + scene.Cam.Cy - cv.Y0) * cv.S;
		}

		// Front to back, so hidden surfaces fail the depth test before they are shaded.
		std::vector<std::pair<float, int>> order;
		order.reserve(mesh.T.size());
		for (size_t t = 0; t < mesh.T.size(); t++)
		{
			auto& tri = mesh.T[t];
			if (!pv[tri.A].Ok || !pv[tri.B].Ok || !pv[tri.C].Ok)
				continue;
			order.emplace_back(std::min({pv[tri.A].Cam.z, pv[tri.B].Cam.z, pv[tri.C].Cam.z}), static_cast<int>(t));
		}
		std::sort(order.begin(), order.end());

		for (auto& entry : order)
		{
			auto& tri = mesh.T[entry.second];
			auto& a = pv[tri.A];
			auto& b = pv[tri.B];
			auto& c = pv[tri.C];
			auto area = (b.X - a.X) * (c.Y - a.Y) - (c.X - a.X) * (b.Y - a.Y);
			if (std::fabs(area) < 1e-7f)
				continue;
			auto inv = 1.0f / area;
			auto minX = std::max(0, static_cast<int>(std::floor(std::min({a.X, b.X, c.X}))));
			auto maxX = std::min(cv.W - 1, static_cast<int>(std::floor(std::max({a.X, b.X, c.X}))));
			auto minY = std::max(0, static_cast<int>(std::floor(std::min({a.Y, b.Y, c.Y}))));
			auto maxY = std::min(cv.H - 1, static_cast<int>(std::floor(std::max({a.Y, b.Y, c.Y}))));
			if (minX > maxX || minY > maxY)
				continue;

			auto& mat = mesh.Mats[tri.Mat];
			auto& va = mesh.V[tri.A];
			auto& vb = mesh.V[tri.B];
			auto& vc = mesh.V[tri.C];
			auto bary = [&](float x, float y, float& w0, float& w1, float& w2)
			{
				w0 = ((b.X - x) * (c.Y - y) - (c.X - x) * (b.Y - y)) * inv;
				w1 = ((c.X - x) * (a.Y - y) - (a.X - x) * (c.Y - y)) * inv;
				w2 = 1 - w0 - w1;
			};

			for (int py = minY; py <= maxY; py++)
			{
				int spanX0, spanX1;
				if (!RowSpan(a.X, a.Y, b.X, b.Y, c.X, c.Y, py, minX, maxX, spanX0, spanX1))
					continue;
				for (int px = spanX0; px <= spanX1; px++)
				{
					int mask = 0;
					float sw0 = 0, sw1 = 0, sw2 = 0;
					auto base = (static_cast<size_t>(py) * cv.W + px) * 4;
					for (int k = 0; k < 4; k++)
					{
						auto x = px + SampleX[k], y = py + SampleY[k];
						float w0, w1, w2;
						bary(x, y, w0, w1, w2);
						if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f)
							continue;
						auto iz = w0 * a.InvZ + w1 * b.InvZ + w2 * c.InvZ;
						auto camP = (a.Cam * (w0 * a.InvZ) + b.Cam * (w1 * b.InvZ) + c.Cam * (w2 * c.InvZ)) * (1.0f / iz);
						auto dist = Len(camP);
						if (dist >= cv.Depth[base + k])
							continue;
						// Hidden behind the table's own art (ramps, rails, wire forms)?
						auto sceneZ = scene.Depth.At(cv.ScreenX(x), cv.ScreenY(y));
						if (static_cast<int>(proj::NormalizeDepth(dist)) > sceneZ + depthBias)
							continue;
						cv.Depth[base + k] = dist;
						mask |= 1 << k;
						sw0 += w0;
						sw1 += w1;
						sw2 += w2;
					}
					if (!mask)
						continue;

					// Shade once per pixel, at the centre when it is inside, else at the covered samples' centroid.
					float w0, w1, w2;
					bary(px + 0.5f, py + 0.5f, w0, w1, w2);
					if (w0 < 0 || w1 < 0 || w2 < 0)
					{
						auto s = sw0 + sw1 + sw2;
						w0 = sw0 / s;
						w1 = sw1 / s;
						w2 = sw2 / s;
					}
					auto k0 = w0 * a.InvZ, k1 = w1 * b.InvZ, k2 = w2 * c.InvZ;
					auto ks = 1.0f / (k0 + k1 + k2);
					k0 *= ks;
					k1 *= ks;
					k2 *= ks;
					auto p = va.P * k0 + vb.P * k1 + vc.P * k2;
					auto n = Norm(va.N * k0 + vb.N * k1 + vc.N * k2);
					auto color = scene.Shade(mat, p, n);
					for (int k = 0; k < 4; k++)
						if (mask & (1 << k))
							cv.Color[base + k] = color;
				}
			}
		}
	}

	// Shadow of a mesh on the table plane z = planeZ, cast away from the key light.
	void RasterMeshShadow(Canvas& cv, const Mesh& mesh, const Scene& scene, float planeZ)
	{
		auto& l = scene.Key;
		std::vector<float> sx(mesh.V.size()), sy(mesh.V.size());
		std::vector<char> ok(mesh.V.size());
		for (size_t i = 0; i < mesh.V.size(); i++)
		{
			auto p = mesh.V[i].P;
			auto t = (p.z - planeZ) / l.z;
			auto q = p - l * t;
			q.z = planeZ;
			float x, y;
			ok[i] = scene.Cam.Project(q, x, y);
			sx[i] = (x - cv.X0) * cv.S;
			sy[i] = (y - cv.Y0) * cv.S;
		}
		for (auto& tri : mesh.T)
		{
			if (!ok[tri.A] || !ok[tri.B] || !ok[tri.C])
				continue;
			float ax = sx[tri.A], ay = sy[tri.A], bx = sx[tri.B], by = sy[tri.B], cx = sx[tri.C], cy = sy[tri.C];
			auto area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
			if (std::fabs(area) < 1e-7f)
				continue;
			auto inv = 1.0f / area;
			auto minX = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
			auto maxX = std::min(cv.W - 1, static_cast<int>(std::floor(std::max({ax, bx, cx}))));
			auto minY = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
			auto maxY = std::min(cv.H - 1, static_cast<int>(std::floor(std::max({ay, by, cy}))));
			for (int py = minY; py <= maxY; py++)
			{
				int spanX0, spanX1;
				if (!RowSpan(ax, ay, bx, by, cx, cy, py, minX, maxX, spanX0, spanX1))
					continue;
				// One sample per pixel: the mask is blurred afterwards anyway.
				auto y = py + 0.5f;
				for (int px = spanX0; px <= spanX1; px++)
				{
					auto x = px + 0.5f;
					auto w0 = ((bx - x) * (cy - y) - (cx - x) * (by - y)) * inv;
					auto w1 = ((cx - x) * (ay - y) - (ax - x) * (cy - y)) * inv;
					if (w0 >= 0 && w1 >= 0 && w0 + w1 <= 1)
						cv.Shadow[static_cast<size_t>(py) * cv.W + px] = 1.0f;
				}
			}
		}
	}

	// Box blur of the shadow mask, twice, for a soft penumbra.
	void BlurShadow(Canvas& cv, int radius)
	{
		if (radius <= 0)
			return;
		std::vector<float> tmp(cv.Shadow.size());
		for (int pass = 0; pass < 2; pass++)
		{
			for (int y = 0; y < cv.H; y++)
			{
				float sum = 0;
				auto row = &cv.Shadow[static_cast<size_t>(y) * cv.W];
				for (int x = -radius; x <= radius; x++)
					sum += row[std::max(0, std::min(cv.W - 1, x))];
				for (int x = 0; x < cv.W; x++)
				{
					tmp[static_cast<size_t>(y) * cv.W + x] = sum / (2 * radius + 1);
					sum += row[std::min(cv.W - 1, x + radius + 1)] - row[std::max(0, x - radius)];
				}
			}
			for (int x = 0; x < cv.W; x++)
			{
				float sum = 0;
				for (int y = -radius; y <= radius; y++)
					sum += tmp[static_cast<size_t>(std::max(0, std::min(cv.H - 1, y))) * cv.W + x];
				for (int y = 0; y < cv.H; y++)
				{
					cv.Shadow[static_cast<size_t>(y) * cv.W + x] = sum / (2 * radius + 1);
					sum += tmp[static_cast<size_t>(std::min(cv.H - 1, y + radius + 1)) * cv.W + x] -
						tmp[static_cast<size_t>(std::max(0, y - radius)) * cv.W + x];
				}
			}
		}
	}

	// Keeps shadow only where the table surface itself is visible (not on raised art in front of it).
	void ClipShadowToSurface(Canvas& cv, const Scene& scene, float planeZ)
	{
		for (int py = 0; py < cv.H; py++)
			for (int px = 0; px < cv.W; px++)
			{
				auto& s = cv.Shadow[static_cast<size_t>(py) * cv.W + px];
				if (s <= 0.002f)
					continue;
				auto sx = cv.ScreenX(px + 0.5f), sy = cv.ScreenY(py + 0.5f);
				V3 q;
				if (!scene.Cam.OnPlane(sx, sy, planeZ, q))
				{
					s = 0;
					continue;
				}
				auto dist = Len(q - scene.Cam.Pos);
				auto surface = static_cast<int>(proj::NormalizeDepth(dist - scene.BallRadius * 0.6f));
				if (scene.Depth.At(sx, sy) < surface)
					s = 0;
			}
	}

	// The ball: ray traced per pixel, so its edge and highlights are perfectly round at any size.
	void RasterBall(Canvas& cv, const V3& center, float r, uint16_t ballDepth, const Material& mat, Scene& scene)
	{
		auto& cam = scene.Cam;
		// Only the pixels round the ball's outline need rays.
		int minX = 0, minY = 0, maxX = cv.W - 1, maxY = cv.H - 1;
		float csx, csy;
		auto camC = cam.ToCam(center);
		if (cam.Project(center, csx, csy) && camC.z > r)
		{
			auto rad = (r * std::fabs(cam.D) / (camC.z - r) + 1.0f) * cv.S;
			auto cx = (csx - cv.X0) * cv.S, cy = (csy - cv.Y0) * cv.S;
			minX = std::max(minX, static_cast<int>(cx - rad));
			maxX = std::min(maxX, static_cast<int>(cx + rad));
			minY = std::max(minY, static_cast<int>(cy - rad));
			maxY = std::min(maxY, static_cast<int>(cy + rad));
		}
		for (int py = minY; py <= maxY; py++)
			for (int px = minX; px <= maxX; px++)
			{
				int mask = 0;
				auto base = (static_cast<size_t>(py) * cv.W + px) * 4;
				for (int k = 0; k < 4; k++)
				{
					auto sx = cv.ScreenX(px + SampleX[k]), sy = cv.ScreenY(py + SampleY[k]);
					auto d = cam.Ray(sx, sy);
					auto oc = cam.Pos - center;
					auto b = Dot(oc, d);
					auto c = Dot(oc, oc) - r * r;
					auto disc = b * b - c;
					if (disc < 0)
						continue;
					// Same rule as the original ball sprite: hidden wherever the table art is nearer than its centre.
					if (scene.Depth.At(sx, sy) <= ballDepth)
						continue;
					cv.Depth[base + k] = -b - std::sqrt(disc);
					mask |= 1 << k;
				}
				if (!mask)
					continue;

				auto d = cam.Ray(cv.ScreenX(px + 0.5f), cv.ScreenY(py + 0.5f));
				auto oc = cam.Pos - center;
				auto b = Dot(oc, d);
				auto disc = b * b - (Dot(oc, oc) - r * r);
				V3 n;
				if (disc >= 0)
					n = Norm(cam.Pos + d * (-b - std::sqrt(disc)) - center);
				else
					n = Norm(cam.Pos + d * (-b) - center); // just outside the edge: use the rim
				auto color = scene.Shade(mat, center + n * r, n);
				for (int k = 0; k < 4; k++)
					if (mask & (1 << k))
						cv.Color[base + k] = color;
			}
	}

	// Soft shadow of the ball on the surface under it, plus a darker contact spot.
	void BallShadow(Canvas& cv, const V3& center, float r, const Scene& scene)
	{
		auto planeZ = center.z - r;
		auto& l = scene.Key;
		for (int py = 0; py < cv.H; py++)
			for (int px = 0; px < cv.W; px++)
			{
				V3 q;
				if (!scene.Cam.OnPlane(cv.ScreenX(px + 0.5f), cv.ScreenY(py + 0.5f), planeZ, q))
					continue;
				auto w = center - q;
				auto s = Dot(w, l);
				float shade = 0;
				if (s > 0)
				{
					auto perp = Len(w - l * s);
					auto soft = r * 0.22f + s * 0.12f;
					shade = 1 - Smooth(r - soft, r + soft, perp);
				}
				auto contact = Len(V3{q.x - center.x, q.y - center.y, 0});
				auto ao = (1 - Smooth(0, r * 0.95f, contact)) * 0.75f;
				cv.Shadow[static_cast<size_t>(py) * cv.W + px] = std::max(shade * 0.85f, ao);
			}
	}

	uint32_t PackColor(const V3& c, float a)
	{
		auto ch = [](float v)
		{
			// Soft shoulder so bright highlights roll off instead of clipping flat.
			if (v > 0.8f)
				v = 0.8f + 0.2f * (1 - std::exp(-(v - 0.8f) * 5.0f));
			return static_cast<uint32_t>(std::max(0.0f, std::min(1.0f, v)) * 255.0f + 0.5f);
		};
		auto alpha = static_cast<uint32_t>(std::max(0.0f, std::min(1.0f, a)) * 255.0f + 0.5f);
		return alpha << 24 | ch(c.x) << 16 | ch(c.y) << 8 | ch(c.z);
	}

	// Model over its own shadow, into straight-alpha ARGB pixels.
	void Resolve(Canvas& cv, float shadowStrength)
	{
		cv.Pixels.resize(static_cast<size_t>(cv.W) * cv.H);
		for (size_t i = 0; i < cv.Pixels.size(); i++)
		{
			V3 sum{0, 0, 0};
			int n = 0;
			for (int k = 0; k < 4; k++)
				if (cv.Depth[i * 4 + k] < std::numeric_limits<float>::infinity())
				{
					sum = sum + cv.Color[i * 4 + k];
					n++;
				}
			auto a = n * 0.25f;
			auto shadow = cv.Shadow[i] * shadowStrength * (1 - a);
			auto outA = a + shadow;
			V3 c = n ? sum * (1.0f / n) : V3{0, 0, 0};
			cv.Pixels[i] = outA > 0 ? PackColor(c * (a / outA), outA) : 0;
		}
	}

	// One drawn model with its texture.
	struct Model
	{
		Canvas Cv;
		SDL_Texture* Texture = nullptr;
		int TexW = 0, TexH = 0;
		float SortDepth = 0;
		bool Visible = false;
		float Key = -1;
		std::vector<float> ShadowCache;

		void Upload()
		{
			if (Cv.W > TexW || Cv.H > TexH || !Texture)
			{
				if (Texture)
					SDL_DestroyTexture(Texture);
				TexW = std::max(Cv.W, TexW);
				TexH = std::max(Cv.H, TexH);
				Texture = SDL_CreateTexture(winmain::Renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
				                            TexW, TexH);
				if (!Texture)
					return;
				SDL_SetTextureBlendMode(Texture, SDL_BLENDMODE_BLEND);
#if SDL_VERSION_ATLEAST(2, 0, 12)
				SDL_SetTextureScaleMode(Texture, SDL_ScaleModeLinear);
#endif
			}
			SDL_Rect rect{0, 0, Cv.W, Cv.H};
			SDL_UpdateTexture(Texture, &rect, Cv.Pixels.data(), Cv.W * 4);
		}

		void Destroy()
		{
			if (Texture)
				SDL_DestroyTexture(Texture);
			Texture = nullptr;
			TexW = TexH = 0;
		}
	};

	struct State
	{
		TPinballTable* Table = nullptr;
		Scene Sc;
		std::vector<Model> Balls;
		std::vector<Model> Flippers;
		std::vector<Model> Statics; // bumpers, posts, rails
		std::vector<Mesh> StaticMeshes;
		std::vector<TBumper*> BumperParts; // matching Statics for bumpers that light up (nullptr otherwise)
		std::vector<render_sprite*> HiddenSprites;
		std::vector<ledfx::LitLamp> LampScratch;
		float BuiltScale = 0;
		bool BuiltShadows = false, BuiltReflections = false;
		size_t NextStatic = 0;
		std::vector<float> StaticKeys, LitKeys;
		Material FlipperBody, FlipperRubber, FlipperCap, Chrome;
		float FlipperHeight = 0.45f;
	};

	State St;

	// Depth and colour of the table come from the HD twins when there are some (sharper occlusion edges and
	// reflections), else from the 1x buffers.
	void SetupViews()
	{
		auto& sc = St.Sc;
		auto hd = hdgfx::Active();
		auto z = hd ? hdgfx::Twin(render::GetZScreen()) : render::GetZScreen();
		auto screen = hd ? hdgfx::Twin(render::vscreen) : render::vscreen;
		auto scale = hd ? static_cast<float>(hdgfx::Scale) : 1.0f;
		sc.Depth.Z = z->ZPtr1;
		sc.Depth.Stride = z->Stride;
		sc.Depth.W = z->Width;
		sc.Depth.H = z->Height;
		sc.Depth.Scale = scale;
		sc.Table.P = screen->BmpBufPtr1;
		sc.Table.Stride = screen->Stride;
		sc.Table.W = screen->Width;
		sc.Table.H = screen->Height;
		sc.Table.Scale = scale;
	}

	// Key light from the upper left of the screen and above, like the table art; fill from the lower right.
	void SetupLights(float ballRadius)
	{
		auto& sc = St.Sc;
		auto& cam = sc.Cam;
		V3 o;
		if (!cam.OnPlane(cam.Cx, cam.Cy, 0, o))
			o = {0, 0, 0};
		float ox, oy, xx, xy, yx, yy;
		cam.Project(o, ox, oy);
		cam.Project(o + V3{1, 0, 0}, xx, xy);
		cam.Project(o + V3{0, 1, 0}, yx, yy);
		// Table direction that moves (-1, -1) on screen: solve a*(dX) + b*(dY) = (-1, -1).
		auto ax = xx - ox, ay = xy - oy, bx = yx - ox, by = yy - oy;
		auto det = ax * by - bx * ay;
		V3 upLeft{0, 0, 0};
		if (std::fabs(det) > 1e-6f)
			upLeft = Norm(V3{(-1 * by - bx * -1) / det, (ax * -1 - -1 * ay) / det, 0});
		auto toCam = cam.Pos - o;
		toCam.z = 0;
		toCam = Norm(toCam);
		sc.Key = Norm(upLeft * 0.62f + V3{0, 0, 1} * 0.78f + toCam * 0.12f);
		sc.Fill = Norm(upLeft * -0.55f + V3{0, 0, 1} * 0.6f + toCam * 0.45f);
		sc.BallRadius = ballRadius;
		sc.LampRange = ballRadius * 5.0f;
	}

	void GatherLamps()
	{
		auto& sc = St.Sc;
		sc.AllLamps.clear();
		ledfx::LitLamps(St.LampScratch);
		for (auto& lamp : St.LampScratch)
		{
			V3 p;
			if (lamp.Strength < 0.03f || !sc.Cam.OnPlane(lamp.X, lamp.Y, 0, p))
				continue;
			p.z = sc.BallRadius * 0.6f;
			V3 color{lamp.Color[0] / 255.0f, lamp.Color[1] / 255.0f, lamp.Color[2] / 255.0f};
			sc.AllLamps.push_back({p, color, lamp.Strength});
		}
	}

	// Screen bounds (1x pixels) of a set of table points.
	bool Bounds(const std::vector<V3>& pts, int& x0, int& y0, int& w, int& h)
	{
		float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
		for (auto& p : pts)
		{
			float sx, sy;
			if (!St.Sc.Cam.Project(p, sx, sy))
				return false;
			minX = std::min(minX, sx);
			minY = std::min(minY, sy);
			maxX = std::max(maxX, sx);
			maxY = std::max(maxY, sy);
		}
		x0 = static_cast<int>(std::floor(minX)) - 2;
		y0 = static_cast<int>(std::floor(minY)) - 2;
		w = static_cast<int>(std::ceil(maxX)) + 2 - x0;
		h = static_cast<int>(std::ceil(maxY)) + 2 - y0;
		return w > 0 && h > 0 && w < 2000 && h < 2000;
	}

	// Mesh bounds plus where its shadow lands.
	bool MeshBounds(const Mesh& mesh, bool shadow, int& x0, int& y0, int& w, int& h)
	{
		std::vector<V3> pts;
		for (auto& v : mesh.V)
		{
			pts.push_back(v.P);
			if (shadow)
			{
				auto t = v.P.z / St.Sc.Key.z;
				pts.push_back(v.P - St.Sc.Key * t + V3{0, 0, 0});
			}
		}
		if (pts.empty())
			return false;
		return Bounds(pts, x0, y0, w, h);
	}

	Material Plastic(V3 c)
	{
		Material m;
		m.Albedo = c;
		m.Spec = 0.55f;
		m.Shininess = 48;
		m.Reflect = 0.35f;
		m.F0 = 0.05f;
		return m;
	}

	Material Rubber(V3 c)
	{
		Material m;
		m.Albedo = c;
		m.Spec = 0.12f;
		m.Shininess = 12;
		m.Reflect = 0.0f;
		return m;
	}

	Material Metal(V3 c, float reflect)
	{
		Material m;
		m.Albedo = c;
		m.Spec = 0.9f;
		m.Shininess = 90;
		m.Reflect = reflect;
		m.Metal = true;
		return m;
	}

	// Average colours of a flipper sprite: its light plastic and its saturated rubber.
	void FlipperColors(TFlipper* flipper, V3& body, V3& rubber)
	{
		body = {0.92f, 0.92f, 0.94f};
		rubber = {0.85f, 0.12f, 0.1f};
		if (!flipper->ListBitmap || flipper->ListBitmap->empty())
			return;
		auto bmp = flipper->ListBitmap->at(0).Bmp;
		if (!bmp || !bmp->BmpBufPtr1)
			return;
		V3 lightSum{0, 0, 0}, satSum{0, 0, 0};
		float lightN = 0, satN = 0;
		for (int y = 0; y < bmp->Height; y++)
			for (int x = 0; x < bmp->Width; x++)
			{
				auto px = bmp->BmpBufPtr1[bmp->Stride * y + x];
				if (!px.Color)
					continue;
				V3 c{px.GetRed() / 255.0f, px.GetGreen() / 255.0f, px.GetBlue() / 255.0f};
				auto mx = std::max({c.x, c.y, c.z}), mn = std::min({c.x, c.y, c.z});
				auto sat = mx > 0 ? (mx - mn) / mx : 0;
				if (sat < 0.25f && mx > 0.55f)
				{
					lightSum = lightSum + c;
					lightN++;
				}
				else if (sat > 0.45f && mx > 0.3f)
				{
					satSum = satSum + c;
					satN++;
				}
			}
		if (lightN > 4)
			body = lightSum * (1.0f / lightN);
		if (satN > 4)
		{
			rubber = satSum * (1.0f / satN);
			// The sprite's rubber is shaded; lift it back to the base colour.
			auto mx = std::max({rubber.x, rubber.y, rubber.z});
			if (mx > 0.05f)
				rubber = rubber * (0.85f / mx);
		}
		// Body is plastic, slightly off-white; keep it from looking grey.
		auto mx = std::max({body.x, body.y, body.z});
		if (mx > 0.05f)
			body = body * (0.93f / mx);
	}

	void BuildFlipperMesh(Mesh& mesh, TFlipper* flipper)
	{
		auto edge = flipper->FlipperEdge;
		auto offset = St.Table->CollisionCompOffset;
		auto rb = edge->CirclebaseRadius - offset, rt = edge->CircleT1Radius - offset;
		float s, c;
		maths::SinCos(edge->CurrentAngle, s, c);
		auto tip = edge->T1Src;
		maths::RotatePt(tip, s, c, edge->RotOrigin);
		auto bx = edge->RotOrigin.X, by = edge->RotOrigin.Y;

		auto h = St.FlipperHeight;
		// The rubber ring sticks out past the plastic, so from above it shows as a coloured edge.
		auto rubberT = std::min(rb, rt) * 0.42f;
		mesh.Clear();
		mesh.Mats = {St.FlipperBody, St.FlipperRubber, St.FlipperCap};
		auto outline = Capsule(bx, by, rb, tip.X, tip.Y, rt, 10, 18);

		// Plastic body: straight sides, a rounded top edge and a flat top.
		auto inset = -rubberT;
		auto b0 = Ring(mesh, outline, inset, 0.0f, 0.0f);
		auto b1 = Ring(mesh, outline, inset, h * 0.8f, 0.0f);
		auto b2 = Ring(mesh, outline, inset - rubberT * 0.25f, h * 0.94f, 0.55f);
		auto b3 = Ring(mesh, outline, inset - rubberT * 0.8f, h, 0.92f);
		mesh.Band(b0, b1, 0, true);
		mesh.Band(b1, b2, 0, true);
		mesh.Band(b2, b3, 0, true);
		auto topRing = Ring(mesh, outline, inset - rubberT * 0.8f, h, 1.0f);
		mesh.Fan({(bx + tip.X) * 0.5f, (by + tip.Y) * 0.5f, h}, {0, 0, 1}, topRing, 0);

		// Rubber ring round the sides, bulging out.
		auto r0 = Ring(mesh, outline, inset, h * 0.1f, -0.75f);
		auto r1 = Ring(mesh, outline, inset * 0.3f, h * 0.22f, -0.45f);
		auto r2 = Ring(mesh, outline, 0.0f, h * 0.4f, 0.0f);
		auto r3 = Ring(mesh, outline, inset * 0.3f, h * 0.58f, 0.45f);
		auto r4 = Ring(mesh, outline, inset, h * 0.72f, 0.8f);
		mesh.Band(r0, r1, 1, true);
		mesh.Band(r1, r2, 1, true);
		mesh.Band(r2, r3, 1, true);
		mesh.Band(r3, r4, 1, true);

		// Pivot screw cap.
		auto capR = rb * 0.42f;
		auto c0 = RoundRing(mesh, bx, by, capR, h, 0.0f, 20);
		auto c1 = RoundRing(mesh, bx, by, capR, h + h * 0.06f, 0.0f, 20);
		auto c2 = RoundRing(mesh, bx, by, capR * 0.8f, h + h * 0.1f, 0.7f, 20);
		mesh.Band(c0, c1, 2, true);
		mesh.Band(c1, c2, 2, true);
		auto c3 = RoundRing(mesh, bx, by, capR * 0.8f, h + h * 0.1f, 1.0f, 20);
		mesh.Fan({bx, by, h + h * 0.11f}, {0, 0, 1}, c3, 2);
	}

	void BuildBumperMesh(Mesh& mesh, float x, float y, float r, float height, const V3& body, const V3& cap,
	                     const V3& metal, float lit)
	{
		mesh.Clear();
		auto litCap = Plastic(cap * 0.9f + V3{0.1f, 0.1f, 0.1f});
		litCap.Emit = (cap * 0.9f + V3{0.25f, 0.25f, 0.25f}) * (lit * 0.85f);
		auto litBody = Plastic(body);
		litBody.Emit = body * (lit * 0.45f);
		mesh.Mats = {Plastic(body * 0.55f), litBody, Metal(metal, 0.7f), litCap, Metal({0.92f, 0.92f, 0.95f}, 0.8f)};
		const int n = 32;
		auto H = height;
		// Skirt.
		auto s0 = RoundRing(mesh, x, y, r * 1.22f, 0.0f, 0.5f, n);
		auto s1 = RoundRing(mesh, x, y, r * 1.0f, H * 0.13f, 0.5f, n);
		mesh.Band(s0, s1, 0, true);
		auto s2 = RoundRing(mesh, x, y, r * 1.0f, H * 0.13f, 1.0f, n);
		auto s3 = RoundRing(mesh, x, y, r * 0.84f, H * 0.13f, 1.0f, n);
		mesh.Band(s2, s3, 0, true);
		// Body.
		auto bd0 = RoundRing(mesh, x, y, r * 0.84f, H * 0.12f, 0.0f, n);
		auto bd1 = RoundRing(mesh, x, y, r * 0.84f, H * 0.63f, 0.0f, n);
		mesh.Band(bd0, bd1, 1, true);
		// Metal ring under the cap.
		auto m0 = RoundRing(mesh, x, y, r * 1.0f, H * 0.62f, -0.6f, n);
		auto m1 = RoundRing(mesh, x, y, r * 1.02f, H * 0.66f, 0.0f, n);
		auto m2 = RoundRing(mesh, x, y, r * 1.0f, H * 0.7f, 0.6f, n);
		mesh.Band(m0, m1, 2, true);
		mesh.Band(m1, m2, 2, true);
		// Cap with a rounded top edge.
		auto c0 = RoundRing(mesh, x, y, r * 0.95f, H * 0.7f, 0.0f, n);
		auto c1 = RoundRing(mesh, x, y, r * 0.95f, H * 0.82f, 0.2f, n);
		auto c2 = RoundRing(mesh, x, y, r * 0.86f, H * 0.88f, 0.75f, n);
		auto c3 = RoundRing(mesh, x, y, r * 0.7f, H * 0.9f, 0.97f, n);
		mesh.Band(c0, c1, 3, true);
		mesh.Band(c1, c2, 3, true);
		mesh.Band(c2, c3, 3, true);
		auto c4 = RoundRing(mesh, x, y, r * 0.7f, H * 0.9f, 1.0f, n);
		auto c5 = RoundRing(mesh, x, y, r * 0.34f, H * 0.905f, 1.0f, n);
		mesh.Band(c4, c5, 3, true);
		// Chrome centre screw.
		auto d0 = RoundRing(mesh, x, y, r * 0.3f, H * 0.905f, 0.3f, n);
		auto d1 = RoundRing(mesh, x, y, r * 0.18f, H * 0.95f, 0.85f, n);
		mesh.Band(d0, d1, 4, true);
		mesh.Fan({x, y, H * 0.96f}, {0, 0, 1}, RoundRing(mesh, x, y, r * 0.18f, H * 0.95f, 1.0f, n), 4);
	}

	void BuildPostMesh(Mesh& mesh, float x, float y, float r, const V3& rubber, const V3& metal, float unit)
	{
		mesh.Clear();
		mesh.Mats = {Rubber(rubber), Metal(metal, 0.75f)};
		const int n = 24;
		// Rubber ring (rounded) round a chrome post with a domed top.
		auto r0 = RoundRing(mesh, x, y, r * 0.85f, unit * 0.05f, -0.7f, n);
		auto r1 = RoundRing(mesh, x, y, r * 1.0f, unit * 0.2f, 0.0f, n);
		auto r2 = RoundRing(mesh, x, y, r * 0.85f, unit * 0.35f, 0.7f, n);
		mesh.Band(r0, r1, 0, true);
		mesh.Band(r1, r2, 0, true);
		auto rTop = RoundRing(mesh, x, y, r * 0.85f, unit * 0.35f, 1.0f, n);
		auto rIn = RoundRing(mesh, x, y, r * 0.55f, unit * 0.35f, 1.0f, n);
		mesh.Band(rTop, rIn, 0, true);
		auto p0 = RoundRing(mesh, x, y, r * 0.55f, unit * 0.3f, 0.0f, n);
		auto p1 = RoundRing(mesh, x, y, r * 0.55f, unit * 0.56f, 0.0f, n);
		auto p2 = RoundRing(mesh, x, y, r * 0.4f, unit * 0.62f, 0.75f, n);
		mesh.Band(p0, p1, 1, true);
		mesh.Band(p1, p2, 1, true);
		mesh.Fan({x, y, unit * 0.64f}, {0, 0, 1}, RoundRing(mesh, x, y, r * 0.4f, unit * 0.62f, 1.0f, n), 1);
	}

	// A round chrome tube along a path, held up by thin posts at its points.
	void BuildRailMesh(Mesh& mesh, const std::vector<float>& pts, float z0, float z1, const V3& metal, bool firstPost)
	{
		mesh.Clear();
		mesh.Mats = {Metal(metal, 0.8f)};
		auto count = pts.size() / 2;
		if (count < 2)
			return;
		auto radius = (z1 - z0) * 0.5f;
		auto zc = (z0 + z1) * 0.5f;
		const int n = 10;
		std::vector<int> prev;
		for (size_t i = 0; i < count; i++)
		{
			// Direction along the path (averaged at corners) and its side normal.
			auto ia = i == 0 ? 0 : i - 1, ib = std::min(count - 1, i + 1);
			auto dx = pts[ib * 2] - pts[ia * 2], dy = pts[ib * 2 + 1] - pts[ia * 2 + 1];
			auto dl = std::sqrt(dx * dx + dy * dy);
			if (dl < 1e-5f)
				continue;
			V3 side{-dy / dl, dx / dl, 0};
			std::vector<int> ring;
			for (int k = 0; k < n; k++)
			{
				auto a = 2 * Pi * k / n;
				auto nrm = side * std::cos(a) + V3{0, 0, 1} * std::sin(a);
				ring.push_back(mesh.Add(V3{pts[i * 2], pts[i * 2 + 1], zc} + nrm * radius, nrm));
			}
			if (!prev.empty())
				mesh.Band(prev, ring, 0, true);
			prev = ring;
		}
		// Posts (the first one only on the first piece, so pieces don't double up).
		for (size_t i = firstPost ? 0 : 1; i < count; i++)
		{
			auto x = pts[i * 2], y = pts[i * 2 + 1];
			auto a = RoundRing(mesh, x, y, radius * 0.5f, 0.0f, 0.0f, 8);
			auto b = RoundRing(mesh, x, y, radius * 0.5f, zc, 0.0f, 8);
			mesh.Band(a, b, 0, true);
		}
	}

	// Coarse fingerprint of the lamps lighting a part, so it is only redrawn when that light visibly changes.
	float LampKey(const Scene& sc)
	{
		float key = 0;
		for (int l = 0; l < sc.LampCount; l++)
		{
			auto& lamp = sc.Lamps[l];
			key += std::round(lamp.Strength * 10.0f) * (l + 1) * 7.0f +
				std::round((lamp.Pos.x * 3.1f + lamp.Pos.y * 1.7f) * 4.0f) * 0.013f;
		}
		return key;
	}

	// Lit amount of a bumper (0..1), following the LED fade when it is on.
	float BumperLit(TBumper* bumper)
	{
		auto sprite = bumper->RenderSprite;
		if (!sprite || !bumper->ListBitmap)
			return 0;
		auto isLit = [&](gdrv_bitmap8* bmp)
		{
			auto& list = *bumper->ListBitmap;
			for (size_t i = 1; i < list.size(); i += 2)
				if (list[i].Bmp == bmp)
					return true;
			return false;
		};
		auto fade = ledfx::Enabled() ? ledfx::Ease(sprite->Fade) : 1.0f;
		auto now = isLit(sprite->Bmp) ? 1.0f : 0.0f;
		auto before = sprite->PrevBmp && isLit(sprite->PrevBmp) ? 1.0f : 0.0f;
		return now * fade + before * (1 - fade);
	}

	void ShowSprites()
	{
		for (auto sprite : St.HiddenSprites)
		{
			sprite->Hidden = false;
			sprite->DirtyFlag = true;
		}
		St.HiddenSprites.clear();
	}

	void HideSprite(render_sprite* sprite)
	{
		if (!sprite || sprite->Hidden)
			return;
		sprite->Hidden = true;
		sprite->DirtyFlag = true;
		St.HiddenSprites.push_back(sprite);
	}

	void ClearTable()
	{
		ShowSprites();
		for (auto list : {&St.Balls, &St.Flippers, &St.Statics})
		{
			for (auto& model : *list)
				model.Destroy();
			list->clear();
		}
		St.StaticMeshes.clear();
		St.BumperParts.clear();
		St.StaticKeys.clear();
		St.LitKeys.clear();
		St.NextStatic = 0;
		St.Table = nullptr;
		St.BuiltScale = 0;
	}

	float BallRadius()
	{
		if (St.Table && !St.Table->BallList.empty())
			return St.Table->BallList[0]->Radius;
		return St.Table ? St.Table->CollisionCompOffset : 0.3f;
	}

	// Builds the part list for a newly loaded table.
	void SetupTable(TPinballTable* table)
	{
		ClearTable();
		St.Table = table;
		auto unit = BallRadius();
		auto& look = models3d::Look;
		auto custom = look.Custom && customtable::Active;

		// Flippers: hidden sprites, colours from the art (or the table's theme).
		V3 body{0.93f, 0.93f, 0.95f}, rubber{0.85f, 0.12f, 0.1f}, metal{0.8f, 0.82f, 0.86f};
		if (custom)
		{
			body = Rgb(look.FlipperBody);
			rubber = Rgb(look.FlipperRubber);
			metal = Rgb(look.Metal);
		}
		else if (!table->FlipperList.empty())
			FlipperColors(table->FlipperList[0], body, rubber);
		St.FlipperBody = Plastic(body);
		St.FlipperRubber = Rubber(rubber);
		St.FlipperCap = Metal(metal, 0.75f);
		St.FlipperHeight = custom ? 0.45f : unit * 1.45f;
		St.Flippers.resize(table->FlipperList.size());

		// Static parts. Custom tables list them; Space Cadet's bumpers come from the physics.
		if (custom)
		{
			for (auto& post : look.Posts)
			{
				St.StaticMeshes.emplace_back();
				BuildPostMesh(St.StaticMeshes.back(), post.X, post.Y, post.Radius, Rgb(post.Rubber), Rgb(post.Metal),
				              1.0f);
				St.BumperParts.push_back(nullptr);
			}
			// Each stretch of rail is its own model, so a lamp change redraws only a small piece.
			for (auto& rail : look.Rails)
				for (size_t p = 0; p + 3 < rail.Points.size(); p += 2)
				{
					std::vector<float> piece(rail.Points.begin() + p, rail.Points.begin() + p + 4);
					St.StaticMeshes.emplace_back();
					BuildRailMesh(St.StaticMeshes.back(), piece, rail.Z0, rail.Z1, Rgb(rail.Metal), p == 0);
					St.BumperParts.push_back(nullptr);
				}
		}
		// Space Cadet's bumpers are detailed pre-rendered art (bigger than their physics circle); they stay as they are.
		for (auto component : custom ? table->ComponentList : std::vector<TPinballComponent*>())
		{
			auto bumper = dynamic_cast<TBumper*>(component);
			if (!bumper || bumper->EdgeList.empty())
				continue;
			auto circle = dynamic_cast<TCircle*>(bumper->EdgeList[0]);
			if (!circle)
				continue;
			St.StaticMeshes.emplace_back();
			St.BumperParts.push_back(bumper);
		}
		St.Statics.resize(St.StaticMeshes.size());
		St.StaticKeys.assign(St.StaticMeshes.size(), -1.0f);
		St.LitKeys.assign(St.StaticMeshes.size(), -1.0f);
	}

	void BumperGeometry(TBumper* bumper, float& x, float& y, float& r, V3& body, V3& cap, V3& metal)
	{
		auto circle = static_cast<TCircle*>(bumper->EdgeList[0]);
		x = circle->Circle.Center.X;
		y = circle->Circle.Center.Y;
		r = std::sqrt(circle->Circle.RadiusSq) - St.Table->CollisionCompOffset;
		body = {0.85f, 0.2f, 0.25f};
		cap = {0.95f, 0.95f, 0.97f};
		metal = {0.8f, 0.82f, 0.86f};
		for (auto& b : models3d::Look.Bumpers)
			if (std::fabs(b.X - x) < 0.01f && std::fabs(b.Y - y) < 0.01f)
			{
				body = Rgb(b.Body);
				cap = Rgb(b.Cap);
				metal = Rgb(b.Metal);
				r = b.Radius;
			}
	}

	void DrawModel(Model& model, float originX, float originY, float scaleX, float scaleY)
	{
		if (!model.Visible || !model.Texture)
			return;
		auto& cv = model.Cv;
		SDL_Rect src{0, 0, cv.W, cv.H};
		SDL_FRect dst{
			originX + cv.X0 * scaleX, originY + cv.Y0 * scaleY, cv.W / cv.S * scaleX, cv.H / cv.S * scaleY
		};
		SDL_RenderCopyF(winmain::Renderer, model.Texture, &src, &dst);
	}
}

bool models3d::Active()
{
	return options::Options.Models3D && pb::MainTable && render::vscreen && render::GetZScreen() &&
		!pb::FullTiltMode;
}

bool models3d::DrawsBalls()
{
	return Active();
}

void models3d::Draw()
{
	if (!Active())
	{
		if (St.Table)
			ClearTable();
		return;
	}

	auto table = pb::MainTable;
	if (St.Table != table)
		SetupTable(table);
#ifdef MODELS3D_TRACE
	auto debugStart = std::chrono::steady_clock::now();
#endif

	float originX, originY, scaleX, scaleY;
	SDL_Rect clip;
	render::GetBoardTransform(originX, originY, scaleX, scaleY, clip);
	// Detail: one model pixel per window pixel (capped at 4x for speed); depth and reflections are read from
	// whichever copy of the table there is, at any scale.
	auto s = std::min(4.0f, std::max(1.0f, scaleY));

	auto& sc = St.Sc;
	sc.Cam.Load();
	SetupViews();
	auto unit = BallRadius();
	SetupLights(unit);
	sc.Reflections = options::Options.ModelReflections;
	GatherLamps();
	auto shadows = options::Options.ModelShadows.V;
	auto shadowBlur = std::max(1, static_cast<int>(s * 0.9f));

	// Hide the sprites the models replace.
	for (auto flipper : table->FlipperList)
		HideSprite(flipper->RenderSprite);
	for (auto bumper : St.BumperParts)
		if (bumper)
			HideSprite(bumper->RenderSprite);

	// Static parts: redrawn only when their look changes (lamps nearby, lit state, switches, detail level), and only
	// a few per frame so a burst of lamp changes never costs a frame. Their shadows never change and are kept.
	auto rebuildAll = St.BuiltScale != s || St.BuiltShadows != shadows || St.BuiltReflections != sc.Reflections;
	St.BuiltScale = s;
	St.BuiltShadows = shadows;
	St.BuiltReflections = sc.Reflections;
	int budget = rebuildAll ? 1 << 30 : 3;
	auto count = St.Statics.size();
	for (size_t n = 0; n < count && budget > 0; n++)
	{
		auto i = (St.NextStatic + n) % count;
		auto& model = St.Statics[i];
		auto& mesh = St.StaticMeshes[i];
		auto bumper = St.BumperParts[i];
		float lit = 0;
		V3 center{0, 0, 0};
		if (bumper)
		{
			float x, y, r;
			V3 body, cap, metal;
			BumperGeometry(bumper, x, y, r, body, cap, metal);
			lit = std::round(BumperLit(bumper) * 12.0f) / 12.0f;
			auto height = models3d::Look.Custom ? 0.86f : unit * 2.9f;
			if (lit != St.LitKeys[i] || mesh.V.empty())
				BuildBumperMesh(mesh, x, y, r, height, body, cap, metal, lit);
			center = {x, y, height * 0.5f};
		}
		else if (!mesh.V.empty())
			center = mesh.V[mesh.V.size() / 2].P;

		sc.PickLamps(center);
		auto key = LampKey(sc) + lit * 1000.0f;
		if (!rebuildAll && model.Visible && key == St.StaticKeys[i] && lit == St.LitKeys[i])
			continue;
		St.StaticKeys[i] = key;
		St.LitKeys[i] = lit;
		budget--;
		St.NextStatic = (i + 1) % count;

		int x0, y0, w, h;
		model.Visible = MeshBounds(mesh, shadows, x0, y0, w, h);
		if (!model.Visible)
			continue;
		model.Cv.Reset(x0, y0, w, h, s);
		if (shadows)
		{
			if (model.ShadowCache.size() != model.Cv.Shadow.size() || rebuildAll)
			{
				RasterMeshShadow(model.Cv, mesh, sc, 0);
				BlurShadow(model.Cv, shadowBlur);
				ClipShadowToSurface(model.Cv, sc, 0);
				model.ShadowCache = model.Cv.Shadow;
			}
			else
				model.Cv.Shadow = model.ShadowCache;
		}
		sc.FloorZ = 0;
		// Small depth slack so the part is not hidden by the art painted of itself.
		RasterMesh(model.Cv, mesh, sc, 600);
		Resolve(model.Cv, 0.5f);
		model.Upload();
		model.SortDepth = Len(sc.Cam.ToCam(center));
	}

#ifdef MODELS3D_TRACE
	auto tStatics = std::chrono::steady_clock::now();
#endif
	// Flippers: built from the physics angle, so they turn smoothly instead of in sprite steps. Redrawn when they
	// move or the light on them changes.
	Mesh flipperMesh;
	for (size_t i = 0; i < table->FlipperList.size() && i < St.Flippers.size(); i++)
	{
		auto flipper = table->FlipperList[i];
		auto& model = St.Flippers[i];
		auto center = V3{flipper->FlipperEdge->RotOrigin.X, flipper->FlipperEdge->RotOrigin.Y, 0};
		sc.PickLamps(center);
		auto key = LampKey(sc) + flipper->FlipperEdge->CurrentAngle * 100000.0f;
		if (!rebuildAll && model.Visible && key == model.Key)
			continue;
		model.Key = key;
		BuildFlipperMesh(flipperMesh, flipper);
		int x0, y0, w, h;
		model.Visible = MeshBounds(flipperMesh, shadows, x0, y0, w, h);
		if (!model.Visible)
			continue;
		model.Cv.Reset(x0, y0, w, h, s);
		if (shadows)
		{
			RasterMeshShadow(model.Cv, flipperMesh, sc, 0);
			BlurShadow(model.Cv, shadowBlur);
			ClipShadowToSurface(model.Cv, sc, 0);
		}
		sc.FloorZ = 0;
		RasterMesh(model.Cv, flipperMesh, sc, 300);
		Resolve(model.Cv, 0.5f);
		model.Upload();
		model.SortDepth = Len(sc.Cam.ToCam(center));
	}

#ifdef MODELS3D_TRACE
	auto tFlippers = std::chrono::steady_clock::now();
#endif
	// Balls.
	auto chrome = Metal({0.86f, 0.87f, 0.9f}, 0.88f);
	chrome.Spec = 1.2f;
	chrome.Shininess = 160;
	St.Balls.resize(table->BallList.size());
	for (size_t i = 0; i < table->BallList.size(); i++)
	{
		auto ball = table->BallList[i];
		auto& model = St.Balls[i];
		auto sprite = ball->RenderSprite;
		model.Visible = false;
		if (!ball->ActiveFlag || !sprite || !sprite->Bmp)
			continue;
		V3 center{ball->Position.X, ball->Position.Y, ball->Position.Z};
		auto r = ball->Radius;
		std::vector<V3> pts;
		for (int k = 0; k < 8; k++)
		{
			auto a = 2 * Pi * k / 8;
			for (int z = -1; z <= 1; z += 2)
				pts.push_back(center + V3{std::cos(a) * r * 1.1f, std::sin(a) * r * 1.1f, z * r * 1.1f});
			if (shadows)
			{
				// Where the shadow lands.
				auto ground = center + V3{std::cos(a) * r * 1.4f, std::sin(a) * r * 1.4f, -r};
				pts.push_back(ground);
				auto t = r * 2.4f / sc.Key.z;
				pts.push_back(ground - V3{sc.Key.x, sc.Key.y, 0} * t);
			}
		}
		int x0, y0, w, h;
		if (!Bounds(pts, x0, y0, w, h))
			continue;
		model.Visible = true;
		model.Cv.Reset(x0, y0, w, h, s);
		sc.PickLamps(center);
		sc.FloorZ = center.z - r;
		if (shadows)
		{
			BallShadow(model.Cv, center, r, sc);
			ClipShadowToSurface(model.Cv, sc, center.z - r);
		}
		RasterBall(model.Cv, center, r, sprite->Depth, chrome, sc);
#ifdef MODELS3D_TRACE
		{
			// Where the ball is on screen, for screenshot scripts.
			float wx, wy;
			sc.Cam.Project(center, wx, wy);
			printf("BALLWIN %d %d\n", static_cast<int>(originX + wx * scaleX), static_cast<int>(originY + wy * scaleY));
		}
#endif
		Resolve(model.Cv, 0.55f);
		model.Upload();
		model.SortDepth = Len(sc.Cam.ToCam(center));
	}

	// Far to near.
	std::vector<Model*> order;
	for (auto list : {&St.Statics, &St.Flippers, &St.Balls})
		for (auto& model : *list)
			if (model.Visible)
				order.push_back(&model);
	std::stable_sort(order.begin(), order.end(), [](const Model* a, const Model* b)
	{
		return a->SortDepth > b->SortDepth;
	});

	SDL_RenderSetClipRect(winmain::Renderer, &clip);
	for (auto model : order)
		DrawModel(*model, originX, originY, scaleX, scaleY);
	SDL_RenderSetClipRect(winmain::Renderer, nullptr);
#ifdef MODELS3D_TRACE
	{
		static double total = 0, worst = 0, wS = 0, wF = 0, wB = 0;
		static int frames = 0;
		auto now = std::chrono::steady_clock::now();
		auto ms = std::chrono::duration<double, std::milli>(now - debugStart).count();
		auto msS = std::chrono::duration<double, std::milli>(tStatics - debugStart).count();
		auto msF = std::chrono::duration<double, std::milli>(tFlippers - tStatics).count();
		auto msB = std::chrono::duration<double, std::milli>(now - tFlippers).count();
		total += ms;
		worst = std::max(worst, ms);
		wS = std::max(wS, msS);
		wF = std::max(wF, msF);
		wB = std::max(wB, msB);
		if (++frames == 120)
		{
			printf("models3d: %.2f ms avg, %.2f ms worst (statics %.2f, flippers %.2f, balls %.2f)\n", total / frames, worst, wS, wF, wB);
			total = worst = wS = wF = wB = 0;
			frames = 0;
		}
	}
#endif
}

void models3d::Uninit()
{
	// The sprites are already gone when the renderer shuts down; just drop the models.
	St.HiddenSprites.clear();
	ClearTable();
}
