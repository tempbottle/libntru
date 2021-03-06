#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "ntru.h"
#include "rand.h"
#include "poly.h"
#include "idxgen.h"
#include "mgf.h"

/** Whether to ensure g is invertible when generating a key */
#define NTRU_CHECK_INVERTIBILITY_G 0

const int8_t NTRU_COEFF1_TABLE[] = {0, 0, 0, 1, 1, 1, -1, -1};
const int8_t NTRU_COEFF2_TABLE[] = {0, 1, -1, 0, 1, -1, 0, 1};
const uint8_t NTRU_BIT1_TABLE[] = {1, 1, 1, 0, 0, 0, 1, 0, 1};
const uint8_t NTRU_BIT2_TABLE[] = {1, 1, 1, 1, 0, 0, 0, 1, 0};
const uint8_t NTRU_BIT3_TABLE[] = {1, 0, 1, 0, 0, 1, 1, 1, 0};

uint8_t ntru_gen_key_pair(NtruEncParams *params, NtruEncKeyPair *kp, NtruRandContext *rand_ctx) {
    uint16_t N = params->N;
    uint16_t q = params->q;
    uint16_t df1 = params->df1;
#ifndef NTRU_AVOID_HAMMING_WT_PATENT
    uint16_t df2 = params->df2;
    uint16_t df3 = params->df3;
#endif   /* NTRU_AVOID_HAMMING_WT_PATENT */

    NtruIntPoly fq;

    /* choose a random f that is invertible mod q */
#ifndef NTRU_AVOID_HAMMING_WT_PATENT
    if (params->prod_flag) {
        NtruPrivPoly *t = &kp->priv.t;
        t->prod_flag = 1;
        t->poly.prod.N = N;
        kp->priv.q = q;
        for (;;) {
            /* choose random t, find the inverse of 3t+1 */
            if (!ntru_rand_prod(N, df1, df2, df3, df3, &t->poly.prod, rand_ctx))
                return NTRU_ERR_PRNG;
            if (ntru_invert(t, q, &fq))
                break;
        }
    }
    else
#endif   /* NTRU_AVOID_HAMMING_WT_PATENT */
    {
        NtruPrivPoly *t = &kp->priv.t;
        t->prod_flag = 0;
        kp->priv.q = q;
        for (;;) {
            /* choose random t, find the inverse of 3t+1 */
            if (!ntru_rand_tern(N, df1, df1, &t->poly.tern, rand_ctx))
                return NTRU_ERR_PRNG;
            if (ntru_invert(t, q, &fq))
                break;
        }
    }

    /* choose a random g that is invertible mod q */
    NtruPrivPoly g;
    uint16_t dg = N / 3;
    for (;;) {
#ifndef NTRU_AVOID_HAMMING_WT_PATENT
        if (params->prod_flag && !ntru_rand_prod(N, df1, df2, df3, df3, &g.poly.prod, rand_ctx))
            return NTRU_ERR_PRNG;
        if (!params->prod_flag && !ntru_rand_tern(N, dg, dg, &g.poly.tern, rand_ctx))
            return NTRU_ERR_PRNG;
        g.prod_flag = params->prod_flag;
#else
        if (!ntru_rand_tern(N, dg, dg, &g.poly.tern, rand_ctx))
            return NTRU_ERR_PRNG;
        g.prod_flag = 0;
#endif   /* NTRU_AVOID_HAMMING_WT_PATENT */

        if (!NTRU_CHECK_INVERTIBILITY_G)
            break;
        NtruIntPoly gq;
        if (ntru_invert(&g, q, &gq))
            break;
    }

    NtruIntPoly *h = &kp->pub.h;
    if (!ntru_mult_priv(&g, &fq, h, q))
        return NTRU_ERR_PRNG;
    ntru_mult_fac(h, 3);
    ntru_mod(h, q);

    ntru_clear_priv(&g);
    ntru_clear_int(&fq);

    kp->pub.q = q;

    return NTRU_SUCCESS;
}

/**
 * @brief byte array to ternary polynomial
 *
 * Decodes a uint8_t array encoded with ntru_to_sves() back to a polynomial with N
 * coefficients between -1 and 1.
 * Ignores any excess bytes.
 * See P1363.1 section 9.2.2.
 *
 * @param M an encoded ternary polynomial
 * @param M_len number of elements in M
 * @param N number of coefficients to generate
 * @param skip whether to leave the constant coefficient zero and start populating at the linear coefficient
 * @param poly output parameter; pointer to write the polynomial to
 */
