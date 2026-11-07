/* A public room as the in-game menu lists it (net/netmenu.c fills it; ui_host.h hands it to nxui). Plain C types
 * only: included by C++ (the menu) and by the runtime's C. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct UiRoom {
    char name[128], owner[64], address[128], game[128];
    uint16_t port;
    int players, max_players;
    bool password;
    bool this_game;                         /* the room's preferred game, or a member playing it, is ours */
} UiRoom;

enum { UI_ROOM_NONE, UI_ROOM_JOINING, UI_ROOM_JOINED, UI_ROOM_FAILED };
