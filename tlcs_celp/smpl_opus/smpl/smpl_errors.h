#ifndef SMPL_ERRORS_H
#define SMPL_ERRORS_H

#ifdef __cplusplus
extern "C"
{
#endif

/******************/
/* Error messages */
/******************/
#define SMPL_NO_ERROR                               0

/**************************/
/* Encoder error messages */
/**************************/

/* Input length is not a multiple of 10 ms, or length is longer than the packet length */
#define SMPL_ENC_INPUT_INVALID_NO_OF_SAMPLES        -101

/* Sampling frequency not 8000, 12000 or 16000 Hertz */
#define SMPL_ENC_FS_NOT_SUPPORTED                   -102

/* Packet size not 10, 20, 60 or 120 ms */
#define SMPL_ENC_PACKET_SIZE_NOT_SUPPORTED          -103

/* Allocated payload buffer too short */
#define SMPL_ENC_PAYLOAD_BUF_TOO_SHORT              -104

/* Loss rate not between 0 and 100 percent */
#define SMPL_ENC_INVALID_LOSS_RATE                  -105

/* Complexity setting not valid, use 0...10 */
#define SMPL_ENC_INVALID_COMPLEXITY_SETTING         -106

/* Inband FEC setting not valid, use 0 or 1 */
#define SMPL_ENC_INVALID_INBAND_FEC_SETTING         -107

/* DTX setting not valid, use 0 or 1 */
#define SMPL_ENC_INVALID_DTX_SETTING                -108

///* CBR setting not valid, use 0 or 1 */
//#define SMPL_ENC_INVALID_CBR_SETTING                -109

/* Internal encoder error */
#define SMPL_ENC_INTERNAL_ERROR                     -110

/* Channel setting not valid, use 1 or 2 */
#define SMPL_ENC_INVALID_NUMBER_OF_CHANNELS_ERROR   -111

/* Global memory not created */
#define SMPL_ENC_NO_GLOBAL_DATA						-112

/**************************/
/* Decoder error messages */
/**************************/

/* Output sampling frequency lower than internal decoded sampling frequency */
#define SMPL_DEC_INVALID_SAMPLING_FREQUENCY         -200

/* Payload size exceeded the maximum allowed 1024 bytes */
#define SMPL_DEC_PAYLOAD_TOO_LARGE                  -201

/* Payload has bit errors */
#define SMPL_DEC_PAYLOAD_ERROR                      -202

/* Payload has bit errors */
#define SMPL_DEC_INVALID_FRAME_SIZE                 -203

#ifdef __cplusplus
}
#endif

#endif
