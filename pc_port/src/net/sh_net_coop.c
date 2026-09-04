/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See sh_net_coop.h. Deliberately tiny: the value of this file is that there
 * is exactly ONE flag meaning "another player is in this world", set from one
 * place, so nothing has to infer it from ghost counts or lobby membership. */

#include "sh_net_coop.h"
#include "sh_log.h"

int g_ShNetCoopActive = 0;

int ShNet_CoopActive(void)
{
    return g_ShNetCoopActive;
}

void ShNet_SetCoopActive(int active, const char* why)
{
    active = active ? 1 : 0;
    if (active == g_ShNetCoopActive)
    {
        return;
    }
    g_ShNetCoopActive = active;
    SH_DBG("[COOP] %s (%s) - pausing is %s",
           active ? "another player is in this world" : "this world is yours alone",
           why ? why : "no reason given",
           active ? "blocked" : "allowed again");
}

int ShNet_PauseBlocked(void)
{
    return g_ShNetCoopActive;
}
