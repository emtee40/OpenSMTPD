/*	$OpenBSD: ca.c,v 1.5 2014/04/30 08:23:42 reyk Exp $	*/

/*
 * Copyright (c) 2014 Reyk Floeter <reyk@openbsd.org>
 * Copyright (c) 2012 Gilles Chehade <gilles@poolp.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/socket.h>

#include <string.h>
#include <stdlib.h>
#include <imsg.h>

#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/engine.h>

#include "smtpd.h"
#include "log.h"
#include "ssl.h"

static int	 ca_verify_cb(int, X509_STORE_CTX *);

static int	 rsae_send_imsg(int, const u_char *, u_char *, RSA *,
		    int, u_int);
static int	 rsae_pub_enc(int, const u_char *, u_char *, RSA *, int);
static int	 rsae_pub_dec(int,const u_char *, u_char *, RSA *, int);
static int	 rsae_priv_enc(int, const u_char *, u_char *, RSA *, int);
static int	 rsae_priv_dec(int, const u_char *, u_char *, RSA *, int);
static int	 rsae_mod_exp(BIGNUM *, const BIGNUM *, RSA *, BN_CTX *);
static int	 rsae_bn_mod_exp(BIGNUM *, const BIGNUM *, const BIGNUM *,
		    const BIGNUM *, BN_CTX *, BN_MONT_CTX *);
static int	 rsae_init(RSA *);
static int	 rsae_finish(RSA *);
static int	 rsae_sign(int, const u_char *, u_int, u_char *, u_int *,
		    const RSA *);
static int	 rsae_verify(int dtype, const u_char *m, u_int, const u_char *,
		    u_int, const RSA *);
static int	 rsae_keygen(RSA *, int, BIGNUM *, BN_GENCB *);

static uint64_t	 rsae_reqid = 0;

void
ca_init(void)
{
	BIO		*in = NULL;
	EVP_PKEY	*pkey = NULL;
	struct pki	*pki;
	const char	*k;
	void		*iter_dict;

	log_debug("debug: init private ssl-tree");
	iter_dict = NULL;
	while (dict_iter(env->sc_pki_dict, &iter_dict, &k, (void **)&pki)) {
		if (pki->pki_key == NULL)
			continue;

		if ((in = BIO_new_mem_buf(pki->pki_key,
		    pki->pki_key_len)) == NULL)
			fatalx("ca_launch: key");

		if ((pkey = PEM_read_bio_PrivateKey(in,
		    NULL, NULL, NULL)) == NULL)
			fatalx("ca_launch: PEM");
		BIO_free(in);

		pki->pki_pkey = pkey;

		explicit_bzero(pki->pki_key, pki->pki_key_len);
		free(pki->pki_key);
		pki->pki_key = NULL;
	}
}

static int
ca_verify_cb(int ok, X509_STORE_CTX *ctx)
{
	switch (X509_STORE_CTX_get_error(ctx)) {
	case X509_V_OK:
		break;
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
		log_warnx("warn: unable to get issuer cert");
		break;
        case X509_V_ERR_CERT_NOT_YET_VALID:
        case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
		log_warnx("warn: certificate not yet valid");
		break;
        case X509_V_ERR_CERT_HAS_EXPIRED:
        case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
		log_warnx("warn: certificate has expired");
		break;
        case X509_V_ERR_NO_EXPLICIT_POLICY:
		log_warnx("warn: no explicit policy");
		break;
	}
	return ok;
}

int
ca_X509_verify(void *certificate, void *chain, const char *CAfile,
    const char *CRLfile, const char **errstr)
{
	X509_STORE     *store = NULL;
	X509_STORE_CTX *xsc = NULL;
	int		ret = 0;

	if ((store = X509_STORE_new()) == NULL)
		goto end;

	if (! X509_STORE_load_locations(store, CAfile, NULL)) {
		log_warn("warn: unable to load CA file %s", CAfile);
		goto end;
	}
	X509_STORE_set_default_paths(store);

	if ((xsc = X509_STORE_CTX_new()) == NULL)
		goto end;

	if (X509_STORE_CTX_init(xsc, store, certificate, chain) != 1)
		goto end;

	X509_STORE_CTX_set_verify_cb(xsc, ca_verify_cb);

	ret = X509_verify_cert(xsc);

end:
	*errstr = NULL;
	if (ret != 1) {
		if (xsc)
			*errstr = X509_verify_cert_error_string(xsc->error);
		else if (ERR_peek_last_error())
			*errstr = ERR_error_string(ERR_peek_last_error(), NULL);
	}

	if (xsc)
		X509_STORE_CTX_free(xsc);
	if (store)
		X509_STORE_free(store);

	return ret > 0 ? 1 : 0;
}

void
ca_imsg(struct mproc *p, struct imsg *imsg)
{
	RSA			*rsa;
	const void		*from = NULL;
	u_char			 *to = NULL;
	struct msg		 m;
	const char		*pkiname;
	size_t			 flen, tlen, padding;
	struct pki		*pki;
	int			 ret = 0;
	uint64_t		 id;

	m_msg(&m, imsg);
	m_get_id(&m, &id);
	m_get_string(&m, &pkiname);
	m_get_data(&m, &from, &flen);
	m_get_size(&m, &tlen);
	m_get_size(&m, &padding);
	m_end(&m);

	pki = dict_get(env->sc_pki_dict, pkiname);
	if (pki == NULL || pki->pki_pkey == NULL ||
	    (rsa = EVP_PKEY_get1_RSA(pki->pki_pkey)) == NULL)
		fatalx("ca_imsg: invalid pki");

	if ((to = calloc(1, tlen)) == NULL)
		fatalx("ca_imsg: calloc");

	switch (imsg->hdr.type) {
	case IMSG_CA_PRIVENC:
		ret = RSA_private_encrypt(flen, from, to, rsa,
		    padding);
		break;
	case IMSG_CA_PRIVDEC:
		ret = RSA_private_decrypt(flen, from, to, rsa,
		    padding);
		break;
	}

	m_create(p, imsg->hdr.type, 0, 0, -1);
	m_add_id(p, id);
	m_add_int(p, ret);
	if (ret > 0)
		m_add_data(p, to, (size_t)ret);
	m_close(p);

	free(to);
	RSA_free(rsa);
}

/*
 * RSA privsep engine (called from unprivileged processes)
 */

