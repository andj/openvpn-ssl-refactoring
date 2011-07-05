/*
 *  OpenVPN -- An application to securely tunnel IP networks
 *             over a single TCP/UDP port, with support for SSL/TLS-based
 *             session authentication and key exchange,
 *             packet encryption, packet authentication, and
 *             packet compression.
 *
 *  Copyright (C) 2002-2010 OpenVPN Technologies, Inc. <sales@openvpn.net>
 *  Copyright (C) 2010 Fox Crypto B.V. <openvpn@fox-it.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file Data Channel Cryptography OpenSSL-specific backend interface
 */

#include "syshead.h"

#include "basic.h"
#include "buffer.h"
#include "integer.h"
#include "crypto_backend.h"
#include <openssl/objects.h>
#include <openssl/evp.h>
#include <openssl/des.h>

/*
 * Check for key size creepage.
 */

#if MAX_CIPHER_KEY_LENGTH < EVP_MAX_KEY_LENGTH
#warning Some OpenSSL EVP ciphers now support key lengths greater than MAX_CIPHER_KEY_LENGTH -- consider increasing MAX_CIPHER_KEY_LENGTH
#endif

#if MAX_HMAC_KEY_LENGTH < EVP_MAX_MD_SIZE
#warning Some OpenSSL HMAC message digests now support key lengths greater than MAX_HMAC_KEY_LENGTH -- consider increasing MAX_HMAC_KEY_LENGTH
#endif

/*
 *
 * Workarounds for incompatibilites between OpenSSL libraries.
 * Right now we accept OpenSSL libraries from 0.9.5 to 0.9.7.
 *
 */

#if SSLEAY_VERSION_NUMBER < 0x00907000L

#define DES_cblock                        des_cblock
#define DES_is_weak_key                   des_is_weak_key
#define DES_check_key_parity              des_check_key_parity
#define DES_set_odd_parity                des_set_odd_parity

#define HMAC_CTX_init(ctx)                CLEAR (*ctx)
#define HMAC_Init_ex(ctx,sec,len,md,impl) HMAC_Init(ctx, sec, len, md)
#define HMAC_CTX_cleanup(ctx)             HMAC_cleanup(ctx)
#define EVP_MD_CTX_cleanup(md)            CLEAR (*md)

#define INFO_CALLBACK_SSL_CONST

#endif

#if SSLEAY_VERSION_NUMBER < 0x00906000

static inline bool
cipher_ok (const char* name)
{
  const int i = strlen (name) - 4;
  if (i >= 0)
    return !strcmp (name + i, "-CBC");
  else
    return false;
}

#else

static inline bool
cipher_ok (const char* name)
{
  return true;
}

#endif

#if SSLEAY_VERSION_NUMBER < 0x0090581f

#endif /* SSLEAY_VERSION_NUMBER < 0x0090581f */

#ifndef EVP_MD_name
#define EVP_MD_name(e)			OBJ_nid2sn(EVP_MD_type(e))
#endif

/*
 *
 * OpenSSL engine support. Allows loading/unloading of engines.
 *
 */

#if defined(HAVE_OPENSSL_ENGINE_H) && defined(HAVE_ENGINE_LOAD_BUILTIN_ENGINES) && defined(HAVE_ENGINE_REGISTER_ALL_COMPLETE) && defined(HAVE_ENGINE_CLEANUP)
#define CRYPTO_ENGINE 1
#else
#define CRYPTO_ENGINE 0
#endif

#if CRYPTO_ENGINE
#include <openssl/engine.h>

static bool engine_initialized = false; /* GLOBAL */

static ENGINE *engine_persist = NULL;   /* GLOBAL */

/* Try to load an engine in a shareable library */
static ENGINE *
try_load_engine (const char *engine)
{
  ENGINE *e = ENGINE_by_id ("dynamic");
  if (e)
    {
      if (!ENGINE_ctrl_cmd_string (e, "SO_PATH", engine, 0)
	  || !ENGINE_ctrl_cmd_string (e, "LOAD", NULL, 0))
	{
	  ENGINE_free (e);
	  e = NULL;
	}
    }
  return e;
}

static ENGINE *
setup_engine (const char *engine)
{
  ENGINE *e = NULL;

  ENGINE_load_builtin_engines ();

  if (engine)
    {
      if (strcmp (engine, "auto") == 0)
	{
	  msg (M_INFO, "Initializing OpenSSL auto engine support");
	  ENGINE_register_all_complete ();
	  return NULL;
	}
      if ((e = ENGINE_by_id (engine)) == NULL
	 && (e = try_load_engine (engine)) == NULL)
	{
	  msg (M_FATAL, "OpenSSL error: cannot load engine '%s'", engine);
	}

      if (!ENGINE_set_default (e, ENGINE_METHOD_ALL))
	{
	  msg (M_FATAL, "OpenSSL error: ENGINE_set_default failed on engine '%s'",
	       engine);
	}

      msg (M_INFO, "Initializing OpenSSL support for engine '%s'",
	   ENGINE_get_id (e));
    }
  return e;
}

#endif /* CRYPTO_ENGINE */

