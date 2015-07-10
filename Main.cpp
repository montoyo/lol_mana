#include <Windows.h>
#include <TlHelp32.h>
#include "Resource.h"

#define DLL_FILE "Mana.dll"

static NOTIFYICONDATA g_notifyIcon;

static void showBaloon(const char *str, int type = NIIF_ERROR)
{
	g_notifyIcon.uFlags = NIF_INFO;
	strcpy(g_notifyIcon.szInfo, str);
	strcpy(g_notifyIcon.szInfoTitle, type == NIIF_ERROR ? "Error" : "Info");
	g_notifyIcon.dwInfoFlags = type;

	Shell_NotifyIcon(NIM_MODIFY, &g_notifyIcon);
}

static void injectDLL(DWORD pid)
{
	showBaloon("LoL Mana woke up!", NIIF_INFO);
	for(int i = 0; i < 60; i++)
		Sleep(1000);

	//Open the target process
	HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
	if(process == NULL) {
		showBaloon("Failed to open process.\nNot admin?");
		return;
	}

	//Get DLL path
	char dllPath[MAX_PATH];
	GetCurrentDirectory(MAX_PATH, dllPath);

	int pathLen = strlen(dllPath);
	if(dllPath[pathLen - 1] != '\\') {
		dllPath[pathLen++] = '\\';
		dllPath[pathLen] = 0;
	}

	strcpy(dllPath + pathLen, DLL_FILE);
	pathLen += strlen(DLL_FILE) + 1;

	//Inject DLL path
	void *remoteDll = VirtualAllocEx(process, NULL, pathLen, MEM_COMMIT, PAGE_READWRITE);
	if(remoteDll == NULL) {
		showBaloon("VirtualAllocEx failed!");
		return;
	}

	DWORD written;
	if(!WriteProcessMemory(process, remoteDll, dllPath, pathLen, &written)) {
		showBaloon("WriteProcessMemory failed!");
		return;
	}

	//Get LoadLibraryA address
	HMODULE kernel32 = GetModuleHandle("kernel32.dll");
	void *threadFunc = GetProcAddress(kernel32, "LoadLibraryA");

	//Start remote thread
	DWORD tid;
	HANDLE thread = CreateRemoteThread(process, NULL, 0, static_cast<LPTHREAD_START_ROUTINE>(threadFunc), remoteDll, 0, &tid);

	if(thread == NULL) {
		showBaloon("CreateRemoteThread failed!");
		return;
	}

	//Wait for LoL exit
	while(WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0);
	CloseHandle(process);
}

int CALLBACK WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	//Create the notify icon
	ZeroMemory(&g_notifyIcon, sizeof(NOTIFYICONDATA));
	g_notifyIcon.cbSize = sizeof(NOTIFYICONDATA);
	g_notifyIcon.uFlags = NIF_ICON | NIF_TIP;
	g_notifyIcon.uVersion = NOTIFYICON_VERSION;
	g_notifyIcon.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
	strcpy(g_notifyIcon.szTip, "LoL Mana");
	Shell_NotifyIcon(NIM_ADD, &g_notifyIcon);

	//Look for league of legends process
	while(true) {
		PROCESSENTRY32 entry;
		entry.dwFlags = sizeof(PROCESSENTRY32);

		DWORD pid = 0;
		HANDLE finder = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

		if(Process32First(finder, &entry)) {
			while(Process32Next(finder, &entry)) {
				if(strcmp(entry.szExeFile, "League of Legends.exe") == 0) {
					pid = entry.th32ProcessID;
					break;
				}
			}
		}

		if(pid != 0)
			injectDLL(pid);

		CloseHandle(finder);
		Sleep(1000);
	}

	return 0;
}
