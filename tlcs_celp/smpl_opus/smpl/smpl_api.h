
#ifndef SMPL_API_H
#define SMPL_API_H

#ifdef __cplusplus
extern "C"
{
#endif
    // To avoid issue with entrophy codec which isn't explicitly c declared
    #include "entenc.h"
    #include "entdec.h"
#ifdef __cplusplus
}
#endif
#include "opus_types.h"
#include "opus_defines.h"
#include "smpl_errors.h"
#include "smpl_structs.h"

#ifdef __cplusplus
extern "C"
{
#endif

/***********************************************/
/* Creates Read only tables in dynamic memory  */
/***********************************************/
opus_int smpl_CreateCodec(void);

/***********************************************/
/* Free Read only tables in dynamic memory     */
/***********************************************/
opus_int smpl_FreeCodec(void);

//
///* Struct for TOC (Table of Contents) */
//typedef struct {
//    opus_int    VADFlag;                                /* Voice activity for packet                            */
//    opus_int    VADFlags[ SMPL_MAX_FRAMES_PER_PACKET ]; /* Voice activity for each frame in packet              */
//    opus_int    inbandFECFlag;                          /* Flag indicating if packet contains in-band FEC       */
//} smpl_TOC_struct;

/****************************************/
/* Encoder functions                    */
/****************************************/

/***********************************************/
/* Get size in bytes of the SMPL encoder state */
/***********************************************/
opus_int smpl_Get_Encoder_Size(                         /* O    Returns error code                              */
    opus_int                        *encSizeBytes       /* O    Number of bytes in SILK encoder state           */
);

/*************************/
/* Init or reset encoder */
/*************************/
opus_int smpl_InitEncoder(                              /* O    Returns error code                              */
    void                            *encState,          /* I/O  State                                           */
    smpl_EncControlStruct           *encStatus          /* O    Encoder Status                                  */
);

/**************************/
/* Encode frame with SMPL */
/**************************/
/* Note: if prefillFlag is set, the input must contain 10 ms of audio, irrespective of what                     */
/* encControl->payloadSize_ms is set to                                                                         */
opus_int smpl_Encode(                                   /* O    Returns error code                              */
    void                            *encState,          /* I/O  State                                           */
    smpl_EncControlStruct           *encControl,        /* I    Control status                                  */
    const opus_int16                *samplesIn,         /* I    Speech sample input vector                      */
    opus_int                        nSamplesIn,         /* I    Number of samples (per channel) in input vector */
    ec_enc                          *psRangeEnc,        /* I/O  Compressor data structure                       */
    unsigned char                   *toc_byte,          /* O    TOC byte to be put first in payload             */
    opus_int32                      *nBytesOut,         /* I/O  Number of bytes in payload TOC + range encoded (input: Max bytes)   */
    const opus_int                  prefillFlag,        /* I    Flag to indicate prefilling buffers no coding   */
    int                             activity            /* I    Decision of Opus voice activity detector        */
);

/******************************************/
/* Reencode frame with lower bitrate SMPL */
/******************************************/
opus_int smpl_Encode_secondary(                                   /* O    Returns error code                              */
    void* encState,               /* I/O  State                                           */
    smpl_EncControlStruct* encControl,        /* I    Control status                                  */
    ec_enc* psRangeEnc,           /* I/O  Compressor data structure                       */
    unsigned char* toc_byte,      /* O    TOC byte to be put first in payload             */
    opus_int32* nBytesOut         /* I/O  Number of bytes in payload TOC + range encoded (input: Max bytes)   */
);

void smpl_open_enc_files(void* encState);          /* I/O  State                                           */
void smpl_close_enc_files(void* encState);          /* I/O  State                                           */

/****************************************/
/* Decoder functions                    */
/****************************************/

/***********************************************/
/* Get size in bytes of the Silk decoder state */
/***********************************************/
opus_int smpl_Get_Decoder_Size(                         /* O    Returns error code                              */
    opus_int                        *decSizeBytes       /* O    Number of bytes in SMPL decoder state           */
);

/*************************/
/* Init or Reset decoder */
/*************************/
opus_int smpl_InitDecoder(                              /* O    Returns error code                              */
    void                            *decState           /* I/O  State                                           */
);

/******************/
/* Decode a frame */
/******************/
opus_int smpl_Decode(                                   /* O    Returns error code                              */
    void*                           decState,           /* I/O  State                                           */
    smpl_DecControlStruct*          decControl,         /* I/O  Control Structure                               */
    opus_int                        lostFlag,           /* I    0: no loss, 1 loss, 2 decode fec                */
    opus_int                        newPacketFlag,      /* I    Indicates first decoder call for this packet    */
    ec_dec                          *psRangeDec,        /* I/O  Compressor data structure                       */
    unsigned char                   toc_byte,           /* I    TOC byte describing the payload                 */
    opus_int16                      *samplesOut,        /* O    Decoded output speech vector                    */
    opus_int32                      *nSamplesOut        /* I/O  I: Buffer size (per channel)                    */
                                                        /*      O: Number of samples decoded per channel        */
);

void smpl_open_dec_files(void* decState);          /* I/O  State                                           */
void smpl_close_dec_files(void* decState);          /* I/O  State                                           */

#ifdef __cplusplus
}
#endif

#endif
