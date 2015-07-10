#include <Windows.h>
#include <TlHelp32.h>
#include <shlobj.h>
#include <string>
#include <map>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/filereadstream.h>
#include "RiotAPI.h"

#define LOG_FILE "LoLMana.log"
#define CONFIG_FILE "LoLMana.json"

static int g_summonerId;
static int g_minimumMana;
static void *g_baseAddress;

static void *getPointer(void *base, int offset)
{
	char *ptr = *static_cast<char**>(base);
	return static_cast<void*>(ptr + offset);
}

static int getMana()
{
	void *ptr0 = getPointer(g_baseAddress, 0x14);
	void *ptr1 = getPointer(ptr0, 0x40);
	void *ptr2 = getPointer(ptr1, 0x18);
	void *ptr3 = getPointer(ptr2, 0x88);
	void *ptr4 = getPointer(ptr3, 0x4C);

	return *static_cast<int*>(ptr4);
}

static std::string getMyDocumentsFile(const char *fle)
{
	char docs[MAX_PATH];
	SHGetFolderPath(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docs);

	std::string docsDir(docs);
	if(docsDir[docsDir.size() - 1] != '\\')
		docsDir += '\\';

	docsDir += fle;
	return docsDir;
}

static DWORD WINAPI manaThread(LPVOID userdata)
{
	//Create log file
	std::ofstream log(getMyDocumentsFile(LOG_FILE).c_str(), std::ofstream::out | std::ofstream::trunc);
	log << "Log opened." << std::endl;

	//Load config
	FILE *fle = fopen(getMyDocumentsFile(CONFIG_FILE).c_str(), "r");
	char buffer[8192];
	rapidjson::FileReadStream is(fle, buffer, 8192);
	rapidjson::Document d;
	d.ParseStream<0, rapidjson::UTF8<>, rapidjson::FileReadStream>(is);
	fclose(fle);

	if(d.GetParseError() != 0) {
		log << "Failed to parse config file: parse error #" << d.GetParseError() << std::endl;
		return 0;
	}

	//Init Riot API and get summoner ID
	riotInit(&log, d["api"]["host"].GetString(), d["api"]["key"].GetString());
	g_summonerId = riotGetSummonerId(d["player"]["region"].GetString(), d["player"]["summoner"].GetString());

	log << "Summoner ID is " << g_summonerId << std::endl;

	//Parse champion list
	rapidjson::Value &champs = d["champions"];
	std::map<int, int> champMana;

	for(auto champ = champs.MemberBegin(); champ != champs.MemberEnd(); champ++) {
		int cId = riotGetChampionId(champ->name.GetString());
		log << "Champion ID for " << champ->name.GetString() << " is " << cId << std::endl;

		champMana[cId] = champ->value.GetInt();
	}

	//Retrieve current game data
	GameData gd;
	gd.found = false;

	while(!gd.found) {
		gd = riotGetGameData(d["player"]["platform"].GetString(), g_summonerId);

		if(!gd.found) {
			log << "Couldn't read game data. Trying again in 10 seconds." << std::endl;

			for(int i = 0; i < 10; i++)
				Sleep(1000);
		}
	}

	if(champMana.find(gd.selectedChampion) == champMana.end()) {
		log << "The champion you are playing was NOT configured." << std::endl;
		return 0;
	}

	//Wait for 10 minutes
	int time = 10 * 60 - gd.gameLength;
	log << "Waiting some more " << time / 60 << " seconds." << std::endl;

	for(int i = 0; i < time; i++)
		Sleep(1000);

	//Get minimum mana for champion
	int minMana = champMana[gd.selectedChampion];
	log << "Minimum mana for this chamion: " << minMana << std::endl;

	//Beep data
	rapidjson::Value &alarm = d["alarm"];

	while(true) {
		if(getMana() < minMana) {
			Beep(alarm["frequency"].GetInt(), alarm["time"].GetInt());
			Sleep(alarm["pause"].GetInt());
		}
	}

	return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	if(fdwReason == DLL_PROCESS_ATTACH) {
		MODULEENTRY32 entry;
		entry.dwSize = sizeof(MODULEENTRY32);

		HANDLE finder = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
		Module32First(finder, &entry);
		CloseHandle(finder);

		DWORD tid;
		g_baseAddress = static_cast<void*>(reinterpret_cast<char*>(entry.modBaseAddr) + 0x011D67DC);
		CreateThread(NULL, 0, manaThread, NULL, 0, &tid);
	}

	return TRUE;
}
