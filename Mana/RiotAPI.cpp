#include "RiotAPI.h"
#include <WinSock2.h>
#include <ws2tcpip.h>
#include <string>
#include <sstream>
#include <iostream>
#include <openssl/ssl.h>
#include <rapidjson/document.h>

#define LOG_PARSE_ERROR(doc, src, retVal) if((doc).GetParseError() != 0) { *g_log << "RiotAPI: Failed to parse document line " << __LINE__ << "; error #" << (doc).GetParseError() << std::endl << src << std::endl << std::endl; return (retVal); }
#define LOG_QUERY_ERROR() *g_log << "RiotAPI: Query line " << __LINE__ << " has FAILED." << std::endl;

static std::ofstream *g_log;
static std::string g_apiKey;
static std::string g_apiHost;

static void sslClose(SOCKET sock, SSL *ssl, SSL_CTX *ctx)
{
	closesocket(sock);
	SSL_shutdown(ssl);
	SSL_free(ssl);
	SSL_CTX_free(ctx);
}

static char *nextLine(char *str, int max)
{
	char *ptr = str;
	for(; ; ptr++) {
		if(ptr - str >= max - 1)
			return NULL;

		if(*ptr == '\r' && *(++ptr) == '\n')
			break;
	}

	return ++ptr;
}

static void appendBuffer(char **content, int *contentSz, const char *src, int sz)
{
	if(*content == NULL) {
		*content = new char[sz];
		memcpy(*content, src, sz);
		*contentSz = sz;
	} else {
		char *np = new char[*contentSz + sz];
		memcpy(np, *content, *contentSz);
		memcpy(np + *contentSz, src, sz);

		delete[](*content);
		*content = np;
		*contentSz += sz;
	}
}

//WARNING!!!
//The function you are about to read is TERRIBLE.
//Please wash your eyes with acid after reading it.
//Thanks.
static bool httpGetRequest(const char *loc, char **content, bool forceNa = false)
{
	*g_log << "RiotAPI query: " << loc << "; Force NA: " << forceNa << std::endl;

	//Step one: resolve hostname
	struct addrinfo hints;
	struct addrinfo *result;

	ZeroMemory(&hints, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if(getaddrinfo(forceNa ? "na.api.pvp.net" : g_apiHost.c_str(), NULL, &hints, &result) != 0)
		return false;

	if(result->ai_addr == NULL || result->ai_addrlen != sizeof(sockaddr_in)) {
		freeaddrinfo(result);
		return false;
	}

	sockaddr_in addr;
	memcpy(&addr, result->ai_addr, sizeof(sockaddr_in));
	freeaddrinfo(result);
	addr.sin_port = htons(443);

	//Step two: create socket
	SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(sock == INVALID_SOCKET)
		return false;

	if(connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(sockaddr_in)) == SOCKET_ERROR) {
		closesocket(sock);
		return false;
	}

	//SSL STUFF
	SSL_CTX *ctx = SSL_CTX_new(SSLv23_client_method());
	SSL *ssl = SSL_new(ctx);
	SSL_set_fd(ssl, sock);
	SSL_connect(ssl);

	//Step three: send request
	std::string req("GET ");
	req += loc;
	req += " HTTP/1.1\r\nHost: ";
	req += (forceNa ? std::string("na.api.pvp.net") : g_apiHost);
	req += "\r\nUser-Agent: LoL Mana\r\n\r\n";

	if(SSL_write(ssl, req.c_str(), req.length()) != req.length()) {
		sslClose(sock, ssl, ctx);
		return false;
	}

	//Step four: read response
	char buf[8192];
	int read = SSL_read(ssl, buf, 8192);
	if(read <= 0) {
		sslClose(sock, ssl, ctx);
		return false;
	}

	char *response = buf;
	int num = 0;
	bool wasEmpty = false;
	int retCode = -1;
	int contentLength = -1;

	while(!wasEmpty) {
		char *ptr = nextLine(response, read);
		int len = ptr - response;

		if(ptr == NULL) //End!!
			break;

		read -= len;
		len -= 2;

		if(len <= 0) //Only CRLF; skip
			wasEmpty = true;
		else if(num == 0 || contentLength < 0) {
			char *line = new char[len + 1];
			memcpy(line, response, len);
			line[len] = 0;

			std::istringstream iss(line);
			delete[] line;

			if(num == 0) {
				std::string dontcare;
				iss >> dontcare;
				iss >> retCode;
			} else if(contentLength < 0) {
				std::string key;
				iss >> key;

				if(key == "Content-Length:")
					iss >> contentLength;
			}
		}

		num++;
		response = ptr;
	}

	if(retCode != 200) {
		*g_log << "RiotAPI query failed with return code " << retCode << std::endl;

		sslClose(sock, ssl, ctx);
		return false;
	}

	if(contentLength <= 0) { //Chunked Transfer Coding
		int total = 0;
		*content = NULL;

		while(true) {
			//Check that we have enough data
			if(read < 3) {
				if(read != 0)
					*g_log << "RiotAPI: HTTP: Still got " << read << " bytes remaining!" << std::endl;

				read = SSL_read(ssl, buf, 8192);
				if(read <= 0) {
					if(*content != NULL)
						delete[](*content);

					sslClose(sock, ssl, ctx);
					return false;
				}

				response = buf;
			}

			//Read chunk size
			char *ptr = nextLine(response, read);
			int len = ptr - response;

			if(ptr == NULL) //What should I do in this case? O.o
				break;

			read -= len;
			len -= 2;

			char *line = new char[len + 1];
			memcpy(line, response, len);
			line[len] = 0;

			std::istringstream iss(line);
			delete[] line;

			int chunkSize;
			iss >> std::hex >> chunkSize;

			if(chunkSize == 0) //This was last-chunk
				break;

			bool notEnough = false;
			if(read < chunkSize)
				notEnough = true;

			//Read chunk content
			response = ptr;
			appendBuffer(content, &total, response, notEnough ? read : chunkSize);

			if(notEnough) {
				chunkSize -= read;

				while(chunkSize > 0) {
					read = SSL_read(ssl, buf, 8192);
					if(read <= 0) {
						delete[](*content);
						sslClose(sock, ssl, ctx);
						return false;
					}

					notEnough = (read < chunkSize);
					appendBuffer(content, &total, buf, notEnough ? read : chunkSize);

					if(notEnough)
						chunkSize -= read; 
					else {
						response = buf + chunkSize;
						read -= chunkSize;
						chunkSize = 0;
					}
				}
			} else {
				response += chunkSize;
				read -= chunkSize;
			}

			//Check for CRLF
			if(read < 2 || response[0] != '\r' || response[1] != '\n') {
				*g_log << "RiotAPI: HTTP: Expected CRLF at the end of the chunk." << std::endl;

				delete[](*content);
				sslClose(sock, ssl, ctx);
				return false;
			}

			response += 2;
			read -= 2;
		}

		appendBuffer(content, &total, "", 1);
		return true;
	}

	if(contentLength == read) {
		sslClose(sock, ssl, ctx);
		*content = new char[read + 1];
		memcpy(*content, response, read);
		(*content)[read] = 0;

		return true;
	}

	if(contentLength < read) {
		sslClose(sock, ssl, ctx);
		*content = new char[contentLength + 1];
		memcpy(*content, response, contentLength);
		(*content)[contentLength] = 0;

		return true;
	}

	//We need more data!
	*g_log << "RiotAPI: Note: Need more data (Got " << read << " out of " << contentLength << " bytes)" << std::endl;

	int total = read;
	*content = new char[contentLength + 1];
	memcpy(*content, response, read);

	while(total < contentLength) {
		read = SSL_read(ssl, *content + total, contentLength - total);
		if(read <= 0) {
			delete[](*content);
			sslClose(sock, ssl, ctx);
			return false;
		}

		total += read;
	}

	(*content)[contentLength] = 0;
	sslClose(sock, ssl, ctx);
	return true;
}

