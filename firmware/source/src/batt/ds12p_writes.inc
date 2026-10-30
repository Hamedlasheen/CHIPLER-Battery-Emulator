/* Write SHAPES only: the firmware queues opaque writes and never infers a
 * response transition from them. Wire length and PEC were validated against
 * the captures; payload bytes are not served from this table. */
static const ds_ev_write_t DS12P_WRITES[] = {
    {0x45, DS_EV_BLOCK, 0x04, "P12P-CHARGER/wtx511/4.967321s"},
    {0x46, DS_EV_BLOCK, 0x04, "P12P-CHARGER/wtx512/4.967826s"},
    {0x61, DS_EV_BLOCK, 0x20, "P12P-CHARGER/wtx18316/24.645722s"},
    {0x65, DS_EV_WORD, 0x02, "P12P-POWER/wtx6/4.140934s"},
    {0x73, DS_EV_WORD, 0x02, "P12P-POWER/wtx46/4.208423s"},
    {0x83, DS_EV_WORD, 0x02, "P12P-POWER/wtx198/5.346333s"},
    {0xA4, DS_EV_WORD, 0x02, "P12P-POWER/wtx107/4.391519s"},
};
