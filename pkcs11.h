/*
 *  OpenVPN -- An application to securely tunnel IP networks
 *             over a single TCP/UDP port, with support for SSL/TLS-based
 *             session authentication and key exchange,
 *             packet encryption, packet authentication, and
 *             packet compression.
 *
 *  Copyright (C) 2002-2005 OpenVPN Solutions LLC <info@openvpn.net>
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

#ifndef OPENVPN_PKCS11_H
#define OPENVPN_PKCS11_H

#if defined(USE_CRYPTO) && defined(USE_SSL) && defined(ENABLE_PKCS11)

int
SSL_CTX_use_pkcs11 (
	SSL_CTX * const ssl_ctx,
	const char * const pkcs11_slot_type,
	const char * const pkcs11_slot,
	const char * const pkcs11_id_type,
	const char * const pkcs11_id,
	const char * const pin,
	const bool pkcs11_protected_authentication
);

void
add_pkcs11 (
	const char * const provider,
	const char * const sign_mode
);

void
free_pkcs11 ();

void
show_pkcs11_slots (
	const int msglev,
	const int warnlev,
	const char * const provider
);

void
show_pkcs11_objects (
	const int msglev,
	const int warnlev,
	const char * const provider,
	const char * const slot,
	const char * const pin
);

#endif

#endif