const RSA_METHOD *rsa_default = NULL;

static RSA_METHOD rsae_method = {
	"RSA privsep engine",
	rsae_pub_enc,
	rsae_pub_dec,
	rsae_priv_enc,
	rsae_priv_dec,
	rsae_mod_exp,
	rsae_bn_mod_exp,
	rsae_init,
	rsae_finish,
	0,
	NULL,
	rsae_sign,
	rsae_verify,
	rsae_keygen
};

static int
rsae_send_imsg(int flen, const u_char *from, u_char *to, RSA *rsa,
    int padding, u_int cmd)
{
	int		 ret = 0;
	struct imsgbuf	*ibuf;
	struct imsg	 imsg;
	int		 n, done = 0;
	const void	*toptr;
	char		*pkiname;
	size_t		 tlen;
	struct msg	 m;
	uint64_t	 id;

	if ((pkiname = RSA_get_ex_data(rsa, 0)) == NULL)
		return (0);

	/*
	 * Send a synchronous imsg because we cannot defer the RSA
	 * operation in OpenSSL's engine layer.
	 */
	m_create(p_lka, cmd, 0, 0, -1);
	rsae_reqid++;
	m_add_id(p_lka, rsae_reqid);
	m_add_string(p_lka, pkiname);
	m_add_data(p_lka, (const void *)from, (size_t)flen);
	m_add_size(p_lka, (size_t)RSA_size(rsa));
	m_add_size(p_lka, (size_t)padding);
	m_flush(p_lka);

	ibuf = &p_lka->imsgbuf;

	while (!done) {
		if ((n = imsg_read(ibuf)) == -1)
			fatalx("imsg_read");
		if (n == 0)
			fatalx("pipe closed");

		while (!done) {
			if ((n = imsg_get(ibuf, &imsg)) == -1)
				fatalx("imsg_get error");
			if (n == 0)
				break;

			log_imsg(PROC_PONY, PROC_LKA, &imsg);

			switch (imsg.hdr.type) {
			case IMSG_CA_PRIVENC:
			case IMSG_CA_PRIVDEC:
				break;
			default:
				/* Another imsg is queued up in the buffer */
				pony_imsg(p_lka, &imsg);
				imsg_free(&imsg);
				continue;
			}

			m_msg(&m, &imsg);
			m_get_id(&m, &id);
			if (id != rsae_reqid)
				fatalx("invalid response id");
			m_get_int(&m, &ret);
			if (ret > 0)
				m_get_data(&m, &toptr, &tlen);
			m_end(&m);

			if (ret > 0)
				memcpy(to, toptr, tlen);
			done = 1;

			imsg_free(&imsg);
		}
	}
	mproc_event_add(p_lka);

	return (ret);
}

static int
rsae_pub_enc(int flen,const u_char *from, u_char *to, RSA *rsa,int padding)
{
	log_debug("debug: %s: %s", proc_name(smtpd_process), __func__);
	return (rsa_default->rsa_pub_enc(flen, from, to, rsa, padding));
}