void ntru_from_sves(uint8_t *M, uint16_t M_len, uint16_t N, uint16_t skip, NtruIntPoly *poly) {
    poly->N = N;

    uint16_t coeff_idx = skip ? 1 : 0;
    uint16_t i = 0;
    while (i<M_len/3*3 && coeff_idx<N-1) {
        /* process 24 bits at a time in the outer loop */
        int32_t chunk = (uint8_t)M[i+2];
        chunk <<= 8;
        chunk += (uint8_t)M[i+1];
        chunk <<= 8;
        chunk += (uint8_t)M[i];
        i += 3;

        uint8_t j;
        for (j=0; j<8 && coeff_idx<N-1; j++) {
            /* process 3 bits at a time in the inner loop */
            uint8_t coeff_tbl_idx = ((chunk&1)<<2) + (chunk&2) + ((chunk&4)>>2);   /* low 3 bits in reverse order */
            poly->coeffs[coeff_idx++] = NTRU_COEFF1_TABLE[coeff_tbl_idx];
            poly->coeffs[coeff_idx++] = NTRU_COEFF2_TABLE[coeff_tbl_idx];
            chunk >>= 3;
        }
    }

    while (coeff_idx < N)
        poly->coeffs[coeff_idx++] = 0;
}

/**
 * @brief Ternary polynomial to byte array
 *
 * Encodes a polynomial whose elements are between -1 and 1, to a uint8_t array.
 * The (2*i)-th coefficient and the (2*i+1)-th coefficient must not both equal
 * -1 for any integer i, so this method is only safe to use with arrays
 * produced by ntru_from_sves().
 * See P1363.1 section 9.2.3.
 *
 * @param poly a ternary polynomial
 * @param skip whether to skip the constant coefficient
 * @param data output parameter; must accommodate ceil(num_bits/8) bytes
 * @return NTRU_SUCCESS for success, NTRU_ERR_INVALID_ENCODING otherwise
 */
uint8_t ntru_to_sves(NtruIntPoly *poly, uint16_t skip, uint8_t *data) {
    uint16_t N = poly->N;

    uint16_t num_bits = (N*3+1) / 2;
    memset(data, 0, (num_bits+7)/8);

    uint8_t bit_index = 0;
    uint16_t byte_index = 0;
    uint16_t i;
    uint16_t start = skip ? 1 : 0;
    uint16_t end = skip ? (N-1)|1 : N/2*2;   /* if there is an odd number of coeffs, throw away the highest one */
    for (i=start; i<end; ) {
        int16_t coeff1 = poly->coeffs[i++] + 1;
        int16_t coeff2 = poly->coeffs[i++] + 1;
        if (coeff1==0 && coeff2==0)
            return NTRU_ERR_INVALID_ENCODING;
        int16_t bit_tbl_index = coeff1*3 + coeff2;
        uint8_t j;
        uint8_t bits[] = {NTRU_BIT1_TABLE[bit_tbl_index], NTRU_BIT2_TABLE[bit_tbl_index], NTRU_BIT3_TABLE[bit_tbl_index]};
        for (j=0; j<3; j++) {
            data[byte_index] |= bits[j] << bit_index;
            if (bit_index == 7) {
                bit_index = 0;
                byte_index++;
            }
            else
                bit_index++;
        }
    }

    return NTRU_SUCCESS;
}

/**
 * @brief Seed generation
 *
 * Generates a seed for the Blinding Polynomial Generation Function.
 *
 * @param msg the plain-text message
 * @param msg_len number of characters in msg
 * @param h the public key
 * @param b db bits of random data
 * @param params encryption parameters
 * @param seed output parameter; an array to write the seed value to
 */
void ntru_get_seed(uint8_t *msg, uint16_t msg_len, NtruIntPoly *h, uint8_t *b, NtruEncParams *params, uint8_t *seed) {
    uint16_t oid_len = sizeof params->oid;
    uint16_t pklen = params->pklen;

    uint8_t bh[ntru_enc_len(params)];
    ntru_to_arr(h, params->q, (uint8_t*)&bh);
    uint8_t htrunc[pklen/8];
    memcpy(&htrunc, &bh, pklen/8);

    /* seed = OID|m|b|htrunc */
    uint16_t blen = params->db/8;
    memcpy(seed, &params->oid, oid_len);
    seed += oid_len;
    memcpy(seed, msg, msg_len);
    seed += msg_len;
    memcpy(seed, b, blen);
    seed += blen;
    memcpy(seed, &htrunc, pklen/8);
}

void ntru_gen_tern_poly(NtruIGFState *s, uint16_t df, NtruTernPoly *p) {
    p->N = s->N;
    p->num_ones = df;
    p->num_neg_ones = df;

    uint16_t idx;
    uint16_t r[p->N];
    memset(r, 0, sizeof r);

    uint16_t t = 0;
    while (t < df) {
        ntru_IGF_next(s, &idx);
        if (!r[idx]) {
            p->neg_ones[t] = idx;
            r[idx] = 1;
            t++;
        }
    }
    t = 0;
    while (t < df) {
        ntru_IGF_next(s, &idx);
        if (!r[idx]) {
            p->ones[t] = idx;
            r[idx] = 1;
            t++;
        }
    }
}