void
crypto_init_lib_engine (const char *engine_name)
{
#if CRYPTO_ENGINE
  if (!engine_initialized)
    {
      ASSERT (engine_name);
      ASSERT (!engine_persist);
      engine_persist = setup_engine (engine_name);
      engine_initialized = true;
    }
#else
  msg (M_WARN, "Note: OpenSSL hardware crypto engine functionality is not available");
#endif
}

/*
 *
 * Functions related to the core crypto library
 *
 */

void
crypto_init_lib (void)
{
  /*
   * If you build the OpenSSL library and OpenVPN with
   * CRYPTO_MDEBUG, you will get a listing of OpenSSL
   * memory leaks on program termination.
   */
#ifdef CRYPTO_MDEBUG
  CRYPTO_mem_ctrl(CRYPTO_MEM_CHECK_ON);
#endif
}

void
crypto_uninit_lib (void)
{
#ifdef CRYPTO_MDEBUG
  FILE* fp = fopen ("sdlog", "w");
  ASSERT (fp);
  CRYPTO_mem_leaks_fp (fp);
  fclose (fp);
#endif

#if CRYPTO_ENGINE
  if (engine_initialized)
    {
      ENGINE_cleanup ();
      engine_persist = NULL;
      engine_initialized = false;
    }
#endif

  prng_uninit ();
}

void
crypto_clear_error (void)
{
  ERR_clear_error ();
}

/*
 *
 * OpenSSL memory debugging.  If dmalloc debugging is enabled, tell
 * OpenSSL to use our private malloc/realloc/free functions so that
 * we can dispatch them to dmalloc.
 *
 */

#ifdef DMALLOC
static void *
crypto_malloc (size_t size, const char *file, int line)
{
  return dmalloc_malloc(file, line, size, DMALLOC_FUNC_MALLOC, 0, 0);
}

static void *
crypto_realloc (void *ptr, size_t size, const char *file, int line)
{
  return dmalloc_realloc(file, line, ptr, size, DMALLOC_FUNC_REALLOC, 0);
}

static void
crypto_free (void *ptr)
{
  dmalloc_free (__FILE__, __LINE__, ptr, DMALLOC_FUNC_FREE);
}

void
crypto_init_dmalloc (void)
{
  CRYPTO_set_mem_ex_functions (crypto_malloc,
				crypto_realloc,
				crypto_free);
}
#endif /* DMALLOC */

void
show_available_ciphers ()
{
  int nid;

#ifndef ENABLE_SMALL
  printf ("The following ciphers and cipher modes are available\n"
	  "for use with " PACKAGE_NAME ".  Each cipher shown below may be\n"
	  "used as a parameter to the --cipher option.  The default\n"
	  "key size is shown as well as whether or not it can be\n"
          "changed with the --keysize directive.  Using a CBC mode\n"
	  "is recommended.\n\n");
#endif

  for (nid = 0; nid < 10000; ++nid)	/* is there a better way to get the size of the nid list? */
    {
      const EVP_CIPHER *cipher = EVP_get_cipherbynid (nid);
      if (cipher && cipher_ok (OBJ_nid2sn (nid)))
	{
	  const unsigned int mode = EVP_CIPHER_mode (cipher);
	  if (mode == EVP_CIPH_CBC_MODE
#ifdef ALLOW_NON_CBC_CIPHERS
	      || mode == EVP_CIPH_CFB_MODE || mode == EVP_CIPH_OFB_MODE
#endif
	      )
	    printf ("%s %d bit default key (%s)\n",
		    OBJ_nid2sn (nid),
		    EVP_CIPHER_key_length (cipher) * 8,
		    ((EVP_CIPHER_flags (cipher) & EVP_CIPH_VARIABLE_LENGTH) ?
		     "variable" : "fixed"));
	}
    }
  printf ("\n");
}

void
show_available_digests ()
{
  int nid;

#ifndef ENABLE_SMALL
  printf ("The following message digests are available for use with\n"
	  PACKAGE_NAME ".  A message digest is used in conjunction with\n"
	  "the HMAC function, to authenticate received packets.\n"
	  "You can specify a message digest as parameter to\n"
	  "the --auth option.\n\n");
#endif

  for (nid = 0; nid < 10000; ++nid)
    {
      const EVP_MD *digest = EVP_get_digestbynid (nid);
      if (digest)
	{
	  printf ("%s %d bit digest size\n",
		  OBJ_nid2sn (nid), EVP_MD_size (digest) * 8);
	}
    }
  printf ("\n");
}

void
show_available_engines ()
{
#if CRYPTO_ENGINE /* Only defined for OpenSSL */
  ENGINE *e;

  printf ("OpenSSL Crypto Engines\n\n");

  ENGINE_load_builtin_engines ();

  e = ENGINE_get_first ();
  while (e)
    {
      printf ("%s [%s]\n",
	      ENGINE_get_name (e),
	      ENGINE_get_id (e));
      e = ENGINE_get_next (e);
    }
  ENGINE_cleanup ();
#else
  printf ("Sorry, OpenSSL hardware crypto engine functionality is not available.\n");
#endif
}

