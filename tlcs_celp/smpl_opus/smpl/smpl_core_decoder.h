#ifndef SMPL_CORE_DECODER_H
#define SMPL_CORE_DECODER_H

#include "smpl_structs.h"
#include "smpl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

void smpl_core_decode_init(
    smpl_core_decoder* dec_state
);

int smpl_core_decode(
    smpl_core_decoder *dec_state,
    smpl_DecControlStruct *decControl, /* I/O  Control Structure                               */
    smpl_TOC *toc,                     /* I/O  Table of Contents                               */
    const opus_int lostFlag,           /* I    0: no loss, 1 loss, 2 decode fec                */
    opus_int newPacketFlag,            /* I    Indicates first decoder call for this packet    */
    ec_dec *psRangeDec,                /* I/O  Compressor data structure                       */
    opus_int16 *samplesOut,            /* O    Decoded output speech vector                    */
    opus_int32 *nSamplesOut            /* O    Number of samples decoded                       */
);

#ifdef __cplusplus
}
#endif

#endif
