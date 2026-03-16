#include "silk/main.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void* smpl_create_ec_encoder(opus_uint32 nBytes);
int smpl_ec_tell(void *st);
int smpl_ec_enc_done(void *st, unsigned char buf[], size_t buf_len);
void* smpl_ec_get_srange(void* st);
void* smpl_free(void *st);
void* smpl_create_ec_decoder(unsigned char *data, opus_uint32 len);
void smpl_ec_enc_bits(ec_enc *_this,opus_uint32 _fl,unsigned _ftb);
void smpl_ec_encode(ec_enc *_this,unsigned _fl,unsigned _fh,unsigned _ft);
opus_uint32 smpl_ec_dec_bits(ec_dec *_this,unsigned _ftb);
unsigned smpl_ec_decode(ec_dec *_this,unsigned _ft);
void smpl_ec_dec_update(ec_dec *_this,unsigned _fl,unsigned _fh,unsigned _ft);
int smpl_ec_decode_update(ec_dec* _this, const uint16_t cmf[], int cmfLen);
int smpl_ec_decode_uniform(ec_dec* _this, int N);
//void smpl_dump_ec_state(ec_dec* _this);
#ifdef __cplusplus
}
#endif
