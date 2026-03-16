#include <gtest/gtest.h>
#include <math.h>
#include "smpl_api.h"

#define MAX_PACKET 1500

TEST(SmplApiTest, BasicsDecApiWorking)
{
    void *dec = NULL;
    smpl_DecControlStruct decCtrl;
    int dec_size;
    unsigned char encoded_bytes[MAX_PACKET];
    ec_dec ent_dec;

    ASSERT_EQ(SMPL_NO_ERROR, smpl_Get_Decoder_Size(&dec_size));
    ASSERT_GT(dec_size, 0);
    dec = malloc(dec_size);
    ASSERT_EQ(SMPL_NO_ERROR, smpl_InitDecoder(dec));
    free(dec);
    // Don't test decoding yet
}
