#pragma once
#include <fstream>

typedef struct
{
	bool found;
	int gameLength; //In seconds
	int selectedChampion;
} GameData;

void riotInit(std::ofstream *log, const char *apiServer, const char *apiKey);
int riotGetSummonerId(const char *region, const char *name);
int riotGetChampionId(const char *name);
GameData riotGetGameData(const char *platform, int summonerId);