/*
 *
 * Random number functions, used in cases where we want
 * reasonably strong cryptographic random number generation
 * without depleting our entropy pool.  Used for random
 * IV values and a number of other miscellaneous tasks.
 *
 */

int rand_bytes(uint8_t *output, int len)
{
  return RAND_bytes (output, len);
}

/*
 *
 * Key functions, allow manipulation of keys.
 *
 */


int
key_des_num_cblocks (const EVP_CIPHER *kt)
{
  int ret = 0;
  const char *name = OBJ_nid2sn (EVP_CIPHER_nid (kt));
  if (name)
    {
      if (!strncmp (name, "DES-", 4))
	{
	  ret = EVP_CIPHER_key_length (kt) / sizeof (DES_cblock);
	}
      else if (!strncmp (name, "DESX-", 5))
	{
	  ret = 1;
	}
    }
  dmsg (D_CRYPTO_DEBUG, "CRYPTO INFO: n_DES_cblocks=%d", ret);
  return ret;
}

bool
key_des_check (uint8_t *key, int key_len, int ndc)
{
  int i;
  struct buffer b;

  buf_set_read (&b, key, key_len);

  for (i = 0; i < ndc; ++i)
    {
      DES_cblock *dc = (DES_cblock*) buf_read_alloc (&b, sizeof (DES_cblock));
      if (!dc)
	{
	  msg (D_CRYPT_ERRORS, "CRYPTO INFO: check_key_DES: insufficient key material");
	  goto err;
	}
      if (DES_is_weak_key(dc))
	{
	  msg (D_CRYPT_ERRORS, "CRYPTO INFO: check_key_DES: weak key detected");
	  goto err;
	}
      if (!DES_check_key_parity (dc))
	{
	  msg (D_CRYPT_ERRORS, "CRYPTO INFO: check_key_DES: bad parity detected");
	  goto err;
	}
    }
  return true;

 err:
  ERR_clear_error ();
  return false;
}

void
key_des_fixup (uint8_t *key, int key_len, int ndc)
{
  int i;
  struct buffer b;

  buf_set_read (&b, key, key_len);
  for (i = 0; i < ndc; ++i)
    {
      DES_cblock *dc = (DES_cblock*) buf_read_alloc(&b, sizeof(DES_cblock));
      if (!dc)
	{
	  msg (D_CRYPT_ERRORS, "CRYPTO INFO: fixup_key_DES: insufficient key material");
	  ERR_clear_error ();
	  return;
	}
      DES_set_odd_parity (dc);
    }
}


void
cipher_des_encrypt_ecb (const unsigned char key[8],
    unsigned char *src,
    unsigned char *dst)
{
    des_key_schedule sched;

    des_set_key_unchecked((des_cblock*)key, sched);
    des_ecb_encrypt((des_cblock *)src, (des_cblock *)dst, sched, DES_ENCRYPT);
}

/*
 *
 * Generic message digest information functions
 *
 */


const EVP_MD *
md_kt_get (const char *digest)
{
  const EVP_MD *md = NULL;
  ASSERT (digest);
  md = EVP_get_digestbyname (digest);
  if (!md)
    msg (M_SSLERR, "Message hash algorithm '%s' not found", digest);
  if (EVP_MD_size (md) > MAX_HMAC_KEY_LENGTH)
    msg (M_FATAL, "Message hash algorithm '%s' uses a default hash size (%d bytes) which is larger than " PACKAGE_NAME "'s current maximum hash size (%d bytes)",
	 digest,
	 EVP_MD_size (md),
	 MAX_HMAC_KEY_LENGTH);
  return md;
}

const char *
md_kt_name (const EVP_MD *kt)
{
  if (NULL == kt)
    return "[null-digest]";
  return EVP_MD_name (kt);
}

int
md_kt_size (const EVP_MD *kt)
{
  return EVP_MD_size(kt);
}


/*
 *
 * Generic message digest functions
 *
 */

int
md_full (const EVP_MD *kt, const uint8_t *src, int src_len, uint8_t *dst)
{
  unsigned int in_md_len = 0;

  return EVP_Digest(src, src_len, dst, &in_md_len, kt, NULL);
}

void
md_ctx_init (EVP_MD_CTX *ctx, const EVP_MD *kt)
{
  ASSERT(NULL != ctx && NULL != kt);

  CLEAR (*ctx);

  EVP_MD_CTX_init (ctx);
  EVP_DigestInit(ctx, kt);
}

void
md_ctx_cleanup(EVP_MD_CTX *ctx)
{
  EVP_MD_CTX_cleanup(ctx);
}

int
md_ctx_size (const EVP_MD_CTX *ctx)
{
  return EVP_MD_CTX_size(ctx);
}

void
md_ctx_update (EVP_MD_CTX *ctx, const uint8_t *src, int src_len)
{
  EVP_DigestUpdate(ctx, src, src_len);
}

void
md_ctx_final (EVP_MD_CTX *ctx, uint8_t *dst)
{
  unsigned int in_md_len = 0;

  EVP_DigestFinal(ctx, dst, &in_md_len);
}