void ntru_gen_blind_poly(uint8_t *seed, uint16_t seed_len, NtruEncParams *params, NtruPrivPoly *r) {
    NtruIGFState s;
    ntru_IGF_init(seed, seed_len, params, &s);

#ifndef NTRU_AVOID_HAMMING_WT_PATENT
    if (params->prod_flag) {
        r->poly.prod.N = s.N;
        ntru_gen_tern_poly(&s, params->df1, &r->poly.prod.f1);
        ntru_gen_tern_poly(&s, params->df2, &r->poly.prod.f2);
        ntru_gen_tern_poly(&s, params->df3, &r->poly.prod.f3);
    }
    else
#endif   /* NTRU_AVOID_HAMMING_WT_PATENT */
    {
        r->poly.tern.N = s.N;
        ntru_gen_tern_poly(&s, params->df1, &r->poly.tern);
    }
}

uint8_t ntru_check_rep_weight(NtruIntPoly *p, uint16_t dm0) {
    uint16_t i;
    uint16_t weights[3];
    weights[0] = weights[1] = weights[2] = 0;

    for (i=0; i<p->N; i++)
        weights[p->coeffs[i]+1]++;

    return (weights[0]>=dm0 && weights[1]>=dm0 && weights[2]>=dm0);
}

uint8_t ntru_encrypt(uint8_t *msg, uint16_t msg_len, NtruEncPubKey *pub, NtruEncParams *params, NtruRandContext *rand_ctx, uint8_t *enc) {
    uint16_t N = params->N;
    uint16_t q = params->q;
    uint16_t maxm1 = params->maxm1;
    uint16_t db = params->db;
    uint16_t max_len_bytes = ntru_max_msg_len(params);
    uint16_t buf_len_bits = (N*3/2+7)/8*8 + 1;
    uint16_t dm0 = params->dm0;

    if (max_len_bytes > 255)
        return NTRU_ERR_INVALID_MAX_LEN;
    if (msg_len > max_len_bytes)
        return NTRU_ERR_MSG_TOO_LONG;

    for (;;) {
        /* M = b|octL|msg|p0 */
        uint8_t b[db/8];
        if (!rand_ctx->rand_gen->generate(b, db/8, rand_ctx))
            return NTRU_ERR_PRNG;

        uint16_t M_len = (buf_len_bits+7) / 8;
        uint8_t M[M_len];
        memcpy(&M, &b, db/8);
        uint8_t *M_head = (uint8_t*)&M + db/8;
        *M_head = msg_len;
        M_head++;
        memcpy(M_head, msg, msg_len);
        M_head += msg_len;
        memset(M_head, 0, max_len_bytes+1-msg_len);

        NtruIntPoly mtrin;
        ntru_from_sves((uint8_t*)&M, M_len, N, maxm1, &mtrin);

        uint16_t blen = params->db / 8;
        uint16_t sdata_len = sizeof(params->oid) + msg_len + blen + blen;
        uint8_t sdata[sdata_len];
        ntru_get_seed(msg, msg_len, &pub->h, (uint8_t*)&b, params, (uint8_t*)&sdata);

        NtruIntPoly R;
        NtruPrivPoly r;
        ntru_gen_blind_poly((uint8_t*)&sdata, sdata_len, params, &r);
#ifndef NTRU_AVOID_HAMMING_WT_PATENT
        if (params->prod_flag)
            ntru_mult_prod(&pub->h, &r.poly.prod, &R, q);
        else
#endif   /* NTRU_AVOID_HAMMING_WT_PATENT */
            ntru_mult_tern(&pub->h, &r.poly.tern, &R, q);
        uint16_t oR4_len = (N*2+7) / 8;
        uint8_t oR4[oR4_len];
        ntru_to_arr4(&R, (uint8_t*)&oR4);
        NtruIntPoly mask;
        ntru_MGF((uint8_t*)&oR4, oR4_len, params, &mask);
        ntru_add_int(&mtrin, &mask);

        /*
         * If df and dr are close to N/3, and the absolute value of ntru_sum_coeffs(mtrin) is
         * large enough, the message becomes vulnerable to a meet-in-the-middle attack.
         * To prevent this, we set the constant coefficient to zero but first check to ensure
         * ntru_sum_coeffs() is small enough to keep the likelihood of a decryption failure low.
         */
        if (maxm1 > 0) {
            if (ntru_sum_coeffs(&mtrin) > maxm1)
                continue;
            mtrin.coeffs[0] = 0;
        }

        ntru_mod3(&mtrin);

        if (dm0>0 && !ntru_check_rep_weight(&mtrin, dm0))
            continue;

        ntru_add_int_mod(&R, &mtrin, q);
        ntru_to_arr(&R, q, enc);
        return NTRU_SUCCESS;
    }
}

