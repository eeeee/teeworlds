/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_VERSION_H
#define GAME_VERSION_H
#include "generated/nethash.cpp"
#include "generated/nethash_cust.c"
#define GAME_VERSION "0.6 trunk"
#define GAME_NETVERSION "0.6 " GAME_NETVERSION_HASH
#define GAME_NETVERSION_CUST "0.6 " GAME_NETVERSION_HASH_CUST
static const char GAME_RELEASE_VERSION[8] = {'0', '.', '6', '1', 0};
#endif