int riotGetSummonerId(const char *region, const char *name)
{
	std::ostringstream oss;
	oss << "/api/lol/" << region << "/v1.4/summoner/by-name/";
	oss << name;
	oss << "?api_key=" << g_apiKey;

	char *json;
	if(httpGetRequest(oss.str().c_str(), &json)) {
		rapidjson::Document doc;
		doc.Parse(json);

		LOG_PARSE_ERROR(doc, json, 0);
		delete[] json;

		return doc.MemberBegin()->value["id"].GetInt();
	} else {
		LOG_QUERY_ERROR();
		return 0;
	}
}

int riotGetChampionId(const char *name)
{
	std::ostringstream oss;
	oss << "/api/lol/static-data/na/v1.2/champion?api_key=" << g_apiKey;

	char *json;
	if(httpGetRequest(oss.str().c_str(), &json, true)) {
		rapidjson::Document doc;
		doc.Parse(json);

		LOG_PARSE_ERROR(doc, json, 0);
		delete[] json;

		return doc["data"][name]["id"].GetInt();
	} else {
		LOG_QUERY_ERROR();
		return 0;
	}
}

GameData riotGetGameData(const char *plaform, int summonerId)
{
	GameData ret;
	ZeroMemory(&ret, sizeof(GameData));

	std::ostringstream oss;
	oss << "/observer-mode/rest/consumer/getSpectatorGameInfo/";
	oss << plaform << '/';
	oss << summonerId;
	oss << "?api_key=" << g_apiKey;

	char *json;
	if(httpGetRequest(oss.str().c_str(), &json)) {
		rapidjson::Document doc;
		doc.Parse(json);

		LOG_PARSE_ERROR(doc, json, ret);
		delete[] json;

		rapidjson::Value &list = doc["participants"];
		if(!list.IsArray()) {
			*g_log << "RiotAPI: CurrentGameInfo.participants is not an array (type = " << list.GetType() << ")" << std::endl;
			return ret;
		}

		ret.found = true;
		ret.gameLength = doc["gameLength"].GetInt();

		for(unsigned int i = 0; i < list.Size(); i++) {
			rapidjson::Value &player = list[i];

			if(player["summonerId"].GetInt() == summonerId) {
				ret.selectedChampion = player["championId"].GetInt();
				break;
			}
		}
	} else
		LOG_QUERY_ERROR();

	return ret;
}

void riotInit(std::ofstream *log, const char *apiServer, const char *apiKey)
{
	*log << "RiotAPI starting. Server: " << apiServer << std::endl;

	//Should already be initialized; but just in case...
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);

	SSL_library_init();
	SSL_load_error_strings();

	g_apiHost = apiServer;
	g_apiKey = apiKey;
	g_log = log;
}