void ntru_decrypt_poly(NtruIntPoly *e, NtruEncPrivKey *priv, uint16_t q, NtruIntPoly *d) {
#ifndef NTRU_AVOID_HAMMING_WT_PATENT
    if (priv->t.prod_flag)
        ntru_mult_prod(e, &priv->t.poly.prod, d, q);
    else
#endif   /* NTRU_AVOID_HAMMING_WT_PATENT */
        ntru_mult_tern(e, &priv->t.poly.tern, d, q);
    ntru_mult_fac(d, 3);
    ntru_add_int(d, e);
    ntru_mod_center(d, q);
    ntru_mod3(d);
}

uint8_t ntru_decrypt(uint8_t *enc, NtruEncKeyPair *kp, NtruEncParams *params, uint8_t *dec, uint16_t *dec_len) {
    uint16_t N = params->N;
    uint16_t q = params->q;
    uint16_t db = params->db;
    uint16_t maxm1 = params->maxm1;
    uint16_t max_len_bytes = ntru_max_msg_len(params);
    uint16_t dm0 = params->dm0;

    if (max_len_bytes > 255)
        return NTRU_ERR_INVALID_MAX_LEN;

    uint16_t blen = db / 8;

    NtruIntPoly e;
    ntru_from_arr(enc, N, q, &e);
    NtruIntPoly ci;
    ntru_decrypt_poly(&e, &kp->priv, q, &ci);

    if (dm0>0 && !ntru_check_rep_weight(&ci, dm0))
        return NTRU_ERR_DM0_VIOLATION;

    NtruIntPoly cR = e;
    ntru_sub_int(&cR, &ci);
    ntru_mod(&cR, q);

    uint16_t coR4_len = (N*2+7) / 8;
    uint8_t coR4[coR4_len];
    ntru_to_arr4(&cR, (uint8_t*)&coR4);

    NtruIntPoly mask;
    ntru_MGF((uint8_t*)&coR4, coR4_len, params, &mask);
    NtruIntPoly cmtrin = ci;
    ntru_sub_int(&cmtrin, &mask);
    ntru_mod3(&cmtrin);
    uint16_t cM_len_bits = (N*3+1) / 2;
    uint16_t cM_len_bytes = (cM_len_bits+7) / 8;
    uint8_t cM[cM_len_bytes];
    ntru_to_sves(&cmtrin, maxm1, (uint8_t*)&cM);

    uint8_t cb[blen];
    uint8_t *cM_head = cM;
    memcpy(cb, cM_head, blen);
    cM_head += blen;
    uint8_t cl = *cM_head;   /* llen=1, so read one byte */
    cM_head++;
    if (cl > max_len_bytes)
        return NTRU_ERR_MSG_TOO_LONG;

    memcpy(dec, cM_head, cl);
    cM_head += cl;

    uint8_t *i;
    for (i=cM_head; i<cM+cM_len_bytes; i++)
        if (*i)
            return NTRU_ERR_NO_ZERO_PAD;

    uint16_t sdata_len = sizeof(params->oid) + cl + blen + db/8;
    uint8_t sdata[sdata_len];
    ntru_get_seed(dec, cl, &kp->pub.h, (uint8_t*)&cb, params, (uint8_t*)&sdata);

    NtruPrivPoly cr;
    ntru_gen_blind_poly((uint8_t*)&sdata, sdata_len, params, &cr);
    NtruIntPoly cR_prime;
#ifndef NTRU_AVOID_HAMMING_WT_PATENT
    if (params->prod_flag)
        ntru_mult_prod(&kp->pub.h, &cr.poly.prod, &cR_prime, q);
    else
#endif   /* NTRU_AVOID_HAMMING_WT_PATENT */
        ntru_mult_tern(&kp->pub.h, &cr.poly.tern, &cR_prime, q);
    if (!ntru_equals_int(&cR_prime, &cR))
        return NTRU_ERR_INVALID_ENCODING;

    *dec_len = cl;
    return NTRU_SUCCESS;
}

uint8_t ntru_max_msg_len(NtruEncParams *params) {
    uint16_t N = params->N;
    uint8_t llen = 1;   /* ceil(log2(max_len)) */
    uint16_t db = params->db;
    uint16_t max_msg_len;
    if (params->maxm1 > 0)
        max_msg_len = (N-1)*3/2/8 - llen - db/8;   /* only N-1 coeffs b/c the constant coeff is not used */
    else
        max_msg_len = N*3/2/8 - llen - db/8;
    return max_msg_len;
}
