#include "silk/main.h" // Use real include
#include "smpl_entropy_wrapper.h"
#include <stdlib.h>

typedef struct smpl_ec_state{
    ec_ctx sRange;
    unsigned char *buf;
    opus_uint32 bufBytes;
} smpl_ec_state;

void* smpl_create_ec_encoder(opus_uint32 nBytes)
{
    smpl_ec_state *ec_enc_st = (smpl_ec_state*)malloc(sizeof(smpl_ec_state));
    if(!ec_enc_st){
        return NULL;
    }
    memset(ec_enc_st, 0, sizeof(smpl_ec_state));
    ec_enc_st->buf = (unsigned char*)malloc(nBytes);
    if(!ec_enc_st->buf){
        return NULL;
    }
    ec_enc_st->bufBytes = nBytes;
    ec_enc_init(&ec_enc_st->sRange, ec_enc_st->buf, ec_enc_st->bufBytes);
    return (void*)ec_enc_st;
}

void* smpl_ec_get_srange(void* st)
{
    if (!st) {
        return NULL;
    }
    smpl_ec_state* p_ec = (smpl_ec_state*)st;
    return &p_ec->sRange;
}

int smpl_ec_tell(void *st)
{
    if(!st){
        return -1;
    }
    return ec_tell(&((smpl_ec_state*)st)->sRange);   
}

int smpl_ec_enc_done(void *st, unsigned char buf[], size_t buf_len)
{
    if(!st){
        return -1;
    }
    smpl_ec_state *ec_enc_st = (smpl_ec_state*)st;

    size_t nBytes = (size_t)(ec_tell(&ec_enc_st->sRange) + 7) / 8;
    if(ec_get_error(&ec_enc_st->sRange)){
        return 0;
    }
     if(nBytes > buf_len){
        ec_enc_init(&ec_enc_st->sRange, ec_enc_st->buf, ec_enc_st->bufBytes);
        return 0;
    }
    ec_enc_shrink(&ec_enc_st->sRange, nBytes);
    ec_enc_done(&ec_enc_st->sRange);
    memcpy(buf, ec_enc_st->buf, nBytes);
    ec_enc_init(&ec_enc_st->sRange, ec_enc_st->buf, ec_enc_st->bufBytes);    
    
    return nBytes;
}

void* smpl_free(void *st)
{
    if(st){
       smpl_ec_state *ec_enc_st = (smpl_ec_state*)st;
       if(ec_enc_st->buf){
           free(ec_enc_st->buf);
       }
       free(st);
    }
    return NULL;
}

void* smpl_create_ec_decoder(unsigned char *data, opus_uint32 len)
{
    smpl_ec_state *ec_dec_st = (smpl_ec_state*)malloc(sizeof(smpl_ec_state));
    if(!ec_dec_st || len == 0){
        return NULL;
    }
    memset(ec_dec_st, 0, sizeof(smpl_ec_state));
    ec_dec_st->buf = (unsigned char*)malloc(len);
    if (!ec_dec_st->buf){
        return NULL;
    }
    ec_dec_st->bufBytes = len;
    memcpy(ec_dec_st->buf, data, len);
    ec_dec_init(&ec_dec_st->sRange, ec_dec_st->buf, ec_dec_st->bufBytes);

    return (void*)ec_dec_st;
}

// Pass-through functions to ensure C API that julia can call (as enteec.h doesn't "C" declare them)

void smpl_ec_enc_bits(ec_enc *_this,opus_uint32 _fl,unsigned _ftb)
{
    ec_enc_bits(_this, _fl, _ftb);
}

void smpl_ec_encode(ec_enc *_this,unsigned _fl,unsigned _fh,unsigned _ft)
{
    ec_encode(_this, _fl, _fh, _ft);
}

opus_uint32 smpl_ec_dec_bits(ec_dec *_this,unsigned _ftb)
{
    return ec_dec_bits(_this, _ftb);
}

unsigned smpl_ec_decode(ec_dec *_this,unsigned _ft)
{
    return ec_decode(_this, _ft);
}

void smpl_ec_dec_update(ec_dec *_this,unsigned _fl,unsigned _fh,unsigned _ft)
{
    ec_dec_update(_this, _fl, _fh, _ft);
}

//void smpl_dump_ec_state(ec_dec* _this)
//{
//    printf("buf: %p \n", _this->buf);
//    printf("storage: %d \n", _this->storage);
//    printf("end_offs: %d \n", _this->end_offs);
//    printf("end_window: %d \n", _this->end_window);
//    printf("nend_bits: %d \n", _this->nend_bits);
//    printf("nbits_total: %d \n", _this->nbits_total);
//    printf("offs: %u \n", _this->offs);
//    printf("rng: %u \n", _this->rng);
//    printf("val: %u \n", _this->val);
//    printf("ext: %u \n", _this->ext);
//    printf("rng: %d \n", _this->rem);
//    printf("error: %d \n", _this->error);
//}

int smpl_ec_decode_uniform(ec_dec* _this, int N)
{
    unsigned cmf_low = ec_decode(_this, N);
    ec_dec_update(_this, cmf_low, cmf_low+1, N);
    return cmf_low;
}

int smpl_ec_decode_update(ec_dec* _this, const uint16_t cmf[], int cmfLen)
{
    int s = 0;
    unsigned cmf_low = ec_decode(_this, cmf[cmfLen - 1] - cmf[0]) + cmf[0];
    for ( ; s < cmfLen - 1; s++) {
        if (cmf_low < cmf[s + 1]) {
            break;
        }
    }
    ec_dec_update(_this, cmf[s] - cmf[0], cmf[s + 1] - cmf[0], cmf[cmfLen - 1] - cmf[0]);
    return s;
}

