// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//        Archiving: SaveGame I/O.
//        Thinker, Ticker.
//
//-----------------------------------------------------------------------------


#include "doom_config.h"

#include "z_zone.h"
#include "p_local.h"
#include "doomstat.h"


int leveltime;

//
// THINKERS
// All thinkers should be allocated by Z_Malloc
// so they can be operated on uniformly.
// The actual structures will vary in size,
// but the first element must be thinker_t.
//

// Both the head and tail of the thinker list.
thinker_t thinkercap;


//
// P_InitThinkers
//
void P_InitThinkers(void)
{
    thinkercap.prev = thinkercap.next = &thinkercap;
}


//
// P_AddThinker
// Adds a new thinker at the end of the list.
//
void P_AddThinker(thinker_t* thinker)
{
    thinkercap.prev->next = thinker;
    thinker->next = &thinkercap;
    thinker->prev = thinkercap.prev;
    thinkercap.prev = thinker;
}


/* Called by P_RunThinkers for thinkers marked for removal.
 * Unlinks the thinker from the doubly-linked list and frees it.
 * Using a real function pointer (actionf_p1 signature) instead of the
 * acint=-1 sentinel avoids LTO's function-pointer alignment analysis
 * proving acint==-1 impossible (RISC-V code pointers are always even)
 * and eliminating the entire removal branch (crash at mepc=0xfffffffe). */
void thinker_nop_and_remove(void* thinker_void)
{
    thinker_t* thinker = (thinker_t*)thinker_void;
    thinker->next->prev = thinker->prev;
    thinker->prev->next = thinker->next;
    Z_Free(thinker);
}


//
// P_RemoveThinker
// Deallocation is lazy -- it will not actually be freed
// until its thinking turn comes up.
//
void P_RemoveThinker(thinker_t* thinker)
{
    thinker->function.acp1 = thinker_nop_and_remove;
}


//
// P_RunThinkers
//
void P_RunThinkers(void)
{
    thinker_t* currentthinker;
    thinker_t* nextthinker;

    currentthinker = thinkercap.next;
    while (currentthinker != &thinkercap)
    {
        /* Save next pointer before calling the thinker; thinker_nop_and_remove
         * calls Z_Free on the current thinker, so we must not touch it after
         * the call.  nextthinker points to a different block and stays valid. */
        nextthinker = currentthinker->next;
        if (currentthinker->function.acp1)
            currentthinker->function.acp1(currentthinker);
        currentthinker = nextthinker;
    }
}


//
// P_Ticker
//
void P_Ticker(void)
{
    int i;

    // run the tic
    if (paused)
        return;

    // pause if in menu and at least one tic has been run
    if (!netgame
        && menuactive
        && !demoplayback
        && players[consoleplayer].viewz != 1)
    {
        return;
    }

    for (i = 0; i < MAXPLAYERS; i++)
        if (playeringame[i])
            P_PlayerThink(&players[i]);

    P_RunThinkers();
    P_UpdateSpecials();
    P_RespawnSpecials();

    // for par times
    leveltime++;
}
