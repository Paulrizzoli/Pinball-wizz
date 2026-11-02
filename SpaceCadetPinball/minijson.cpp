#include "pch.h"
#include "minijson.h"

#include <cstdlib>

namespace minijson
{
	namespace
	{
		const Value& NullValue()
		{
			static const Value nullValue;
			return nullValue;
		}

		void AppendUtf8(std::string& out, unsigned codePoint)
		{
			if (codePoint < 0x80)
				out += static_cast<char>(codePoint);
			else if (codePoint < 0x800)
			{
				out += static_cast<char>(0xC0 | (codePoint >> 6));
				out += static_cast<char>(0x80 | (codePoint & 0x3F));
			}
			else if (codePoint < 0x10000)
			{
				out += static_cast<char>(0xE0 | (codePoint >> 12));
				out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (codePoint & 0x3F));
			}
			else
			{
				out += static_cast<char>(0xF0 | (codePoint >> 18));
				out += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
				out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (codePoint & 0x3F));
			}
		}
	}

	class Parser
	{
	public:
		Parser(const std::string& text) : Text(text)
		{
		}

		bool Run(Value& out, std::string& error)
		{
			SkipSpace();
			if (!ParseValue(out, 0))
			{
				error = Error.empty() ? "Invalid JSON" : Error;
				error += " (at character " + std::to_string(Pos) + ")";
				return false;
			}
			SkipSpace();
			if (Pos != Text.size())
			{
				error = "Unexpected text after the end of the table data (at character " + std::to_string(Pos) + ")";
				return false;
			}
			return true;
		}

	private:
		const std::string& Text;
		size_t Pos = 0;
		std::string Error;

		void SkipSpace()
		{
			while (Pos < Text.size())
			{
				auto c = Text[Pos];
				if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
					Pos++;
				else
					break;
			}
		}

		bool Fail(const char* message)
		{
			if (Error.empty())
				Error = message;
			return false;
		}

		bool ParseValue(Value& out, int depth)
		{
			if (depth > 64)
				return Fail("Table data is nested too deeply");
			if (Pos >= Text.size())
				return Fail("Unexpected end of table data");

			auto c = Text[Pos];
			if (c == '{')
				return ParseObject(out, depth);
			if (c == '[')
				return ParseArray(out, depth);
			if (c == '"')
			{
				out = Value(std::string());
				return ParseString(out.text);
			}
			if (c == '-' || (c >= '0' && c <= '9'))
				return ParseNumber(out);
			if (Text.compare(Pos, 4, "true") == 0)
			{
				Pos += 4;
				out = Value(true);
				return true;
			}
			if (Text.compare(Pos, 5, "false") == 0)
			{
				Pos += 5;
				out = Value(false);
				return true;
			}
			if (Text.compare(Pos, 4, "null") == 0)
			{
				Pos += 4;
				out = Value();
				return true;
			}
			return Fail("Unexpected character");
		}

		bool ParseObject(Value& out, int depth)
		{
			out = Value::MakeObject();
			Pos++;
			SkipSpace();
			if (Pos < Text.size() && Text[Pos] == '}')
			{
				Pos++;
				return true;
			}
			while (true)
			{
				SkipSpace();
				if (Pos >= Text.size() || Text[Pos] != '"')
					return Fail("Expected a name in quotes");
				std::string key;
				if (!ParseString(key))
					return false;
				SkipSpace();
				if (Pos >= Text.size() || Text[Pos] != ':')
					return Fail("Expected ':'");
				Pos++;
				SkipSpace();
				Value item;
				if (!ParseValue(item, depth + 1))
					return false;
				out.members.emplace_back(key, std::move(item));
				SkipSpace();
				if (Pos < Text.size() && Text[Pos] == ',')
				{
					Pos++;
					continue;
				}
				if (Pos < Text.size() && Text[Pos] == '}')
				{
					Pos++;
					return true;
				}
				return Fail("Expected ',' or '}'");
			}
		}

		bool ParseArray(Value& out, int depth)
		{
			out = Value::MakeArray();
			Pos++;
			SkipSpace();
			if (Pos < Text.size() && Text[Pos] == ']')
			{
				Pos++;
				return true;
			}
			while (true)
			{
				SkipSpace();
				Value item;
				if (!ParseValue(item, depth + 1))
					return false;
				out.items.push_back(std::move(item));
				SkipSpace();
				if (Pos < Text.size() && Text[Pos] == ',')
				{
					Pos++;
					continue;
				}
				if (Pos < Text.size() && Text[Pos] == ']')
				{
					Pos++;
					return true;
				}
				return Fail("Expected ',' or ']'");
			}
		}

		static int HexDigit(char c)
		{
			if (c >= '0' && c <= '9')
				return c - '0';
			if (c >= 'a' && c <= 'f')
				return c - 'a' + 10;
			if (c >= 'A' && c <= 'F')
				return c - 'A' + 10;
			return -1;
		}

		bool ParseHex4(unsigned& value)
		{
			if (Pos + 4 > Text.size())
				return Fail("Bad \\u escape");
			value = 0;
			for (int i = 0; i < 4; i++)
			{
				auto d = HexDigit(Text[Pos + i]);
				if (d < 0)
					return Fail("Bad \\u escape");
				value = value * 16 + d;
			}
			Pos += 4;
			return true;
		}

		bool ParseString(std::string& out)
		{
			Pos++; // opening quote
			out.clear();
			while (Pos < Text.size())
			{
				auto c = Text[Pos++];
				if (c == '"')
					return true;
				if (c != '\\')
				{
					out += c;
					continue;
				}
				if (Pos >= Text.size())
					break;
				auto e = Text[Pos++];
				switch (e)
				{
				case '"': out += '"';
					break;
				case '\\': out += '\\';
					break;
				case '/': out += '/';
					break;
				case 'b': out += '\b';
					break;
				case 'f': out += '\f';
					break;
				case 'n': out += '\n';
					break;
				case 'r': out += '\r';
					break;
				case 't': out += '\t';
					break;
				case 'u':
					{
						unsigned cp;
						if (!ParseHex4(cp))
							return false;
						if (cp >= 0xD800 && cp <= 0xDBFF && Pos + 6 <= Text.size() && Text[Pos] == '\\' &&
							Text[Pos + 1] == 'u')
						{
							Pos += 2;
							unsigned low;
							if (!ParseHex4(low))
								return false;
							cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
						}
						AppendUtf8(out, cp);
						break;
					}
				default:
					return Fail("Bad escape in text");
				}
			}
			return Fail("Text is missing its closing quote");
		}

		bool ParseNumber(Value& out)
		{
			auto start = Text.c_str() + Pos;
			char* end = nullptr;
			auto value = std::strtod(start, &end);
			if (end == start)
				return Fail("Bad number");
			Pos += static_cast<size_t>(end - start);
			out = Value(value);
			return true;
		}
	};

	const Value& Value::operator[](const char* key) const
	{
		if (type != Type::Object)
			return NullValue();
		for (const auto& member : members)
		{
			if (member.first == key)
				return member.second;
		}
		return NullValue();
	}

	const Value& Value::operator[](size_t index) const
	{
		if (type != Type::Array || index >= items.size())
			return NullValue();
		return items[index];
	}

	bool Value::Has(const char* key) const
	{
		if (type != Type::Object)
			return false;
		for (const auto& member : members)
		{
			if (member.first == key)
				return true;
		}
		return false;
	}

	size_t Value::Size() const
	{
		if (type == Type::Array)
			return items.size();
		if (type == Type::Object)
			return members.size();
		return 0;
	}

	void Value::Push(const Value& v)
	{
		if (type != Type::Array)
		{
			*this = MakeArray();
		}
		items.push_back(v);
	}

	void Value::Set(const std::string& key, const Value& v)
	{
		if (type != Type::Object)
			*this = MakeObject();
		for (auto& member : members)
		{
			if (member.first == key)
			{
				member.second = v;
				return;
			}
		}
		members.emplace_back(key, v);
	}

	std::string Value::Dump(int indent) const
	{
		std::string out;
		DumpTo(out, indent, 0);
		return out;
	}

	static void DumpString(std::string& out, const std::string& s)
	{
		out += '"';
		for (auto ch : s)
		{
			auto c = static_cast<unsigned char>(ch);
			switch (c)
			{
			case '"': out += "\\\"";
				break;
			case '\\': out += "\\\\";
				break;
			case '\n': out += "\\n";
				break;
			case '\r': out += "\\r";
				break;
			case '\t': out += "\\t";
				break;
			default:
				if (c < 0x20)
				{
					char buf[8];
					snprintf(buf, sizeof buf, "\\u%04x", c);
					out += buf;
				}
				else
					out += static_cast<char>(c);
			}
		}
		out += '"';
	}

	void Value::DumpTo(std::string& out, int indent, int depth) const
	{
		auto newline = [&](int d)
		{
			if (indent <= 0)
				return;
			out += '\n';
			out.append(static_cast<size_t>(indent * d), ' ');
		};

		switch (type)
		{
		case Type::Null:
			out += "null";
			break;
		case Type::Bool:
			out += boolean ? "true" : "false";
			break;
		case Type::Number:
			{
				char buf[40];
				if (number == std::floor(number) && std::fabs(number) < 1e15)
					snprintf(buf, sizeof buf, "%.0f", number);
				else
					snprintf(buf, sizeof buf, "%.6g", number);
				out += buf;
				break;
			}
		case Type::String:
			DumpString(out, text);
			break;
		case Type::Array:
			{
				// Short arrays of numbers (points, colours) stay on one line.
				bool flat = true;
				for (const auto& item : items)
					if (item.type == Type::Array || item.type == Type::Object)
						flat = false;
				out += '[';
				for (size_t i = 0; i < items.size(); i++)
				{
					if (i)
						out += flat || indent <= 0 ? ", " : ",";
					if (!flat)
						newline(depth + 1);
					items[i].DumpTo(out, indent, depth + 1);
				}
				if (!flat && !items.empty())
					newline(depth);
				out += ']';
				break;
			}
		case Type::Object:
			out += '{';
			for (size_t i = 0; i < members.size(); i++)
			{
				if (i)
					out += ',';
				newline(depth + 1);
				DumpString(out, members[i].first);
				out += indent > 0 ? ": " : ":";
				members[i].second.DumpTo(out, indent, depth + 1);
			}
			if (!members.empty())
				newline(depth);
			out += '}';
			break;
		}
	}

	bool Parse(const std::string& textIn, Value& out, std::string& error)
	{
		// Skip a UTF-8 byte order mark, which some editors add.
		if (textIn.size() >= 3 && static_cast<unsigned char>(textIn[0]) == 0xEF &&
			static_cast<unsigned char>(textIn[1]) == 0xBB && static_cast<unsigned char>(textIn[2]) == 0xBF)
		{
			std::string trimmed = textIn.substr(3);
			Parser parser(trimmed);
			return parser.Run(out, error);
		}
		Parser parser(textIn);
		return parser.Run(out, error);
	}
}