static int
rsae_pub_dec(int flen,const u_char *from, u_char *to, RSA *rsa,int padding)
{
	log_debug("debug: %s: %s", proc_name(smtpd_process), __func__);
	return (rsa_default->rsa_pub_dec(flen, from, to, rsa, padding));
}

static int
rsae_priv_enc(int flen, const u_char *from, u_char *to, RSA *rsa, int padding)
{
	log_debug("debug: %s: %s", proc_name(smtpd_process), __func__);
	if (RSA_get_ex_data(rsa, 0) != NULL) {
		return (rsae_send_imsg(flen, from, to, rsa, padding,
		    IMSG_CA_PRIVENC));
	}
	return (rsa_default->rsa_priv_enc(flen, from, to, rsa, padding));
}

static int
rsae_priv_dec(int flen, const u_char *from, u_char *to, RSA *rsa, int padding)
{
	log_debug("debug: %s: %s", proc_name(smtpd_process), __func__);
	if (RSA_get_ex_data(rsa, 0) != NULL) {
		return (rsae_send_imsg(flen, from, to, rsa, padding,
		    IMSG_CA_PRIVDEC));
	}
	return (rsa_default->rsa_priv_dec(flen, from, to, rsa, padding));
}

static int
rsae_mod_exp(BIGNUM *r0, const BIGNUM *I, RSA *rsa, BN_CTX *ctx)
{
	log_debug("debug: %s: %s", proc_name(smtpd_process), __func__);
	return (rsa_default->rsa_mod_exp(r0, I, rsa, ctx));
}

static int
rsae_bn_mod_exp(BIGNUM *r, const BIGNUM *a, const BIGNUM *p,
    const BIGNUM *m, BN_CTX *ctx, BN_MONT_CTX *m_ctx)
{
	log_debug("debug: %s: %s", proc_name(smtpd_process), __func__);
	return (rsa_default->bn_mod_exp(r, a, p, m, ctx, m_ctx));
}

static int
rsae_init(RSA *rsa)
{
	log_debug("debug: %s: %s", proc_name(smtpd_process), __func__);
	if (rsa_default->init == NULL)
		return (1);
	return (rsa_default->init(rsa));
}

static int
rsae_finish(RSA *rsa)
{
	log_debug("debug: %s: %s", proc_name(smtpd_process), __func__);
	if (rsa_default->finish == NULL)
		return (1);
	return (rsa_default->finish(rsa));
}

static int
rsae_sign(int type, const u_char *m, u_int m_length, u_char *sigret,
    u_int *siglen, const RSA *rsa)
{
	log_debug("debug: %s: %s", proc_name(smtpd_process), __func__);
	return (rsa_default->rsa_sign(type, m, m_length,
	    sigret, siglen, rsa));
}

static int
rsae_verify(int dtype, const u_char *m, u_int m_length, const u_char *sigbuf,
    u_int siglen, const RSA *rsa)
{
	log_debug("debug: %s: %s", proc_name(smtpd_process), __func__);
	return (rsa_default->rsa_verify(dtype, m, m_length,
	    sigbuf, siglen, rsa));
}

static int
rsae_keygen(RSA *rsa, int bits, BIGNUM *e, BN_GENCB *cb)
{
	log_debug("debug: %s: %s", proc_name(smtpd_process), __func__);
	return (rsa_default->rsa_keygen(rsa, bits, e, cb));
}

int
ca_engine_init(void)
{
	ENGINE	*e;

	log_debug("debug: %s: %s", proc_name(smtpd_process), __func__);

	if ((e = ENGINE_get_default_RSA()) == NULL ||
	    (rsa_default = ENGINE_get_RSA(e)) == NULL)
		return (-1);

	if (rsa_default->flags & RSA_FLAG_SIGN_VER)
		fatalx("unsupported RSA engine");

	if (rsa_default->rsa_mod_exp == NULL)
		rsae_method.rsa_mod_exp = NULL;
	if (rsa_default->rsa_mod_exp == NULL)
		rsae_method.rsa_mod_exp = NULL;
	if (rsa_default->bn_mod_exp == NULL)
		rsae_method.bn_mod_exp = NULL;
	if (rsa_default->rsa_keygen == NULL)
		rsae_method.rsa_keygen = NULL;
	rsae_method.flags = rsa_default->flags |
	    RSA_METHOD_FLAG_NO_CHECK;
	rsae_method.app_data = rsa_default->app_data;

	if (!ENGINE_set_RSA(e, &rsae_method) ||
	    !ENGINE_set_default_RSA(e))
		return (-1);

	return (0);
}