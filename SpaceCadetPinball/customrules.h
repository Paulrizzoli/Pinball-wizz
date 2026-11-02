#pragma once

// Game rules for custom tables: scoring, balls, players and game over.
// Space Cadet keeps its own rules in control.cpp; when a custom table is loaded, control forwards here.

class TBall;
class TPinballTable;
class TPinballComponent;
enum class MessageCode;

class customrules
{
public:
	static void Attach(TPinballTable* table);
	static void Detach();
	static void Handler(MessageCode code, TPinballComponent* caller);
	static void UnstuckBall(TBall& ball, int dt);

private:
	static TPinballTable* Table;
	static bool LastBall;
	static void Drained();
};
