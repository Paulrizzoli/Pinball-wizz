#pragma once

// A small JSON reader and writer for Pinball Wizz table files.
// It handles everything a table file uses: objects, arrays, strings, numbers, true/false/null.

#include <string>
#include <vector>
#include <utility>

namespace minijson
{
	class Value
	{
	public:
		enum class Type { Null, Bool, Number, String, Array, Object };

		Value() = default;
		explicit Value(bool b) : type(Type::Bool), boolean(b) { }
		explicit Value(double n) : type(Type::Number), number(n) { }
		explicit Value(const std::string& s) : type(Type::String), text(s) { }

		static Value MakeArray() { Value v; v.type = Type::Array; return v; }
		static Value MakeObject() { Value v; v.type = Type::Object; return v; }

		Type GetType() const { return type; }
		bool IsNull() const { return type == Type::Null; }
		bool IsNumber() const { return type == Type::Number; }
		bool IsString() const { return type == Type::String; }
		bool IsArray() const { return type == Type::Array; }
		bool IsObject() const { return type == Type::Object; }
		bool IsBool() const { return type == Type::Bool; }

		// Lookups never fail: a missing key or index gives a Null value.
		const Value& operator[](const char* key) const;
		const Value& operator[](const std::string& key) const { return (*this)[key.c_str()]; }
		const Value& operator[](size_t index) const;
		bool Has(const char* key) const;
		size_t Size() const;

		double Num(double def = 0.0) const { return type == Type::Number ? number : def; }
		float Float(float def = 0.0f) const { return static_cast<float>(Num(def)); }
		int Int(int def = 0) const { return type == Type::Number ? static_cast<int>(number) : def; }
		bool Bool(bool def = false) const { return type == Type::Bool ? boolean : def; }
		std::string Str(const std::string& def = std::string()) const { return type == Type::String ? text : def; }

		const std::vector<Value>& Items() const { return items; }
		const std::vector<std::pair<std::string, Value>>& Members() const { return members; }

		// Building values (used when saving a table).
		void Push(const Value& v);
		void Set(const std::string& key, const Value& v);

		std::string Dump(int indent = 0) const;

	private:
		friend class Parser;
		Type type = Type::Null;
		bool boolean = false;
		double number = 0.0;
		std::string text;
		std::vector<Value> items;
		std::vector<std::pair<std::string, Value>> members;

		void DumpTo(std::string& out, int indent, int depth) const;
	};

	// Returns false and fills error on malformed input.
	bool Parse(const std::string& textIn, Value& out, std::string& error);
}
