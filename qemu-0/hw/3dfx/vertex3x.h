static const int slen[] = 
                    { 8, 4, 4, 4, 4, 4, 12, 4, 8, 4, 8, 4 };
static int vlut[] = { 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0 };
static void vlut_vvars(int param, int offs, int mode)
{
    vlut[GR_PARAM_IDX(param)] = (mode)? offs:0;
    if (GR_PARAM_PARGB == param) {
        vlut[GR_PARAM_IDX(GR_PARAM_A)] = 0;
        vlut[GR_PARAM_IDX(GR_PARAM_RGB)] = 0;
    }
    if (GR_PARAM_RGB == param)
        vlut[GR_PARAM_IDX(GR_PARAM_PARGB)] = 0;
}
static int size_vertex3x(void)
{
    int n = sizeof(slen) / sizeof(int), ret = slen[0];
    for (int i = 0; i < n; i++)
        ret = (vlut[i] && ((vlut[i] + slen[i]) > ret))? (vlut[i] + slen[i]):ret;

    return ret;
}

