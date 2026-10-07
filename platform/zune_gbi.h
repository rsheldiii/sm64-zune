#ifndef ZUNE_GBI_H
#define ZUNE_GBI_H
/* Zune HD display-list extension, interpreted by gfx_pc.c (patch 0013): a texture load without
 * the 4096-texel limit of G_LOADBLOCK's 12-bit size field. Used for the skybox atlas
 * (patch 0014). w1 is the byte count to load from the last G_SETTIMG address. */
#define G_ZUNE_LOADBIGBLOCK 0x30

#define gDPZuneLoadBigBlock(pkt, bytes)                     \
    {                                                       \
        Gfx *_g = (Gfx *)(pkt);                             \
        _g->words.w0 = _SHIFTL(G_ZUNE_LOADBIGBLOCK, 24, 8); \
        _g->words.w1 = (uintptr_t)(bytes);                  \
    }
#endif
