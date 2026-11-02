#pragma once

// Game rules for custom tables: scoring, target groups and their awards, end-of-ball bonus, score
// multiplier, extra balls, players and game over.
// Space Cadet keeps its own rules in control.cpp; when a custom table is loaded, control forwards here.

#include <string>
#include <vector>
#include <map>

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
	// Messages the table itself handles (new game, next player...).
	static void TableMessage(MessageCode code);
	static void UnstuckBall(TBall& ball, int dt);

private:
	struct GroupState
	{
		std::vector<bool> Done;
		int Hits = 0;
	};

	static TPinballTable* Table;
	static bool LastBall;
	static int BallBonus;
	static int ActivePlayer;
	static std::vector<GroupState> Groups[4];
	static std::map<int, TPinballComponent*> Parts;

	static void Drained();
	static void PartHit(TPinballComponent* part);
	static void Complete(size_t groupIndex);
	static void SetLight(int group, bool on);
	static void FlashLight(int group, float seconds);
	static void ShowMultiplierLights();
	static void ResetGroups(int player);
	static void Info(const std::string& text, float seconds);
	static void Mission(const std::string& text, float seconds);
};
